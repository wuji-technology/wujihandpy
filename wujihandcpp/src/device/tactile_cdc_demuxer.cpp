#include "tactile_cdc_demuxer.hpp"

#include <chrono>
#include <cstring>
#include <unistd.h>

#include "../transport/cdc_transport.hpp"

namespace wujihandcpp {

namespace {

// Reader-side polling timeout: short enough to react to stop_requested
// quickly without burning CPU. The CDC fd doesn't have a true async
// notification path, so we poll in cdc::read_exact.
constexpr uint32_t READER_POLL_MS = 100;

// Maximum command frame payload size after subtracting sync+length+cmd_id
// +seq+crc = 10 bytes (per spec §2.2).
constexpr size_t MAX_REQUEST_PAYLOAD = TACTILE_FRAME_MAX - 10;

// Response frame fixed overhead (sync 2 + length 2 + cmd_id 2 + seq 2 +
// status 1 + crc 2 = 11) per spec §2.3.
constexpr size_t RESPONSE_FIXED_OVERHEAD = 11;

inline uint16_t read_le16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

inline void write_le16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>(v >> 8);
}

}  // namespace

TactileCdcDemuxer::TactileCdcDemuxer(int fd) : fd_(fd) {}

TactileCdcDemuxer::~TactileCdcDemuxer() {
    stop();
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

void TactileCdcDemuxer::start() {
    stop_requested_.store(false, std::memory_order_release);
    disconnected_.store(false, std::memory_order_release);
    disconnect_cb_fired_.store(false, std::memory_order_release);
    thread_ = std::thread(&TactileCdcDemuxer::reader_loop, this);
}

void TactileCdcDemuxer::stop() {
    stop_requested_.store(true, std::memory_order_release);

    // Wake any data-frame consumer.
    {
        std::lock_guard<std::mutex> lock(frame_mu_);
        frame_cv_.notify_all();
    }
    // Wake any in-flight command waiter.
    {
        std::lock_guard<std::mutex> lock(pending_mu_);
        pending_cv_.notify_all();
    }
    if (thread_.joinable()) thread_.join();
}

void TactileCdcDemuxer::set_disconnect_callback(DisconnectCallback cb) {
    std::lock_guard<std::mutex> lock(disconnect_cb_mu_);
    disconnect_cb_ = std::move(cb);
}

bool TactileCdcDemuxer::wait_data_frame(uint8_t out[tactile_protocol::FRAME_SIZE],
                                        uint32_t timeout_ms) {
    std::unique_lock<std::mutex> lock(frame_mu_);
    bool got = frame_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
        [this]() {
            return !frame_queue_.empty()
                || stop_requested_.load(std::memory_order_acquire)
                || disconnected_.load(std::memory_order_acquire);
        });
    if (!got || frame_queue_.empty()) return false;
    std::memcpy(out, frame_queue_.front().data(), tactile_protocol::FRAME_SIZE);
    frame_queue_.pop_front();
    return true;
}

std::vector<uint8_t> TactileCdcDemuxer::command(TactileCmd cmd, const uint8_t* payload,
                                                size_t payload_len, uint32_t timeout_ms) {
    // Spec §2.4: requests are strictly serial. Hold this mutex through
    // build → send → wait so concurrent callers queue.
    std::lock_guard<std::mutex> serial(command_mu_);

    // Single retry on BadCrc, surfacing every other status to the caller.
    try {
        return single_command(cmd, payload, payload_len, timeout_ms);
    } catch (const TactileError& e) {
        if (e.status() == TactileStatus::BadCrc) {
            return single_command(cmd, payload, payload_len, timeout_ms);
        }
        throw;
    }
}

