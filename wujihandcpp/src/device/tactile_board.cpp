#include "wujihandcpp/device/tactile_board.hpp"

#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>

#include "../transport/cdc_transport.hpp"
#include "tactile_cdc_demuxer.hpp"

namespace wujihandcpp {

// USB identifiers for the tactile sensor board (spec §1).
static constexpr uint16_t TACTILE_USB_VID = 0x0483;
static constexpr uint16_t TACTILE_USB_PID = 0x5700;

namespace {

inline uint16_t read_le16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}
inline uint32_t read_le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0])
         | (static_cast<uint32_t>(p[1]) << 8)
         | (static_cast<uint32_t>(p[2]) << 16)
         | (static_cast<uint32_t>(p[3]) << 24);
}
inline uint64_t read_le64(const uint8_t* p) {
    return static_cast<uint64_t>(read_le32(p))
         | (static_cast<uint64_t>(read_le32(p + 4)) << 32);
}
inline void write_le16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>(v >> 8);
}
inline void write_le32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
}
inline void write_le64(uint8_t* p, uint64_t v) {
    write_le32(p, static_cast<uint32_t>(v));
    write_le32(p + 4, static_cast<uint32_t>(v >> 32));
}

/// Trim trailing NULs from a fixed-size ASCII field.
std::string trim_ascii(const uint8_t* p, size_t len) {
    size_t n = 0;
    while (n < len && p[n] != 0) ++n;
    return std::string(reinterpret_cast<const char*>(p), n);
}

}  // namespace

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------

struct TactileBoard::Impl {
    std::string serial_filter;
    std::string tty_path;
    int fd = -1;
    std::atomic<bool> connected{false};

    // Demuxer owns the reader thread and exposes the data-frame queue +
    // command/response channel. Recreated on each connect().
    std::unique_ptr<TactileCdcDemuxer> demuxer;

    // Streaming consumer (one thread that drains demuxer's data queue).
    std::thread streaming_thread;
    std::atomic<bool> streaming{false};
    std::atomic<bool> stop_streaming_requested{false};

    // Disconnect callback is held here so it survives across connect() cycles
    // and gets re-registered on each new demuxer.
    std::mutex disconnect_cb_mu;
    DisconnectCallback disconnect_cb;

    bool open_device() {
        auto devices = cdc::discover_devices(TACTILE_USB_VID, TACTILE_USB_PID);
        if (devices.empty()) return false;

        const cdc::DeviceInfo* chosen = nullptr;
        if (!serial_filter.empty()) {
            for (const auto& d : devices) {
                if (d.serial_number == serial_filter) { chosen = &d; break; }
            }
            if (!chosen) return false;
        } else {
            chosen = &devices[0];
        }

        tty_path = chosen->tty_path;
        fd = cdc::open_cdc(tty_path.c_str());
        if (fd < 0) return false;

        demuxer = std::make_unique<TactileCdcDemuxer>(fd);
        // Re-install disconnect callback on the fresh demuxer.
        {
            std::lock_guard<std::mutex> lock(disconnect_cb_mu);
            if (disconnect_cb) demuxer->set_disconnect_callback(disconnect_cb);
        }
        demuxer->start();
        connected.store(true, std::memory_order_release);
        return true;
    }

    void close_device() {
        connected.store(false, std::memory_order_release);
        if (demuxer) {
            demuxer->stop();
            demuxer.reset();
        }
        if (fd >= 0) {
            close(fd);
            fd = -1;
        }
    }

    void streaming_loop(FrameCallback callback) {
        uint8_t buf[tactile_protocol::FRAME_SIZE];
        while (!stop_streaming_requested.load(std::memory_order_acquire)) {
            if (!demuxer || !demuxer->wait_data_frame(buf, 200)) {
                // Timeout, stop, or disconnect — re-check the flag.
                if (!connected.load(std::memory_order_acquire)) break;
                continue;
            }
            TactileFrame frame = tactile_protocol::parse_frame(buf);
            try {
                callback(frame);
            } catch (...) {
                break;  // user callback raised; exit cleanly
            }
        }
        streaming.store(false, std::memory_order_release);
    }

