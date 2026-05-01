#include "wujihandcpp/device/tactile_board.hpp"

#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

#include "../transport/cdc_transport.hpp"
#include "tactile_cdc_demuxer.hpp"

namespace wujihandcpp {
namespace tactile {

// USB identifiers for the tactile sensor board (spec §1).
static constexpr uint16_t USB_VID = 0x0483;
static constexpr uint16_t USB_PID = 0x5700;

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

struct Board::Impl {
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
    std::shared_ptr<CdcDemuxer> demuxer;

    // Streaming consumer (one thread that drains demuxer's data queue).
    std::thread streaming_thread;
    std::atomic<bool> streaming{false};
    std::atomic<bool> stop_streaming_requested{false};

    // Bumped by every successful start_streaming(). Each spawned thread
    // captures the value it was started with; on exit it only clears the
    // `streaming` flag if the current generation still matches its own.
    // This closes the window where a self-detached old streaming thread
    // exits AFTER a disconnect→reconnect→start_streaming cycle has spawned
    // a fresh thread, and would otherwise clobber that fresh thread's
    // `streaming=true` back to false.
    std::atomic<uint64_t> streaming_generation{0};

    // User-facing disconnect callback. Held here so it survives across
    // connect() cycles and is re-installed (wrapped) on each new demuxer.
    std::mutex disconnect_cb_mu;
    DisconnectCallback disconnect_cb;

    /// MUST be called with `lifecycle_mu` held.
    bool open_device_locked() {
        auto devices = cdc::discover_devices(USB_VID, USB_PID);
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

        auto new_demuxer = std::make_shared<CdcDemuxer>(fd);

        // Internal disconnect wrapper: flip `connected` BEFORE invoking the
        // user callback so anything checking is_connected() (including
        // streaming_loop) sees the new state immediately. Captures `this`
        // (= Impl*); ~Board runs disconnect() which drops the demuxer's ref
        // before Impl is freed, so the demuxer can never invoke this lambda
        // after Impl is gone.
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
    std::shared_ptr<CdcDemuxer> snapshot_demuxer() {
        std::lock_guard<std::mutex> lock(lifecycle_mu);
        return demuxer;
    }

    void streaming_loop(uint64_t my_gen,
                        std::shared_ptr<CdcDemuxer> dx,
                        FrameCallback callback) {
        uint8_t buf[protocol::FRAME_SIZE];
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
            Frame frame = protocol::parse_frame(buf);
            try {
                callback(frame);
            } catch (...) {
                break;  // user callback raised; exit the consumer
            }
        }
        // Generation guard: only clear the shared `streaming` flag if we are
        // still the current generation. If a disconnect→reconnect cycle
        // started a fresh streaming thread while we (an old, possibly
        // detached, thread) were unwinding, clobbering its `streaming=true`
        // would leave the SDK reporting "not streaming" while a thread is
        // actively consuming frames.
        uint64_t cur_gen = streaming_generation.load(std::memory_order_acquire);
        if (cur_gen == my_gen) {
            streaming.store(false, std::memory_order_release);
        }
    }

