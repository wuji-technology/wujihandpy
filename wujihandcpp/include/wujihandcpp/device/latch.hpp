#pragma once

#include <atomic>
#include <stdexcept>
#include <string>

#include "wujihandcpp/utility/api.hpp"

namespace wujihandcpp {
namespace device {

class TimeoutError : public std::runtime_error {
public:
    using runtime_error::runtime_error;
};

class DeviceDisconnectedError : public std::runtime_error {
public:
    using runtime_error::runtime_error;
};

class Latch {
public:
    template <typename T>
    friend class DataOperator;

    WUJIHANDCPP_API void wait() {
        if (int error_count = try_wait_internal()) {
            if (error_count == 1)
                throw TimeoutError("Operation timed out while waiting for completion");
            else
                throw TimeoutError(
                    std::to_string(error_count)
                    + " operations timed out while waiting for completion");
        }
    }

    WUJIHANDCPP_API bool try_wait() noexcept { return try_wait_internal() == 0; }

private:
    WUJIHANDCPP_API int try_wait_internal() noexcept;

    WUJIHANDCPP_API void count_up() noexcept;
    WUJIHANDCPP_API void count_down(bool success) noexcept;

    std::atomic<int> waiting_count_{0};
    std::atomic<int> error_count_{0};
};

} // namespace device
} // namespace wujihandcpp