    /// Issue a command and return its response payload.
    std::vector<uint8_t> command(TactileCmd cmd, const uint8_t* payload, size_t len,
                                 uint32_t timeout_ms = TACTILE_DEFAULT_TIMEOUT_MS) {
        if (!demuxer) throw std::runtime_error("TactileBoard: not connected");
        return demuxer->command(cmd, payload, len, timeout_ms);
    }
};

// ---------------------------------------------------------------------------
// Public API — connection / streaming
// ---------------------------------------------------------------------------

TactileBoard::TactileBoard(const char* serial_number)
    : impl_(std::make_unique<Impl>()) {
    if (serial_number) impl_->serial_filter = serial_number;
}

TactileBoard::~TactileBoard() {
    disconnect();
}

bool TactileBoard::connect() {
    if (impl_->connected.load(std::memory_order_acquire)) return true;
    return impl_->open_device();
}

void TactileBoard::disconnect() {
    stop_streaming();
    impl_->close_device();
}

bool TactileBoard::is_connected() const {
    return impl_->connected.load(std::memory_order_acquire);
}

TactileFrame TactileBoard::read_frame(uint32_t timeout_ms) {
    if (!impl_->connected.load(std::memory_order_acquire))
        throw std::runtime_error("TactileBoard: not connected");
    if (impl_->streaming.load(std::memory_order_acquire))
        throw std::logic_error("TactileBoard: cannot call read_frame() while streaming");

    uint8_t buf[tactile_protocol::FRAME_SIZE];
    if (!impl_->demuxer->wait_data_frame(buf, timeout_ms)) {
        if (!impl_->connected.load(std::memory_order_acquire))
            throw ConnectionLostError("USB CDC device disconnected");
        throw std::runtime_error("TactileBoard: read_frame timeout");
    }
    return tactile_protocol::parse_frame(buf);
}

void TactileBoard::start_streaming(FrameCallback callback) {
    if (!impl_->connected.load(std::memory_order_acquire))
        throw std::runtime_error("TactileBoard: not connected");
    if (impl_->streaming.load(std::memory_order_acquire))
        throw std::logic_error("TactileBoard: already streaming");

    if (impl_->streaming_thread.joinable()) impl_->streaming_thread.join();

    impl_->stop_streaming_requested.store(false, std::memory_order_release);
    impl_->streaming.store(true, std::memory_order_release);
    try {
        impl_->streaming_thread = std::thread(&Impl::streaming_loop, impl_.get(),
                                              std::move(callback));
    } catch (...) {
        impl_->streaming.store(false, std::memory_order_release);
        throw;
    }
}

void TactileBoard::stop_streaming() {
    impl_->stop_streaming_requested.store(true, std::memory_order_release);
    if (impl_->streaming_thread.joinable()) impl_->streaming_thread.join();
    impl_->streaming.store(false, std::memory_order_release);
}

void TactileBoard::set_disconnect_callback(DisconnectCallback callback) {
    {
        std::lock_guard<std::mutex> lock(impl_->disconnect_cb_mu);
        impl_->disconnect_cb = callback;
    }
    if (impl_->demuxer) impl_->demuxer->set_disconnect_callback(std::move(callback));
}

// ---------------------------------------------------------------------------
// Public API — commands (spec §3)
// ---------------------------------------------------------------------------

TactileDeviceInfo TactileBoard::get_device_info() {
    auto resp = impl_->command(TactileCmd::GetDeviceInfo, nullptr, 0);
    if (resp.size() != 32) {
        throw std::runtime_error("get_device_info: unexpected payload size");
    }
    TactileDeviceInfo info;
    info.serial = trim_ascii(resp.data(), 24);
    std::memcpy(info.hw_revision.data(), resp.data() + 24, 4);
    std::memcpy(info.fw_version.data(), resp.data() + 28, 4);
    return info;
}

TactileFwBuild TactileBoard::get_fw_build() {
    auto resp = impl_->command(TactileCmd::GetFwBuild, nullptr, 0);
    if (resp.size() != 8) {
        throw std::runtime_error("get_fw_build: unexpected payload size");
    }
    TactileFwBuild build;
    build.git_short_sha = trim_ascii(resp.data(), 8);
    return build;
}