    /// Issue a command and return its response payload.
    std::vector<uint8_t> command(Cmd cmd, const uint8_t* payload, size_t len,
                                 uint32_t timeout_ms = DEFAULT_TIMEOUT_MS) {
        auto dx = snapshot_demuxer();
        if (!dx) throw NotConnectedError("tactile::Board: not connected");
        return dx->command(cmd, payload, len, timeout_ms);
    }
};

// ---------------------------------------------------------------------------
// Public API — connection / streaming
// ---------------------------------------------------------------------------

Board::Board(const char* serial_number)
    : impl_(std::make_shared<Impl>()) {
    if (serial_number) impl_->serial_filter = serial_number;
}

Board::~Board() {
    disconnect();
}

bool Board::connect() {
    std::lock_guard<std::mutex> lock(impl_->lifecycle_mu);
    if (impl_->connected.load(std::memory_order_acquire)) return true;
    return impl_->open_device_locked();
}

void Board::disconnect() {
    // Pull the demuxer + streaming thread out under lifecycle_mu_; do the
    // potentially-slow wakes/joins outside the lock so concurrent commands
    // (which only hold the lock to snapshot, not for the blocking call) are
    // not blocked waiting for the streaming thread to drain its callback.
    std::shared_ptr<CdcDemuxer> dx;
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
            // the Board or invoked disconnect() from inside a frame
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
    // last ref drop runs ~CdcDemuxer which close()s the fd.
}

bool Board::is_connected() const {
    return impl_->connected.load(std::memory_order_acquire);
}

Frame Board::read_frame(uint32_t timeout_ms) {
    auto dx = impl_->snapshot_demuxer();
    if (!dx) throw std::runtime_error("tactile::Board: not connected");
    if (impl_->streaming.load(std::memory_order_acquire))
        throw std::logic_error("tactile::Board: cannot call read_frame() while streaming");

    uint8_t buf[protocol::FRAME_SIZE];
    if (!dx->wait_data_frame(buf, timeout_ms)) {
        if (!impl_->connected.load(std::memory_order_acquire))
            throw ConnectionLostError("USB CDC device disconnected");
        throw std::runtime_error("tactile::Board: read_frame timeout");
    }
    return protocol::parse_frame(buf);
}

void Board::start_streaming(FrameCallback callback) {
    std::shared_ptr<CdcDemuxer> dx;
    std::thread old_thread;
    uint64_t my_gen;
    {
        std::lock_guard<std::mutex> lock(impl_->lifecycle_mu);
        if (!impl_->connected.load(std::memory_order_acquire))
            throw std::runtime_error("tactile::Board: not connected");
        if (impl_->streaming.load(std::memory_order_acquire))
            throw std::logic_error("tactile::Board: already streaming");
        dx = impl_->demuxer;
        // A previous streaming thread may have exited via disconnect /
        // user-callback exception; harvest it for joining outside the lock.
        old_thread = std::move(impl_->streaming_thread);
        impl_->stop_streaming_requested.store(false, std::memory_order_release);
        // Bump the generation BEFORE flipping streaming=true. Any old
        // detached thread that exits between these two stores will read the
        // already-bumped generation, see the mismatch with its captured
        // value, and skip the streaming-flag clear that would otherwise
        // clobber us. Bumping after the streaming store would re-open the
        // race we are closing here.
        my_gen = impl_->streaming_generation.fetch_add(
            1, std::memory_order_acq_rel) + 1;
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
            "tactile::Board: disconnected before streaming could start");
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
             cb = std::move(callback),
             my_gen]() mutable {
                impl->streaming_loop(my_gen, std::move(dx_local), std::move(cb));
            });
    } catch (...) {
        impl_->streaming.store(false, std::memory_order_release);
        throw;
    }
}

