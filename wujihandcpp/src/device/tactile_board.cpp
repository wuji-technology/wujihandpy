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

    // `connected` is atomic so streaming_loop and is_connected() can read it
    // without taking lifecycle_mu_ — flips false the moment the device drops.
    std::atomic<bool> connected{false};

    // Lifecycle mutex protects mutations of `demuxer` and `streaming_thread`,
    // and serializes connect()/disconnect()/start_streaming()/stop_streaming().
    // Held only briefly: command()/streaming_loop snapshot the demuxer
    // shared_ptr under it then drop the lock before doing blocking I/O so a
    // concurrent disconnect() never waits on a 2-second command.
    std::mutex lifecycle_mu;

    // Demuxer owns the fd and the reader thread. shared_ptr (not unique_ptr)
    // because in-flight command() / streaming_loop calls take their own refs
    // and may outlive an interleaved disconnect() — the demuxer destructor
    // (which close()s the fd) only fires after the last ref drops.
    std::shared_ptr<TactileCdcDemuxer> demuxer;

    // Streaming consumer (one thread that drains demuxer's data queue).
    std::thread streaming_thread;
    std::atomic<bool> streaming{false};
    std::atomic<bool> stop_streaming_requested{false};

    // User-facing disconnect callback. Held here so it survives across
    // connect() cycles and is re-installed (wrapped) on each new demuxer.
    std::mutex disconnect_cb_mu;
    DisconnectCallback disconnect_cb;

    /// MUST be called with `lifecycle_mu` held.
    bool open_device_locked() {
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
        int fd = cdc::open_cdc(tty_path.c_str());
        if (fd < 0) return false;

        auto new_demuxer = std::make_shared<TactileCdcDemuxer>(fd);

        // Internal disconnect wrapper: flip `connected` BEFORE invoking the
        // user callback so anything checking is_connected() (including
        // streaming_loop) sees the new state immediately. Captures `this`,
        // which is safe — Impl outlives any demuxer it ever held (Impl is
        // owned by TactileBoard via unique_ptr; demuxer cannot be invoked
        // after TactileBoard is destroyed because TactileBoard's destructor
        // calls disconnect() which drops the demuxer ref and waits for the
        // reader thread to exit).
        new_demuxer->set_disconnect_callback([this]() {
            connected.store(false, std::memory_order_release);
            DisconnectCallback cb;
            {
                std::lock_guard<std::mutex> lock(disconnect_cb_mu);
                cb = disconnect_cb;
            }
            if (cb) {
                try { cb(); } catch (...) {}
            }
        });
        new_demuxer->start();
        demuxer = std::move(new_demuxer);
        connected.store(true, std::memory_order_release);
        return true;
    }

    /// Snapshot the demuxer under `lifecycle_mu`. Returns null if not
    /// connected. Caller does blocking I/O on the snapshot WITHOUT the lock,
    /// so a concurrent disconnect() does not wait on the command.
    std::shared_ptr<TactileCdcDemuxer> snapshot_demuxer() {
        std::lock_guard<std::mutex> lock(lifecycle_mu);
        return demuxer;
    }

    void streaming_loop(std::shared_ptr<TactileCdcDemuxer> dx,
                        FrameCallback callback) {
        uint8_t buf[tactile_protocol::FRAME_SIZE];
        while (!stop_streaming_requested.load(std::memory_order_acquire)) {
            // Three independent shutdown signals are checked here:
            //   - is_closed(): the demuxer this thread was started on has
            //     been torn down (e.g. disconnect()→reconnect() created a
            //     fresh demuxer; we, the OLD thread, must exit so we do not
            //     burn CPU spinning on a closed channel)
            //   - !connected: the board is not connected (handles the case
            //     where the wakeup was an EIO)
            //   - wait timeout: keep spinning (loop back to stop_requested)
            if (dx->is_closed()) break;
            if (!dx->wait_data_frame(buf, 200)) {
                if (!connected.load(std::memory_order_acquire)) break;
                if (dx->is_closed()) break;
                continue;
            }
            TactileFrame frame = tactile_protocol::parse_frame(buf);
            try {
                callback(frame);
            } catch (...) {
                break;  // user callback raised; exit the consumer
            }
        }
        // NOTE: this clears the streaming flag in the SHARED Impl. If a
        // disconnect()→connect()→start_streaming() cycle has already kicked
        // off a NEW streaming thread on the same Impl while we were exiting,
        // we will momentarily clobber its `streaming=true`. The window is
        // bounded by the 200 ms wait above; a future generation token would
        // close it but is out of scope for the present fix.
        streaming.store(false, std::memory_order_release);
    }

    /// Issue a command and return its response payload.
    std::vector<uint8_t> command(TactileCmd cmd, const uint8_t* payload, size_t len,
                                 uint32_t timeout_ms = TACTILE_DEFAULT_TIMEOUT_MS) {
        auto dx = snapshot_demuxer();
        if (!dx) throw TactileNotConnectedError("TactileBoard: not connected");
        return dx->command(cmd, payload, len, timeout_ms);
    }
};