TactileHandedness TactileBoard::get_handedness() {
    auto resp = impl_->command(TactileCmd::GetHandedness, nullptr, 0);
    if (resp.size() != 1) {
        throw std::runtime_error("get_handedness: unexpected payload size");
    }
    return static_cast<TactileHandedness>(resp[0]);
}

TactileDiagnostics TactileBoard::get_diagnostics() {
    auto resp = impl_->command(TactileCmd::GetDiagnostics, nullptr, 0);
    if (resp.size() != 18) {
        throw std::runtime_error("get_diagnostics: unexpected payload size");
    }
    TactileDiagnostics d;
    d.uptime_ms       = read_le32(resp.data() + 0);
    d.frame_count     = read_le32(resp.data() + 4);
    d.crc_err_count   = read_le32(resp.data() + 8);
    d.dropout_count   = read_le32(resp.data() + 12);
    d.usb_reset_count = read_le16(resp.data() + 16);
    return d;
}

void TactileBoard::reset_counters() {
    impl_->command(TactileCmd::ResetCounters, nullptr, 0);
}

void TactileBoard::set_streaming(bool enable) {
    uint8_t payload = enable ? 1 : 0;
    impl_->command(TactileCmd::SetStreaming, &payload, 1);
}

void TactileBoard::reset_device() {
    // Device re-enumerates after sending OK; treat any read failure as success
    // because the response may be lost in the disconnect race. Use a short
    // timeout to avoid blocking the caller after the jump.
    try {
        impl_->command(TactileCmd::Reset, nullptr, 0, 100);
    } catch (const std::runtime_error&) {
        // Expected: device may have reset before we read the reply.
    }
}

void TactileBoard::enter_bootloader(uint32_t magic) {
    uint8_t payload[4];
    write_le32(payload, magic);
    try {
        impl_->command(TactileCmd::EnterBootloader, payload, 4, 100);
    } catch (const TactileError&) {
        // Magic mismatch is the only meaningful failure (BadPayload). Surface it.
        throw;
    } catch (const std::runtime_error&) {
        // Device jumped before reply arrived — that's success.
    }
}

uint16_t TactileBoard::get_sample_rate_hz() {
    uint8_t req[2];
    write_le16(req, static_cast<uint16_t>(TactileConfigKey::SampleRateHz));
    auto resp = impl_->command(TactileCmd::GetConfig, req, 2);
    if (resp.size() != 3 || resp[0] != static_cast<uint8_t>(TactileConfigType::U16)) {
        throw std::runtime_error("get_sample_rate_hz: malformed response");
    }
    return read_le16(resp.data() + 1);
}

void TactileBoard::set_sample_rate_hz(uint16_t hz) {
    uint8_t req[5];
    write_le16(req, static_cast<uint16_t>(TactileConfigKey::SampleRateHz));
    req[2] = static_cast<uint8_t>(TactileConfigType::U16);
    write_le16(req + 3, hz);
    impl_->command(TactileCmd::SetConfig, req, 5);
}

bool TactileBoard::get_streaming_enabled() {
    uint8_t req[2];
    write_le16(req, static_cast<uint16_t>(TactileConfigKey::StreamingEnabled));
    auto resp = impl_->command(TactileCmd::GetConfig, req, 2);
    if (resp.size() != 2 || resp[0] != static_cast<uint8_t>(TactileConfigType::EnumU8)) {
        throw std::runtime_error("get_streaming_enabled: malformed response");
    }
    return resp[1] != 0;
}

TactileDeviceTime TactileBoard::get_device_time() {
    auto resp = impl_->command(TactileCmd::GetDeviceTime, nullptr, 0);
    if (resp.size() != 8) {
        throw std::runtime_error("get_device_time: unexpected payload size");
    }
    return TactileDeviceTime{read_le64(resp.data())};
}

TactileSyncResult TactileBoard::sync_host_epoch(uint64_t host_unix_ns) {
    uint8_t req[8];
    write_le64(req, host_unix_ns);
    auto resp = impl_->command(TactileCmd::SyncHostEpoch, req, 8);
    if (resp.size() != 16) {
        throw std::runtime_error("sync_host_epoch: unexpected payload size");
    }
    TactileSyncResult r;
    r.device_ns_at_sync = read_le64(resp.data());
    r.host_ns_echo      = read_le64(resp.data() + 8);
    return r;
}

}  // namespace wujihandcpp
