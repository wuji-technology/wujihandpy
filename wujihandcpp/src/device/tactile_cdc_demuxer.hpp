#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "wujihandcpp/data/tactile.hpp"
#include "wujihandcpp/protocol/tactile_command.hpp"

namespace wujihandcpp {

/// CDC stream demultiplexer for the tactile board.
///
/// Owns one CDC fd and runs a single reader thread that classifies each
/// incoming frame by sync byte (spec §2.1):
///   - `AA 55` data frames     → bounded queue consumed by `wait_data_frame()`
///   - `AA 57` response frames → matched by `seq` to the in-flight `command()`
///   - `AA 56` (host→device)   → drained (protocol violation; not expected on RX)
///
/// Command requests are strictly serial (spec §2.4): only one `command()` call
/// is in flight at a time. On disconnect, all waiters are woken and the
/// registered disconnect callback is invoked once.
///
/// Lifetime: MUST be created via std::make_shared. The reader thread captures
/// a shared_ptr via shared_from_this() in start() so the demuxer remains
/// alive until the reader loop exits — required for the self-detach path in
/// stop() (where stop() is called from inside a user-installed disconnect
/// callback running on the reader thread itself, and joining would
/// std::terminate).
class TactileCdcDemuxer : public std::enable_shared_from_this<TactileCdcDemuxer> {
public:
    using DisconnectCallback = std::function<void()>;

    /// Construct a demuxer that takes ownership of `fd`. The fd is closed
    /// in the destructor so callers must not close it themselves.
    explicit TactileCdcDemuxer(int fd);
    ~TactileCdcDemuxer();

    TactileCdcDemuxer(const TactileCdcDemuxer&) = delete;
    TactileCdcDemuxer& operator=(const TactileCdcDemuxer&) = delete;

    /// Start the reader thread. Must not be called twice.
    void start();

    /// Stop the reader thread. Idempotent. After stop() returns no further
    /// commands can be dispatched and any in-flight wait is aborted.
    /// Normally joins the reader thread; if called from the reader thread
    /// itself (e.g. via a user disconnect callback) the thread is detached
    /// and the captured self-shared_ptr keeps the demuxer alive until the
    /// loop unwinds.
    void stop();

    /// True if no further commands may be dispatched, either because stop()
    /// was called or because the device dropped off the bus.
    bool is_closed() const {
        return stop_requested_.load(std::memory_order_acquire)
            || disconnected_.load(std::memory_order_acquire);
    }

    /// Block waiting for the next data frame.
    /// @return true if a frame was placed in `out`; false on timeout / stop / disconnect.
    bool wait_data_frame(uint8_t out[tactile_protocol::FRAME_SIZE], uint32_t timeout_ms);

    /// Send a command and wait for the response payload. Auto-retries once on
    /// `BadCrc` per spec §2.4.
    /// @throws TactileError on non-Ok status (after retry).
    /// @throws std::runtime_error on timeout.
    /// @throws std::runtime_error on disconnect mid-request.
    std::vector<uint8_t> command(TactileCmd cmd, const uint8_t* payload, size_t payload_len,
                                 uint32_t timeout_ms = TACTILE_DEFAULT_TIMEOUT_MS);

    /// Like command() but returns std::nullopt instead of blocking when the
    /// per-channel command mutex is already held by another caller. Used by
    /// the ROS driver's diagnostics timer so it can yield to user-issued
    /// services instead of queueing behind them on the SDK serializer.
    /// @throws TactileError on non-Ok status (after BadCrc retry).
    /// @throws TactileResponseTimeoutError on timeout.
    /// @throws TactileNotConnectedError / TactileDisconnectedDuringRequestError on disconnect.
    std::optional<std::vector<uint8_t>>
    try_command(TactileCmd cmd, const uint8_t* payload, size_t payload_len,
                uint32_t timeout_ms = TACTILE_DEFAULT_TIMEOUT_MS);

    /// Replace the disconnect callback. Empty std::function clears it.
    void set_disconnect_callback(DisconnectCallback cb);

private:
    void reader_loop();
    void handle_data_frame(const uint8_t* buf);
    void handle_response_frame(const uint8_t* buf, uint16_t length);
    void handle_disconnect();
    bool read_drain(size_t count, uint32_t timeout_ms);
    std::vector<uint8_t> single_command(TactileCmd cmd, const uint8_t* payload,
                                        size_t payload_len, uint32_t timeout_ms);

    int fd_;
    std::thread thread_;
    // stop_requested_ = lifecycle owner asked us to shut down (stop() called).
    // disconnected_   = device dropped off the bus (EIO/HUP from read).
    // Either one short-circuits the command path and aborts in-flight waiters.
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> disconnected_{false};

    // Data frame queue (bounded, drop-oldest). 16 slots = ~130 ms at 120 Hz,
    // enough to absorb scheduler hiccups in the consumer thread without
    // unbounded memory growth if the consumer is permanently slow.
    //
    // Note: there is a separate, ~0.5 s host-side stall observed roughly
    // every 2-3 s at 120 Hz that is NOT a queue-overflow problem (firmware
    // reports zero dropouts and CRC errors during it; the bytes are simply
    // not surfacing through the cdc-acm path during the stall window).
    // Increasing MAX_QUEUE further does not eliminate it. Tracked under
    // "Risks & Open Items" in the design doc; tactile consumers must
    // tolerate occasional sub-second gaps.
    static constexpr size_t MAX_QUEUE = 16;
    using FrameBuf = std::array<uint8_t, tactile_protocol::FRAME_SIZE>;
    std::mutex frame_mu_;
    std::condition_variable frame_cv_;
    std::deque<FrameBuf> frame_queue_;

    // Command path. command_mu_ enforces strict serial requests (spec §2.4);
    // pending_mu_ guards the in-flight slot read by the reader thread.
    std::mutex command_mu_;
    std::mutex pending_mu_;
    std::condition_variable pending_cv_;
    bool pending_active_ = false;
    uint16_t pending_seq_ = 0;
    TactileCmd pending_cmd_ = TactileCmd::GetDeviceInfo;  // matched alongside seq
    bool pending_filled_ = false;
    TactileStatus pending_status_ = TactileStatus::Ok;
    std::vector<uint8_t> pending_payload_;

    std::atomic<uint16_t> next_seq_{1};

    std::mutex disconnect_cb_mu_;
    DisconnectCallback disconnect_cb_;
};

}  // namespace wujihandcpp
