#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include <atomic>
#include <chrono>
#include <span>
#include <type_traits>
#include <vector>

#include "wujihandcpp/device/controller.hpp"
#include "wujihandcpp/utility/api.hpp"

namespace wujihandcpp {
namespace protocol {

class Handler final {
public:
    struct StorageInfo {
        StorageInfo() = default;

        constexpr explicit StorageInfo(
            size_t data_size, uint16_t index, uint8_t sub_index, uint32_t policy = NONE)
            : index(index)
            , sub_index(sub_index)
            , size(
                  data_size == 1
                      ? Size::_1
                      : (data_size == 2 ? Size::_2 : (data_size == 4 ? Size::_4 : Size::_8)))
            , policy(policy) {}

        uint16_t index;
        uint8_t sub_index;

        enum class Size : uint32_t { _1, _2, _4, _8 } size : 2;
        enum Policy : uint32_t {
            NONE = 0,
            MASKED = 1ul << 0,
            CONTROL_WORD = 1ul << 1,
            POSITION = 1ul << 2,
            POSITION_REVERSED = 1ul << 3,
            VELOCITY = 1ul << 4,
            VELOCITY_REVERSED = 1ul << 5,
            HOST_HEARTBEAT = 1ul << 6
        };
        uint32_t policy : 30;
    };

    struct Buffer8 {
        Buffer8() = default;

        template <typename T>
        using remove_cvref_t = typename std::remove_cv<typename std::remove_cv<T>::type>::type;

        template <typename T>
        explicit Buffer8(const T& value) {
            static_assert(sizeof(remove_cvref_t<T>) <= 8, "");
            static_assert(
                std::is_trivially_copyable<remove_cvref_t<T>>::value
                    && std::is_trivially_destructible<remove_cvref_t<T>>::value,
                "");
            new (storage) remove_cvref_t<T>{value};
        }

        template <typename T>
        remove_cvref_t<T> as() const {
            return *reinterpret_cast<const remove_cvref_t<T>*>(storage);
        }

        alignas(8) uint8_t storage[8];
        static_assert(sizeof(void*) == 8, "");
    };

    WUJIHANDCPP_API explicit Handler(
        uint16_t usb_vid, int32_t usb_pid, const char* serial_number, size_t storage_unit_count);

    WUJIHANDCPP_API ~Handler();

    WUJIHANDCPP_API void init_storage_info(int storage_id, StorageInfo info);

    WUJIHANDCPP_API void start_transmit_receive();

    WUJIHANDCPP_API void
        read_async_unchecked(int storage_id, std::chrono::steady_clock::duration::rep timeout);

    WUJIHANDCPP_API void read_async(
        int storage_id, std::chrono::steady_clock::duration::rep timeout,
        void (*callback)(Buffer8 context, bool success), Buffer8 callback_context);

    WUJIHANDCPP_API void write_async_unchecked(
        Buffer8 data, int storage_id, std::chrono::steady_clock::duration::rep timeout);

    WUJIHANDCPP_API void write_async(
        Buffer8 data, int storage_id, std::chrono::steady_clock::duration::rep timeout,
        void (*callback)(Buffer8 context, bool success), Buffer8 callback_context);

    WUJIHANDCPP_API auto realtime_get_joint_actual_position()
        -> const std::atomic<double> (&)[5][4];

    WUJIHANDCPP_API void realtime_set_joint_target_position(const double (&positions)[5][4]);

    WUJIHANDCPP_API void
        attach_realtime_controller(device::IRealtimeController* controller, bool enable_upstream);

    WUJIHANDCPP_API device::IRealtimeController* detach_realtime_controller();

    WUJIHANDCPP_API void start_latency_test();
    WUJIHANDCPP_API void stop_latency_test();

    WUJIHANDCPP_API Buffer8 get(int storage_id);

    WUJIHANDCPP_API void disable_thread_safe_check();

    // Raw SDO operations for debugging
    WUJIHANDCPP_API std::vector<std::byte> raw_sdo_read(
        uint16_t index, uint8_t sub_index, std::chrono::steady_clock::duration timeout);

    WUJIHANDCPP_API void raw_sdo_write(
        uint16_t index, uint8_t sub_index, std::span<const std::byte> data,
        std::chrono::steady_clock::duration timeout);

private:
    class Impl;
    Impl* impl_;
};

} // namespace protocol
} // namespace wujihandcpp
