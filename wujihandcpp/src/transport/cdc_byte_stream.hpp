#pragma once

#include <cstdint>

#include "byte_stream.hpp"

namespace wujihandcpp {
namespace cdc {

/// Linux USB CDC ACM byte stream over /dev/ttyACMx.
///
/// Holds an fd opened with the raw-mode termios setup the new tactile
/// wire protocol expects (no echo, no canonical processing, VMIN=1
/// VTIME=0 — see open_cdc() impl). Closes the fd in the destructor.
///
/// Use via `std::shared_ptr<transport::IByteStream>` so the demuxer
/// + command pipeline can hold their own refs across async teardown.
/// The original `cdc::open_cdc / cdc::read_exact / cdc::write_exact`
/// free-function family this replaces leaked the fd lifecycle into
/// every caller; channelling everything through one RAII class makes
/// the ownership story trivial: if you have a CdcByteStream alive,
/// the fd is open.
class CdcByteStream : public transport::IByteStream {
public:
    /// Open `tty_path` (e.g. "/dev/ttyACM0") in CDC raw mode.
    /// @throws std::runtime_error on open() / tcgetattr() / tcsetattr() failure.
    explicit CdcByteStream(const char* tty_path);
    ~CdcByteStream() override;

    /// Synchronous read with cumulative deadline. See IByteStream::read.
    ssize_t read(uint8_t* buf, size_t len, uint32_t timeout_ms) override;

    /// Synchronous write with cumulative deadline. See IByteStream::write.
    /// timeout_ms must be > 0 — passing 0 silently regresses to the
    /// unbounded-block failure mode this class was added to fix.
    ssize_t write(const uint8_t* buf, size_t len, uint32_t timeout_ms) override;

private:
    int fd_;
};

}  // namespace cdc
}  // namespace wujihandcpp
