#include "frame_demuxer.hpp"

#include <chrono>
#include <cstring>

namespace wujihandcpp {
namespace tactile {

namespace {

// Reader-side polling timeout: short enough to react to stop_requested
// quickly without burning CPU. The underlying byte stream doesn't have
// a true async notification path, so we poll its read() with this
// short deadline and loop back to the stop-flag check.
constexpr uint32_t READER_POLL_MS = 100;

// Spec §2.2: total frame ≤ 512 B AND request payload > 500 B is illegal.
// The two constraints overlap with 2 B of cushion (frame_max - overhead = 502),
// so the binding limit is 500 B. Hard-coded explicitly to track the spec value.
constexpr size_t MAX_REQUEST_PAYLOAD = 500;

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

FrameDemuxer::FrameDemuxer(std::shared_ptr<transport::IByteStream> stream)
    : stream_(std::move(stream)) {}

FrameDemuxer::~FrameDemuxer() {
    stop();
    // The byte stream is dropped (and any underlying fd closed) when the
    // last shared_ptr<IByteStream> ref goes — typically right here when
    // we drop our `stream_`, unless the reader thread was self-detached
    // and still holds a ref via `self->stream_`.
}

void FrameDemuxer::start() {
    // Keep the demuxer alive until reader_loop exits, including the
    // self-detach teardown path from inside the reader thread.
    auto self = shared_from_this();
    thread_ = std::thread([self]() { self->reader_loop(); });
}

void FrameDemuxer::stop() {
    // At most one already-started write can complete during teardown.
    // Taking command_mu_ to prevent it would block stop() up to one
    // command timeout (~2 s) for a benign edge case.
    stop_requested_.store(true, std::memory_order_release);
    // Take the predicate mutexes briefly before notifying. Without this, a
    // waiter that evaluated its predicate just before stop_requested_ was
    // set, but that hasn't yet added itself to the CV wait queue, would
    // miss the notification and sleep until its timeout.
    { std::lock_guard<std::mutex> lock(frame_mu_); frame_cv_.notify_all(); }
    { std::lock_guard<std::mutex> lock(pending_mu_); pending_cv_.notify_all(); }
    if (thread_.joinable()) {
        if (thread_.get_id() == std::this_thread::get_id()) {
            // Called from inside the reader thread (typically because a
            // user disconnect callback synchronously tore down the SDK
            // handle). Joining ourselves would std::terminate; detach and
            // let the reader loop unwind naturally. The shared_ptr<self>
            // captured in start() keeps *this alive until reader_loop
            // returns, after which the last ref drops and ~FrameDemuxer
            // releases its IByteStream ref (the underlying fd / handle
            // is closed by the stream's destructor).
            thread_.detach();
        } else {
            thread_.join();
        }
    }
}

void FrameDemuxer::set_disconnect_callback(DisconnectCallback cb) {
    std::lock_guard<std::mutex> lock(disconnect_cb_mu_);
    disconnect_cb_ = std::move(cb);
}

bool FrameDemuxer::wait_data_frame(uint8_t out[protocol::FRAME_SIZE],
                                 uint32_t timeout_ms) {
    std::unique_lock<std::mutex> lock(frame_mu_);
    bool got = frame_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
        [this]() {
            return !frame_queue_.empty()
                || stop_requested_.load(std::memory_order_acquire)
                || disconnected_.load(std::memory_order_acquire);
        });
    if (!got || frame_queue_.empty()) return false;
    std::memcpy(out, frame_queue_.front().data(), protocol::FRAME_SIZE);
    frame_queue_.pop_front();
    return true;
}

// Single retry on BadCrc per spec §2.4. Other statuses surface to the caller.
std::vector<uint8_t>
FrameDemuxer::command_with_bad_crc_retry(Cmd cmd, const uint8_t* payload,
                                         size_t payload_len, uint32_t timeout_ms) {
    try {
        return single_command(cmd, payload, payload_len, timeout_ms);
    } catch (const Error& e) {
        if (e.status() != Status::BadCrc) throw;
        return single_command(cmd, payload, payload_len, timeout_ms);
    }
}

std::vector<uint8_t> FrameDemuxer::command(Cmd cmd, const uint8_t* payload,
                                           size_t payload_len, uint32_t timeout_ms) {
    // Spec §2.4: requests are strictly serial.
    std::lock_guard<std::mutex> serial(command_mu_);
    return command_with_bad_crc_retry(cmd, payload, payload_len, timeout_ms);
}

// try_command yields when the serializer is busy — used by pollers (e.g. the
// ROS diagnostics timer) so they don't queue behind user-issued commands.
std::optional<std::vector<uint8_t>>
FrameDemuxer::try_command(Cmd cmd, const uint8_t* payload,
                          size_t payload_len, uint32_t timeout_ms) {
    std::unique_lock<std::mutex> serial(command_mu_, std::try_to_lock);
    if (!serial.owns_lock()) return std::nullopt;
    return command_with_bad_crc_retry(cmd, payload, payload_len, timeout_ms);
}

std::vector<uint8_t> FrameDemuxer::single_command(Cmd cmd, const uint8_t* payload,
                                                size_t payload_len, uint32_t timeout_ms) {
    if (payload_len > MAX_REQUEST_PAYLOAD) {
        throw std::runtime_error("FrameDemuxer: command payload too large");
    }
    // Short-circuit if the channel is already torn down — a stale demuxer
    // snapshot must not send bytes after the lifecycle owner closed it.
    if (is_closed()) {
        throw NotConnectedError("FrameDemuxer: channel closed");
    }

    // Build command frame (spec §2.2): sync(2) + length(2) + cmd(2) + seq(2)
    // + payload(N) + crc(2). CRC covers bytes [2, 8+N).
    uint8_t buf[FRAME_MAX];
    const uint16_t length = static_cast<uint16_t>(10 + payload_len);
    const uint16_t seq = next_seq_.fetch_add(1, std::memory_order_relaxed);

    buf[0] = 0xAA;
    buf[1] = 0x56;
    write_le16(buf + 2, length);
    write_le16(buf + 4, static_cast<uint16_t>(cmd));
    write_le16(buf + 6, seq);
    if (payload_len > 0) std::memcpy(buf + 8, payload, payload_len);
    uint16_t crc = protocol::crc16_ccitt(buf + 2, 6 + payload_len);
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

    // Share one absolute deadline between write and response wait so the
    // per-command worst case is timeout_ms, not 2 × timeout_ms. A short
    // write cannot form a valid wire frame, so it's treated as failure.
    auto deadline = std::chrono::steady_clock::now()
                    + std::chrono::milliseconds(timeout_ms);
    if (stream_->write(buf, length, timeout_ms)
        != static_cast<ssize_t>(length)) {
        std::lock_guard<std::mutex> lock(pending_mu_);
        pending_active_ = false;
        throw WriteFailedError(
            "FrameDemuxer: write failed (timeout / device disconnected)");
    }

    // Wait for the matching response within the remaining budget. If
    // write_exact ate the entire budget (rare — the write is 16 bytes
    // and a healthy device drains it in microseconds), `remaining` may
    // be zero or slightly negative, in which case wait_for treats it
    // as already-timed-out and returns immediately.
    auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                        deadline - std::chrono::steady_clock::now());
    if (remaining.count() < 0) remaining = std::chrono::milliseconds(0);
    std::unique_lock<std::mutex> lock(pending_mu_);
    bool got = pending_cv_.wait_for(lock, remaining,
        [this]() {
            return pending_filled_
                || disconnected_.load(std::memory_order_acquire)
                || stop_requested_.load(std::memory_order_acquire);
        });
    pending_active_ = false;