void Board::stop_streaming() {
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

void Board::set_disconnect_callback(DisconnectCallback callback) {
    // Update the user-facing slot. The internal wrapper installed in
    // open_device_locked() reads this slot when the demuxer fires.
    std::lock_guard<std::mutex> lock(impl_->disconnect_cb_mu);
    impl_->disconnect_cb = std::move(callback);
}

// ---------------------------------------------------------------------------
// Public API — commands (spec §3)
// ---------------------------------------------------------------------------

DeviceInfo Board::get_device_info() {
    auto resp = impl_->command(Cmd::GetDeviceInfo, nullptr, 0);
    if (resp.size() != 32) {
        throw std::runtime_error("get_device_info: unexpected payload size");
    }
    DeviceInfo info;
    info.serial = trim_ascii(resp.data(), 24);
    std::memcpy(info.hw_revision.data(), resp.data() + 24, 4);
    std::memcpy(info.fw_version.data(), resp.data() + 28, 4);
    return info;
}

FwBuild Board::get_fw_build() {
    auto resp = impl_->command(Cmd::GetFwBuild, nullptr, 0);
    if (resp.size() != 8) {
        throw std::runtime_error("get_fw_build: unexpected payload size");
    }
    FwBuild build;
    build.git_short_sha = trim_ascii(resp.data(), 8);
    return build;
}

Handedness Board::get_handedness() {
    auto resp = impl_->command(Cmd::GetHandedness, nullptr, 0);
    if (resp.size() != 1) {
        throw std::runtime_error("get_handedness: unexpected payload size");
    }
    return static_cast<Handedness>(resp[0]);
}

namespace {
Diagnostics decode_diagnostics(const std::vector<uint8_t>& resp) {
    if (resp.size() != 18) {
        throw std::runtime_error("get_diagnostics: unexpected payload size");
    }
    Diagnostics d;
    d.uptime_ms       = read_le32(resp.data() + 0);
    d.frame_count     = read_le32(resp.data() + 4);
    d.crc_err_count   = read_le32(resp.data() + 8);
    d.dropout_count   = read_le32(resp.data() + 12);
    d.usb_reset_count = read_le16(resp.data() + 16);
    return d;
}
}  // namespace

Diagnostics Board::get_diagnostics() {
    auto resp = impl_->command(Cmd::GetDiagnostics, nullptr, 0);
    return decode_diagnostics(resp);
}

bool Board::try_get_diagnostics(Diagnostics& out) {
    auto dx = impl_->snapshot_demuxer();
    if (!dx) throw NotConnectedError("tactile::Board: not connected");
    auto resp = dx->try_command(Cmd::GetDiagnostics, nullptr, 0);
    if (!resp.has_value()) return false;  // command channel busy; caller skips
    out = decode_diagnostics(*resp);
    return true;
}

void Board::reset_counters() {
    impl_->command(Cmd::ResetCounters, nullptr, 0);
}

void Board::set_streaming(bool enable) {
    uint8_t payload = enable ? 1 : 0;
    impl_->command(Cmd::SetStreaming, &payload, 1);
}

void Board::reset_device() {
    // The device tears down USB after sending OK, so the response usually
    // never reaches us — that's the success path. Only swallow exceptions
    // that match "command sent, then the device went away"; surface real
    // failures (not connected, write failed, non-Ok status) to the caller.
    try {
        impl_->command(Cmd::Reset, nullptr, 0, 100);
    } catch (const ResponseTimeoutError&) {
        // Device jumped before the reply landed.
    } catch (const DisconnectedDuringRequestError&) {
        // Device dropped USB during/after the write.
    }
    // Either we got OK, or the device is gone. In both cases this SDK
    // handle is no longer connected to a live device — drop the demuxer so
    // is_connected() reports the truth without waiting for the kernel
    // disconnect notification.
    disconnect();
}

void Board::enter_bootloader(uint32_t magic) {
    uint8_t payload[4];
    write_le32(payload, magic);
    try {
        impl_->command(Cmd::EnterBootloader, payload, 4, 100);
    } catch (const ResponseTimeoutError&) {
        // Device jumped to bootloader before the reply landed.
    } catch (const DisconnectedDuringRequestError&) {
        // Device dropped USB during/after the write — same success path.
    }
    // Reaching this point means we got OK, hit a timeout, or saw the device
    // drop — all "bootloader is now running" outcomes. Other exceptions
    // (BadPayload for wrong magic, NotConnected, WriteFailed) propagate from
    // command() and skip this teardown. The device re-enumerates as PID
    // 0x5701; the caller must connect to that handle.
    disconnect();
}

uint16_t Board::get_sample_rate_hz() {
    uint8_t req[2];
    write_le16(req, static_cast<uint16_t>(ConfigKey::SampleRateHz));
    auto resp = impl_->command(Cmd::GetConfig, req, 2);
    if (resp.size() != 3 || resp[0] != static_cast<uint8_t>(ConfigType::U16)) {
        throw std::runtime_error("get_sample_rate_hz: malformed response");
    }
    return read_le16(resp.data() + 1);
}

void Board::set_sample_rate_hz(uint16_t hz) {
    uint8_t req[5];
    write_le16(req, static_cast<uint16_t>(ConfigKey::SampleRateHz));
    req[2] = static_cast<uint8_t>(ConfigType::U16);
    write_le16(req + 3, hz);
    impl_->command(Cmd::SetConfig, req, 5);
}

bool Board::get_streaming_enabled() {
    uint8_t req[2];
    write_le16(req, static_cast<uint16_t>(ConfigKey::StreamingEnabled));
    auto resp = impl_->command(Cmd::GetConfig, req, 2);
    if (resp.size() != 2 || resp[0] != static_cast<uint8_t>(ConfigType::EnumU8)) {
        throw std::runtime_error("get_streaming_enabled: malformed response");
    }
    return resp[1] != 0;
}

DeviceTime Board::get_device_time() {
    auto resp = impl_->command(Cmd::GetDeviceTime, nullptr, 0);
    if (resp.size() != 8) {
        throw std::runtime_error("get_device_time: unexpected payload size");
    }
    return DeviceTime{read_le64(resp.data())};
}

SyncResult Board::sync_host_epoch(uint64_t host_unix_ns) {
    uint8_t req[8];
    write_le64(req, host_unix_ns);
    auto resp = impl_->command(Cmd::SyncHostEpoch, req, 8);
    if (resp.size() != 16) {
        throw std::runtime_error("sync_host_epoch: unexpected payload size");
    }
    SyncResult r;
    r.device_ns_at_sync = read_le64(resp.data());
    r.host_ns_echo      = read_le64(resp.data() + 8);
    // Spec §3.5.2: device echoes the request's host_unix_ns verbatim. A
    // mismatch implies a stale response from a prior sync_host_epoch (request
    // pipelining bug, or seq wrap landing on the wrong waiter despite the
    // (seq, cmd_id) pairing). Surface this rather than silently accepting a
    // wrong epoch pair.
    if (r.host_ns_echo != host_unix_ns) {
        throw std::runtime_error("sync_host_epoch: device echoed mismatched host_unix_ns");
    }
    return r;
}

}  // namespace tactile
}  // namespace wujihandcpp
