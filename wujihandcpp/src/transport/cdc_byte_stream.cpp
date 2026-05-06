#include "cdc_byte_stream.hpp"

#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <termios.h>
#include <unistd.h>

namespace wujihandcpp {
namespace cdc {

namespace {

// Open `tty_path` in CDC raw mode and return the fd. The kernel CDC ACM
// driver presents the device as a character device that, by default,
// runs canonical line-editing — exactly the wrong thing for a binary
// wire protocol. cfmakeraw + VMIN=1 VTIME=0 puts us into the mode where
// read() returns as soon as any byte is available, no line buffering.
//
// Returns -1 on any failure; caller decides whether to throw or retry.
int open_raw_cdc(const char* tty_path) {
    int fd = open(tty_path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) return -1;

    // Clear O_NONBLOCK after open — we use poll() for timeout control,
    // not non-blocking read/write semantics, and EAGAIN handling is
    // simpler when the fd is otherwise blocking.
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);

    struct termios tty{};
    if (tcgetattr(fd, &tty) != 0) {
        close(fd);
        return -1;
    }

    // Raw mode: no echo, no canonical processing, no signal chars.
    // Essential for binary protocol data over USB CDC.
    cfmakeraw(&tty);

    // No flow control.
    tty.c_cflag &= ~CRTSCTS;
    tty.c_cflag |= CREAD | CLOCAL;

    // VMIN=1, VTIME=0: read blocks until at least 1 byte available.
    tty.c_cc[VMIN] = 1;
    tty.c_cc[VTIME] = 0;

    if (tcsetattr(fd, TCSANOW, &tty) != 0) {
        close(fd);
        return -1;
    }

    // Flush any stale data in kernel buffers — important when
    // re-opening after a previous instance crashed mid-frame.
    tcflush(fd, TCIOFLUSH);

    return fd;
}

}  // namespace

CdcByteStream::CdcByteStream(const char* tty_path) : fd_(open_raw_cdc(tty_path)) {
    if (fd_ < 0) {
        throw std::runtime_error(
            std::string("CdcByteStream: failed to open ") + tty_path);
    }
}

CdcByteStream::~CdcByteStream() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

ssize_t CdcByteStream::read(uint8_t* buf, size_t len, uint32_t timeout_ms) {
    size_t total = 0;
    auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

    while (total < len) {
        // Cumulative deadline: gate every iteration on the remaining
        // budget. timeout_ms==0 collapses to "already-timed-out" which
        // returns total=0 immediately — defensive against callers that
        // construct this from `deadline - now` math after the deadline
        // already passed.
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                             deadline - std::chrono::steady_clock::now())
                             .count();
        if (remaining <= 0) return static_cast<ssize_t>(total);

        struct pollfd pfd{};
        pfd.fd = fd_;
        pfd.events = POLLIN;
        int ret = poll(&pfd, 1, static_cast<int>(remaining));
        if (ret < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (ret == 0) {
            return static_cast<ssize_t>(total);  // timeout
        }
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            return -1;  // device disconnected mid-read
        }

        ssize_t n = ::read(fd_, buf + total, len - total);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            return -1;
        }
        if (n == 0) {
            return -1;  // EOF = disconnect
        }
        total += static_cast<size_t>(n);
    }

    return static_cast<ssize_t>(total);
}

ssize_t CdcByteStream::write(const uint8_t* buf, size_t len, uint32_t timeout_ms) {
    size_t total = 0;
    auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

    while (total < len) {
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                             deadline - std::chrono::steady_clock::now())
                             .count();
        if (remaining <= 0) return static_cast<ssize_t>(total);

        struct pollfd pfd{};
        pfd.fd = fd_;
        pfd.events = POLLOUT;
        int ret = poll(&pfd, 1, static_cast<int>(remaining));
        if (ret < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (ret == 0) {
            return static_cast<ssize_t>(total);  // timeout
        }
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            return -1;  // device disconnected mid-write
        }

        ssize_t n = ::write(fd_, buf + total, len - total);
        if (n < 0) {
            if (errno == EINTR) continue;
            // EAGAIN can briefly fire between the poll() ready signal
            // and the write() under heavy USB pressure; the loop's
            // `remaining <= 0` deadline check above bounds the retry.
            if (errno == EAGAIN) continue;
            return -1;
        }
        if (n == 0) {
            return -1;
        }
        total += static_cast<size_t>(n);
    }
    return static_cast<ssize_t>(total);
}

}  // namespace cdc
}  // namespace wujihandcpp