    // Distinguish "device dropped during request" from "lifecycle owner
    // tore the channel down" so callers can react differently (e.g. retry
    // after reconnect vs. surface the teardown).
    if (disconnected_.load(std::memory_order_acquire)) {
        throw DisconnectedDuringRequestError(
            "FrameDemuxer: disconnected while awaiting response");
    }
    if (stop_requested_.load(std::memory_order_acquire) && !pending_filled_) {
        throw NotConnectedError(
            "FrameDemuxer: channel closed while awaiting response");
    }
    if (!got || !pending_filled_) {
        throw ResponseTimeoutError("FrameDemuxer: response timeout");
    }

    Status status = pending_status_;
    std::vector<uint8_t> payload_out = std::move(pending_payload_);
    pending_payload_.clear();

    if (status != Status::Ok) {
        throw Error(status, "tactile command returned non-Ok status: "
                                   + to_string(status));
    }
    return payload_out;
}

void FrameDemuxer::handle_data_frame(const uint8_t* buf) {
    std::lock_guard<std::mutex> lock(frame_mu_);
    if (frame_queue_.size() >= MAX_QUEUE) frame_queue_.pop_front();
    frame_queue_.emplace_back();
    std::memcpy(frame_queue_.back().data(), buf, protocol::FRAME_SIZE);
    frame_cv_.notify_one();
}

void FrameDemuxer::handle_response_frame(const uint8_t* buf, uint16_t length) {
    // Layout per spec §2.3:
    //   2..4 length, 4..6 cmd_id, 6..8 seq, 8 status, 9..(L-2) payload, (L-2)..L crc.
    if (length < RESPONSE_FIXED_OVERHEAD) return;  // malformed; drop
    uint16_t crc_pos = length - 2;
    uint16_t resp_cmd_raw = read_le16(buf + 4);
    uint16_t resp_seq = read_le16(buf + 6);
    uint16_t expected_crc = read_le16(buf + crc_pos);
    uint16_t computed_crc = protocol::crc16_ccitt(buf + 2, crc_pos - 2);

    if (expected_crc != computed_crc) {
        // Surface as BadCrc so the in-flight caller can retry. Match BOTH
        // cmd_id and seq — a stale corrupted echo from a prior command that
        // happens to share `seq` (after u16 wrap) must not poison a newer
        // unrelated command's wait.
        std::lock_guard<std::mutex> lock(pending_mu_);
        if (pending_active_
            && resp_seq == pending_seq_
            && resp_cmd_raw == static_cast<uint16_t>(pending_cmd_)) {
            pending_status_ = Status::BadCrc;
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
    pending_status_ = static_cast<Status>(status_byte);
    pending_payload_.assign(buf + 9, buf + 9 + payload_len);
    pending_filled_ = true;
    pending_cv_.notify_one();
}

bool FrameDemuxer::read_drain(size_t count, uint32_t timeout_ms) {
    if (count == 0) return true;
    uint8_t scratch[512];
    while (count > 0) {
        size_t chunk = count > sizeof(scratch) ? sizeof(scratch) : count;
        ssize_t n = stream_->read(scratch, chunk, timeout_ms);
        if (n < 0) {
            handle_disconnect();
            return false;
        }
        if (static_cast<size_t>(n) < chunk) return false;  // timeout mid-drain
        count -= static_cast<size_t>(n);
    }
    return true;
}

void FrameDemuxer::handle_disconnect() {
    // disconnected_.exchange(true) gates everything below to fire exactly
    // once, even if multiple read paths see EIO concurrently.
    if (disconnected_.exchange(true, std::memory_order_acq_rel)) return;
    // Same predicate-mutex dance as stop(): hold each mutex briefly so a
    // waiter that just evaluated its predicate cannot miss this wakeup.
    { std::lock_guard<std::mutex> lock(frame_mu_); frame_cv_.notify_all(); }
    { std::lock_guard<std::mutex> lock(pending_mu_); pending_cv_.notify_all(); }
    DisconnectCallback cb;
    {
        std::lock_guard<std::mutex> lock(disconnect_cb_mu_);
        cb = disconnect_cb_;
    }
    if (cb) {
        try { cb(); } catch (...) {}
    }
}

// Read `len` bytes; on disconnect tear down the demuxer and return false.
// `partial` reports a short read (timeout mid-frame) so the caller can resync
// without conflating it with a wire-level failure.
bool FrameDemuxer::read_exact_or_disconnect(uint8_t* dst, size_t len,
                                            uint32_t timeout_ms, bool& partial) {
    ssize_t got = stream_->read(dst, len, timeout_ms);
    if (got < 0) { handle_disconnect(); return false; }
    partial = (got != static_cast<ssize_t>(len));
    return true;
}

void FrameDemuxer::reader_loop() {
    uint8_t prev = 0;
    while (!stop_requested_.load(std::memory_order_acquire)
           && !disconnected_.load(std::memory_order_acquire)) {

        uint8_t b;
        bool partial;
        if (!read_exact_or_disconnect(&b, 1, READER_POLL_MS, partial)) return;
        if (partial) { prev = 0; continue; }  // timeout — re-check stop flag

        if (prev != 0xAA) { prev = b; continue; }

        if (b == 0x55) {
            FrameBuf buf;
            buf[0] = 0xAA; buf[1] = 0x55;
            if (!read_exact_or_disconnect(buf.data() + 2,
                                          protocol::FRAME_SIZE - 2, 200, partial)) return;
            prev = 0;
            if (partial) continue;

            uint16_t length = read_le16(buf.data() + protocol::OFFSET_LENGTH);
            if (length != protocol::EXPECTED_LENGTH) continue;
            uint16_t expected_crc = read_le16(buf.data() + protocol::OFFSET_CRC);
            uint16_t computed_crc = protocol::crc16_ccitt(
                buf.data() + protocol::OFFSET_LENGTH,
                protocol::OFFSET_CRC - protocol::OFFSET_LENGTH);
            if (expected_crc != computed_crc) continue;
            handle_data_frame(buf.data());
        } else if (b == 0x57) {
            uint8_t buf[FRAME_MAX];
            buf[0] = 0xAA; buf[1] = 0x57;
            if (!read_exact_or_disconnect(buf + 2, 2, 200, partial)) return;
            prev = 0;
            if (partial) continue;
            uint16_t length = read_le16(buf + 2);
            if (length < RESPONSE_FIXED_OVERHEAD || length > FRAME_MAX) continue;
            if (!read_exact_or_disconnect(buf + 4, length - 4, 200, partial)) return;
            if (partial) continue;
            handle_response_frame(buf, length);
        } else if (b == 0x56) {
            // AA56 = host-to-device echo on RX; drain by length to resync.
            uint8_t len_buf[2];
            if (!read_exact_or_disconnect(len_buf, 2, 200, partial)) return;
            prev = 0;
            if (partial) continue;
            uint16_t length = read_le16(len_buf);
            if (length < 4 || length > FRAME_MAX) continue;
            (void)read_drain(length - 4, 200);
        } else {
            // 0xAA followed by unknown byte; slide the window.
            prev = b;
        }
    }
}

}  // namespace tactile
}  // namespace wujihandcpp
