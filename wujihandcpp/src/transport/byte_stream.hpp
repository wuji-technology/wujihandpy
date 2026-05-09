#pragma once

#include <cstddef>
#include <cstdint>

#include <sys/types.h>  // ssize_t

namespace wujihandcpp {
namespace transport {

/// Synchronous, deadline-bounded byte-stream interface.
///
/// Models the minimum surface a frame demuxer needs from a CDC ACM /
/// kernel-tty / file-descriptor IO source: blocking read up to `len`
/// bytes within a deadline, blocking write of exactly `len` bytes
/// within a deadline. Both methods short-read / short-write on
/// timeout and surface device disconnection as -1.
///
/// This is deliberately *not* the same shape as `transport::ITransport`,
/// which is async + zero-copy + designed around libusb_transfer. The
/// joint controller path uses ITransport because libusb's pipelined
/// async transfers are how it hits 1 kHz throughput; the tactile path
/// uses IByteStream because /dev/ttyACMx with poll(2) is a sync
/// kernel API and pretending otherwise would only add complexity. The
/// two interfaces coexist because they serve genuinely different USB
/// classes (custom bulk vs. CDC ACM) with different IO models.
///
/// Lifetime: implementations own any underlying fd / handle and
/// release it in the destructor. Callers are expected to hold the
/// stream via `std::shared_ptr` so the higher-level demuxer / command
/// pipeline can keep it alive across async teardown paths.
///
/// Thread safety: read() may be called concurrently with write(). A
/// single dedicated reader thread + a single command-channel writer
/// is the expected usage pattern (see `tactile::FrameDemuxer`).
/// Concurrent calls to the same direction (two readers, two writers)
/// are NOT supported by any current implementation.
class IByteStream {
public:
    virtual ~IByteStream() noexcept = default;

    IByteStream() = default;
    IByteStream(const IByteStream&) = delete;
    IByteStream& operator=(const IByteStream&) = delete;
    IByteStream(IByteStream&&) = delete;
    IByteStream& operator=(IByteStream&&) = delete;

    /// Read up to `len` bytes into `buf`, retrying on partial reads
    /// until either `len` is reached or the cumulative deadline expires.
    /// @param buf         destination buffer, size >= len
    /// @param len         bytes to read (caller checks short-read against this)
    /// @param timeout_ms  cumulative deadline in milliseconds. Required —
    ///                    no implementation accepts 0 / "no timeout"; the
    ///                    whole point of this interface is to bound IO.
    /// @return bytes actually placed in `buf`. < len on timeout. -1 on
    ///         disconnect / unrecoverable error (caller treats as fatal
    ///         for this stream).
    virtual ssize_t read(uint8_t* buf, size_t len, uint32_t timeout_ms) = 0;

    /// Write exactly `len` bytes from `buf`, retrying on partial writes
    /// until either `len` is reached or the cumulative deadline expires.
    /// @param buf         source buffer
    /// @param len         bytes to write
    /// @param timeout_ms  cumulative deadline in milliseconds
    /// @return bytes actually written. < len on timeout (caller treats
    ///         the partial write as an unparseable wire frame and fails
    ///         the command). -1 on disconnect / unrecoverable error.
    virtual ssize_t write(const uint8_t* buf, size_t len, uint32_t timeout_ms) = 0;
};

}  // namespace transport
}  // namespace wujihandcpp