// ---------------------------------------------------------------------------
// Public API — connection / streaming
// ---------------------------------------------------------------------------

TactileBoard::TactileBoard(const char* serial_number)
    : impl_(std::make_shared<Impl>()) {
    if (serial_number) impl_->serial_filter = serial_number;
}

TactileBoard::~TactileBoard() {
    disconnect();
}

bool TactileBoard::connect() {
    std::lock_guard<std::mutex> lock(impl_->lifecycle_mu);
    if (impl_->connected.load(std::memory_order_acquire)) return true;
    return impl_->open_device_locked();
}

void TactileBoard::disconnect() {
    // Pull the demuxer + streaming thread out under lifecycle_mu_; do the
    // potentially-slow wakes/joins outside the lock so concurrent commands
    // (which only hold the lock to snapshot, not for the blocking call) are
    // not blocked waiting for the streaming thread to drain its callback.
    std::shared_ptr<TactileCdcDemuxer> dx;
    std::thread thread_to_handle;
    {
        std::lock_guard<std::mutex> lock(impl_->lifecycle_mu);
        impl_->connected.store(false, std::memory_order_release);
        impl_->stop_streaming_requested.store(true, std::memory_order_release);
        dx = std::move(impl_->demuxer);
        thread_to_handle = std::move(impl_->streaming_thread);
    }
    if (dx) dx->stop();  // wake any waiter on data queue / pending response
    if (thread_to_handle.joinable()) {
        if (thread_to_handle.get_id() == std::this_thread::get_id()) {
            // Called from the streaming thread itself (e.g. user destroyed
            // the TactileBoard or invoked disconnect() from inside a frame
            // callback). Joining would self-deadlock and ~thread() would
            // std::terminate. Detach and let the thread exit naturally —
            // its lambda holds a shared_ptr<Impl>, so Impl + demuxer stay
            // alive until the thread's stack unwinds.
            thread_to_handle.detach();
        } else {
            thread_to_handle.join();
        }
    }
    impl_->streaming.store(false, std::memory_order_release);
    // dx goes out of scope here (or in some lingering command() call); the
    // last ref drop runs ~TactileCdcDemuxer which close()s the fd.
}

bool TactileBoard::is_connected() const {
    return impl_->connected.load(std::memory_order_acquire);
}

TactileFrame TactileBoard::read_frame(uint32_t timeout_ms) {
    auto dx = impl_->snapshot_demuxer();
    if (!dx) throw std::runtime_error("TactileBoard: not connected");
    if (impl_->streaming.load(std::memory_order_acquire))
        throw std::logic_error("TactileBoard: cannot call read_frame() while streaming");

    uint8_t buf[tactile_protocol::FRAME_SIZE];
    if (!dx->wait_data_frame(buf, timeout_ms)) {
        if (!impl_->connected.load(std::memory_order_acquire))
            throw ConnectionLostError("USB CDC device disconnected");
        throw std::runtime_error("TactileBoard: read_frame timeout");
    }
    return tactile_protocol::parse_frame(buf);
}

void TactileBoard::start_streaming(FrameCallback callback) {
    std::shared_ptr<TactileCdcDemuxer> dx;
    std::thread old_thread;
    {
        std::lock_guard<std::mutex> lock(impl_->lifecycle_mu);
        if (!impl_->connected.load(std::memory_order_acquire))
            throw std::runtime_error("TactileBoard: not connected");
        if (impl_->streaming.load(std::memory_order_acquire))
            throw std::logic_error("TactileBoard: already streaming");
        dx = impl_->demuxer;
        // A previous streaming thread may have exited via disconnect /
        // user-callback exception; harvest it for joining outside the lock.
        old_thread = std::move(impl_->streaming_thread);
        impl_->stop_streaming_requested.store(false, std::memory_order_release);
        impl_->streaming.store(true, std::memory_order_release);
    }
    if (old_thread.joinable()) old_thread.join();

    // Re-acquire the lifecycle lock and re-validate state. While we were
    // joining `old_thread` outside the lock a concurrent disconnect() may
    // have torn down the demuxer; if so, abort cleanly instead of starting
    // a new streaming thread that would race against the teardown and
    // immediately exit on stop_requested. Compare demuxer identity (not just
    // non-null) so a connect→disconnect→reconnect cycle that produced a
    // fresh demuxer also fails this check.
    std::lock_guard<std::mutex> lock(impl_->lifecycle_mu);
    if (!impl_->connected.load(std::memory_order_acquire)
        || impl_->demuxer != dx
        || impl_->stop_streaming_requested.load(std::memory_order_acquire)) {
        impl_->streaming.store(false, std::memory_order_release);
        throw std::runtime_error(
            "TactileBoard: disconnected before streaming could start");
    }
    try {
        // Capture shared_ptr<Impl> by value so the thread's lambda keeps
        // Impl alive for its own lifetime — required for the self-detach
        // path in disconnect()/stop_streaming() (see Impl ownership note in
        // the public header).
        auto impl_keepalive = impl_;
        impl_->streaming_thread = std::thread(
            [impl = std::move(impl_keepalive),
             dx_local = std::move(dx),
             cb = std::move(callback)]() mutable {
                impl->streaming_loop(std::move(dx_local), std::move(cb));
            });
    } catch (...) {
        impl_->streaming.store(false, std::memory_order_release);
        throw;
    }
}