std::vector<uint8_t> TactileCdcDemuxer::single_command(TactileCmd cmd, const uint8_t* payload,
                                                       size_t payload_len, uint32_t timeout_ms) {
    if (payload_len > MAX_REQUEST_PAYLOAD) {
        throw std::runtime_error("TactileCdcDemuxer: command payload too large");
    }
    if (disconnected_.load(std::memory_order_acquire)) {
        throw TactileNotConnectedError("TactileCdcDemuxer: device disconnected");
    }

    // Build command frame (spec §2.2): sync(2) + length(2) + cmd(2) + seq(2)
    // + payload(N) + crc(2). CRC covers bytes [2, 8+N).
    uint8_t buf[TACTILE_FRAME_MAX];
    const uint16_t length = static_cast<uint16_t>(10 + payload_len);
    const uint16_t seq = next_seq_.fetch_add(1, std::memory_order_relaxed);

    buf[0] = 0xAA;
    buf[1] = 0x56;
    write_le16(buf + 2, length);
    write_le16(buf + 4, static_cast<uint16_t>(cmd));
    write_le16(buf + 6, seq);
    if (payload_len > 0) std::memcpy(buf + 8, payload, payload_len);
    uint16_t crc = tactile_protocol::crc16_ccitt(buf + 2, 6 + payload_len);
    write_le16(buf + 8 + payload_len, crc);

    // Arm the in-flight slot before sending so the reader can deliver as
    // soon as the response lands. Match on (seq, cmd_id) — seq alone wraps
    // every 65535 commands and a delayed stale response could otherwise
    // satisfy the wrong waiter (~109 min at 10 Hz of diagnostics polling).
    {
        std::lock_guard<std::mutex> lock(pending_mu_);
        pending_active_ = true;
        pending_seq_ = seq;
        pending_cmd_ = cmd;
        pending_filled_ = false;
        pending_payload_.clear();
    }

    if (cdc::write_exact(fd_, buf, length) != static_cast<ssize_t>(length)) {
        std::lock_guard<std::mutex> lock(pending_mu_);
        pending_active_ = false;
        throw TactileWriteFailedError(
            "TactileCdcDemuxer: write failed (device disconnected?)");
    }

    // Wait for the matching response.
    std::unique_lock<std::mutex> lock(pending_mu_);
    bool got = pending_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
        [this]() {
            return pending_filled_
                || disconnected_.load(std::memory_order_acquire)
                || stop_requested_.load(std::memory_order_acquire);
        });
    pending_active_ = false;

    if (disconnected_.load(std::memory_order_acquire)) {
        throw TactileDisconnectedDuringRequestError(
            "TactileCdcDemuxer: disconnected while awaiting response");
    }
    if (!got || !pending_filled_) {
        throw TactileResponseTimeoutError("TactileCdcDemuxer: response timeout");
    }

    TactileStatus status = pending_status_;
    std::vector<uint8_t> payload_out = std::move(pending_payload_);
    pending_payload_.clear();

    if (status != TactileStatus::Ok) {
        throw TactileError(status, "tactile command returned non-Ok status: 0x"
                                   + std::to_string(static_cast<int>(status)));
    }
    return payload_out;
}

void TactileCdcDemuxer::handle_data_frame(const uint8_t* buf) {
    std::lock_guard<std::mutex> lock(frame_mu_);
    if (frame_queue_.size() >= MAX_QUEUE) frame_queue_.pop_front();
    frame_queue_.emplace_back();
    std::memcpy(frame_queue_.back().data(), buf, tactile_protocol::FRAME_SIZE);
    frame_cv_.notify_one();
}

void TactileCdcDemuxer::handle_response_frame(const uint8_t* buf, uint16_t length) {
    // Layout per spec §2.3:
    //   2..4 length, 4..6 cmd_id, 6..8 seq, 8 status, 9..(L-2) payload, (L-2)..L crc.
    if (length < RESPONSE_FIXED_OVERHEAD) return;  // malformed; drop
    uint16_t crc_pos = length - 2;
    uint16_t resp_cmd_raw = read_le16(buf + 4);
    uint16_t resp_seq = read_le16(buf + 6);
    uint16_t expected_crc = read_le16(buf + crc_pos);
    uint16_t computed_crc = tactile_protocol::crc16_ccitt(buf + 2, crc_pos - 2);

    if (expected_crc != computed_crc) {
        // Surface as BadCrc so the in-flight caller can retry. Match BOTH
        // cmd_id and seq — a stale corrupted echo from a prior command that
        // happens to share `seq` (after u16 wrap) must not poison a newer
        // unrelated command's wait.
        std::lock_guard<std::mutex> lock(pending_mu_);
        if (pending_active_
            && resp_seq == pending_seq_
            && resp_cmd_raw == static_cast<uint16_t>(pending_cmd_)) {
            pending_status_ = TactileStatus::BadCrc;
            pending_payload_.clear();
            pending_filled_ = true;
            pending_cv_.notify_one();
        }
        return;
    }

    uint8_t status_byte = buf[8];
    size_t payload_len = static_cast<size_t>(crc_pos) - 9;

    std::lock_guard<std::mutex> lock(pending_mu_);
    if (!pending_active_
        || resp_seq != pending_seq_
        || resp_cmd_raw != static_cast<uint16_t>(pending_cmd_)) {
        return;  // stale / unsolicited / cmd_id mismatch
    }
    pending_status_ = static_cast<TactileStatus>(status_byte);
    pending_payload_.assign(buf + 9, buf + 9 + payload_len);
    pending_filled_ = true;
    pending_cv_.notify_one();
}

