#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
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
///   - `AA 56` (host→device)   → drained as protocol violation, warning logged
///
/// Command requests are strictly serial (spec §2.4): only one `command()` call
/// is in flight at a time. On disconnect, all waiters are woken with a
/// disconnected status and the registered disconnect callback is invoked once.
class TactileCdcDemuxer {
public:
    using DisconnectCallback = std::function<void()>;

    explicit TactileCdcDemuxer(int fd);
    ~TactileCdcDemuxer();

    TactileCdcDemuxer(const TactileCdcDemuxer&) = delete;
    TactileCdcDemuxer& operator=(const TactileCdcDemuxer&) = delete;

    /// Start the reader thread. Must not be called twice.
    void start();

    /// Stop the reader thread and wake all waiters. Idempotent.
    void stop();

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
    bool pending_filled_ = false;
    TactileStatus pending_status_ = TactileStatus::Ok;
    std::vector<uint8_t> pending_payload_;

    std::atomic<uint16_t> next_seq_{1};

    std::mutex disconnect_cb_mu_;
    DisconnectCallback disconnect_cb_;
    std::atomic<bool> disconnect_cb_fired_{false};
};

}  // namespace wujihandcpp