void TactileBoard::stop_streaming() {
    std::thread thread_to_handle;
    {
        std::lock_guard<std::mutex> lock(impl_->lifecycle_mu);
        impl_->stop_streaming_requested.store(true, std::memory_order_release);
        thread_to_handle = std::move(impl_->streaming_thread);
    }
    if (thread_to_handle.joinable()) {
        if (thread_to_handle.get_id() == std::this_thread::get_id()) {
            // Self-detach (see disconnect() for rationale).
            thread_to_handle.detach();
        } else {
            thread_to_handle.join();
        }
    }
    impl_->streaming.store(false, std::memory_order_release);
}

void TactileBoard::set_disconnect_callback(DisconnectCallback callback) {
    // Update the user-facing slot. The internal wrapper installed in
    // open_device_locked() reads this slot when the demuxer fires.
    std::lock_guard<std::mutex> lock(impl_->disconnect_cb_mu);
    impl_->disconnect_cb = std::move(callback);
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

namespace {
TactileDiagnostics decode_diagnostics(const std::vector<uint8_t>& resp) {
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
}  // namespace

TactileDiagnostics TactileBoard::get_diagnostics() {
    auto resp = impl_->command(TactileCmd::GetDiagnostics, nullptr, 0);
    return decode_diagnostics(resp);
}

bool TactileBoard::try_get_diagnostics(TactileDiagnostics& out) {
    auto dx = impl_->snapshot_demuxer();
    if (!dx) throw TactileNotConnectedError("TactileBoard: not connected");
    auto resp = dx->try_command(TactileCmd::GetDiagnostics, nullptr, 0);
    if (!resp.has_value()) return false;  // command channel busy; caller skips
    out = decode_diagnostics(*resp);
    return true;
}

void TactileBoard::reset_counters() {
    impl_->command(TactileCmd::ResetCounters, nullptr, 0);
}

void TactileBoard::set_streaming(bool enable) {
    uint8_t payload = enable ? 1 : 0;
    impl_->command(TactileCmd::SetStreaming, &payload, 1);
}

void TactileBoard::reset_device() {
    // The device tears down USB after sending OK, so the response usually
    // never reaches us — that's the success path. Only swallow exceptions
    // that match "command sent, then the device went away"; surface real
    // failures (not connected, write failed, non-Ok status) to the caller.
    try {
        impl_->command(TactileCmd::Reset, nullptr, 0, 100);
    } catch (const TactileResponseTimeoutError&) {
        // Device jumped before the reply landed.
    } catch (const TactileDisconnectedDuringRequestError&) {
        // Device dropped USB during/after the write.
    }
    // Either we got OK, or the device is gone. In both cases this SDK
    // handle is no longer connected to a live device — drop the demuxer so
    // is_connected() reports the truth without waiting for the kernel
    // disconnect notification.
    disconnect();
}

void TactileBoard::enter_bootloader(uint32_t magic) {
    uint8_t payload[4];
    write_le32(payload, magic);
    bool jumped = true;
    try {
        impl_->command(TactileCmd::EnterBootloader, payload, 4, 100);
    } catch (const TactileResponseTimeoutError&) {
        // Device jumped to bootloader before the reply landed.
    } catch (const TactileDisconnectedDuringRequestError&) {
        // Device dropped USB during/after the write — same success path.
    } catch (...) {
        // TactileError(BadPayload) for magic mismatch, NotConnected,
        // WriteFailed — all are caller-visible failures.
        jumped = false;
        throw;
    }
    if (jumped) {
        // App firmware is gone; the device will re-enumerate as PID 0x5701
        // (bootloader). The caller must reconnect to a different device.
        disconnect();
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