bool TactileCdcDemuxer::read_drain(size_t count, uint32_t timeout_ms) {
    if (count == 0) return true;
    uint8_t scratch[512];
    while (count > 0) {
        size_t chunk = count > sizeof(scratch) ? sizeof(scratch) : count;
        ssize_t n = cdc::read_exact(fd_, scratch, chunk, timeout_ms);
        if (n < 0) {
            handle_disconnect();
            return false;
        }
        if (static_cast<size_t>(n) < chunk) return false;  // timeout mid-drain
        count -= static_cast<size_t>(n);
    }
    return true;
}

void TactileCdcDemuxer::handle_disconnect() {
    if (disconnected_.exchange(true, std::memory_order_acq_rel)) return;
    {
        std::lock_guard<std::mutex> lock(frame_mu_);
        frame_cv_.notify_all();
    }
    {
        std::lock_guard<std::mutex> lock(pending_mu_);
        pending_cv_.notify_all();
    }
    if (!disconnect_cb_fired_.exchange(true, std::memory_order_acq_rel)) {
        DisconnectCallback cb;
        {
            std::lock_guard<std::mutex> lock(disconnect_cb_mu_);
            cb = disconnect_cb_;
        }
        if (cb) {
            try { cb(); } catch (...) {}
        }
    }
}

void TactileCdcDemuxer::reader_loop() {
    uint8_t prev = 0;
    while (!stop_requested_.load(std::memory_order_acquire)
           && !disconnected_.load(std::memory_order_acquire)) {

        uint8_t b;
        ssize_t n = cdc::read_exact(fd_, &b, 1, READER_POLL_MS);
        if (n < 0) { handle_disconnect(); return; }
        if (n == 0) { prev = 0; continue; }  // timeout — loop back to check stop flag

        if (prev != 0xAA) { prev = b; continue; }

        if (b == 0x55) {
            // Data frame: read remaining FRAME_SIZE-2 bytes.
            FrameBuf buf;
            buf[0] = 0xAA; buf[1] = 0x55;
            ssize_t got = cdc::read_exact(fd_, buf.data() + 2,
                                          tactile_protocol::FRAME_SIZE - 2, 200);
            if (got < 0) { handle_disconnect(); return; }
            prev = 0;
            if (got != static_cast<ssize_t>(tactile_protocol::FRAME_SIZE - 2)) continue;

            uint16_t length = read_le16(buf.data() + tactile_protocol::OFFSET_LENGTH);
            if (length != tactile_protocol::EXPECTED_LENGTH) continue;  // false header
            uint16_t expected_crc = read_le16(buf.data() + tactile_protocol::OFFSET_CRC);
            uint16_t computed_crc = tactile_protocol::crc16_ccitt(
                buf.data() + tactile_protocol::OFFSET_LENGTH,
                tactile_protocol::OFFSET_CRC - tactile_protocol::OFFSET_LENGTH);
            if (expected_crc != computed_crc) continue;
            handle_data_frame(buf.data());
        } else if (b == 0x57) {
            // Response frame: read length(2), then drain payload+crc into a buf.
            uint8_t buf[TACTILE_FRAME_MAX];
            buf[0] = 0xAA; buf[1] = 0x57;
            ssize_t got = cdc::read_exact(fd_, buf + 2, 2, 200);
            if (got < 0) { handle_disconnect(); return; }
            prev = 0;
            if (got != 2) continue;
            uint16_t length = read_le16(buf + 2);
            if (length < RESPONSE_FIXED_OVERHEAD || length > TACTILE_FRAME_MAX) continue;
            ssize_t rest = cdc::read_exact(fd_, buf + 4, length - 4, 200);
            if (rest < 0) { handle_disconnect(); return; }
            if (rest != static_cast<ssize_t>(length - 4)) continue;
            handle_response_frame(buf, length);
        } else if (b == 0x56) {
            // Host-to-device sync; not expected on RX. Drain by length to resync.
            uint8_t len_buf[2];
            ssize_t got = cdc::read_exact(fd_, len_buf, 2, 200);
            if (got < 0) { handle_disconnect(); return; }
            prev = 0;
            if (got != 2) continue;
            uint16_t length = read_le16(len_buf);
            if (length < 4 || length > TACTILE_FRAME_MAX) continue;
            (void)read_drain(length - 4, 200);
        } else {
            // 0xAA followed by an unknown byte: slide window; if the new byte
            // is itself 0xAA it might start a new sync.
            prev = b;
        }
    }
}

}  // namespace wujihandcpp
