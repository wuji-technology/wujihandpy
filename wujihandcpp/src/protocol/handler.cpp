#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <numbers>
#include <span>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>

#include <spdlog/fmt/bin_to_hex.h>
#include <wujihandcpp/device/latch.hpp>
#include <wujihandcpp/protocol/handler.hpp>
#include <wujihandcpp/utility/api.hpp>

#include "logging/logging.hpp"
#include "protocol/frame_builder.hpp"
#include "protocol/latency_tester.hpp"
#include "protocol/protocol.hpp"
#include "transport/transport.hpp"
#include "utility/tick_executor.hpp"

namespace wujihandcpp::protocol {

class Handler::Impl {
public:
    explicit Impl(std::unique_ptr<transport::ITransport> transport, size_t storage_unit_count)
        : logger_(logging::get_logger())
        , operation_thread_id_(std::this_thread::get_id())
        , storage_unit_count_(storage_unit_count)
        , storage_(std::make_unique<StorageUnit[]>(storage_unit_count))
        , transport_(std::move(transport))
        , sdo_builder_(*transport_, 0x21)
        , pdo_builder_(*transport_, 0x11) {}

    ~Impl() = default;

    void init_storage_info(int storage_id, StorageInfo info) {
        storage_[storage_id].info = info;
        IndexMapKey index{.index = info.index, .sub_index = info.sub_index};
        index_storage_map_[std::bit_cast<uint32_t>(index)] = &storage_[storage_id];
    }

    void start_transmit_receive() {
        transport_->receive([this](const std::byte* buffer, size_t size) {
            receive_transfer_completed_callback(buffer, size);
        });

        sdo_thread_ = std::jthread{
            [this](const std::stop_token& stop_token) { sdo_thread_main(stop_token); }};
    }

    void read_async_unchecked(int storage_id, std::chrono::steady_clock::duration::rep timeout) {
        operation_thread_check();

        if (storage_[storage_id].operation.load(std::memory_order::relaxed).mode
            != Operation::Mode::NONE)
            return;

        storage_[storage_id].timeout = std::chrono::steady_clock::duration(timeout);
        storage_[storage_id].callback = nullptr;
        storage_[storage_id].operation.store(
            Operation{.mode = Operation::Mode::READ, .state = Operation::State::WAITING},
            std::memory_order::release);
    }

    void read_async(
        int storage_id, std::chrono::steady_clock::duration::rep timeout,
        void (*callback)(Buffer8 context, bool success), Buffer8 callback_context) {
        operation_thread_check();

        if (storage_[storage_id].operation.load(std::memory_order::relaxed).mode
            != Operation::Mode::NONE) [[unlikely]]
            throw std::runtime_error("Illegal checked read: Data is being operated!");

        storage_[storage_id].timeout = std::chrono::steady_clock::duration(timeout);
        storage_[storage_id].callback = callback;
        storage_[storage_id].callback_context = callback_context;
        storage_[storage_id].operation.store(
            Operation{.mode = Operation::Mode::READ, .state = Operation::State::WAITING},
            std::memory_order::release);
    }

    void write_async_unchecked(
        Buffer8 data, int storage_id, std::chrono::steady_clock::duration::rep timeout) {
        operation_thread_check();

        store_data(storage_[storage_id], data);

        if (storage_[storage_id].operation.load(std::memory_order::relaxed).mode
            != Operation::Mode::NONE)
            return;

        storage_[storage_id].timeout = std::chrono::steady_clock::duration(timeout);
        storage_[storage_id].callback = nullptr;
        storage_[storage_id].operation.store(
            Operation{.mode = Operation::Mode::WRITE, .state = Operation::State::WAITING},
            std::memory_order::release);
    }

    void write_async(
        Buffer8 data, int storage_id, std::chrono::steady_clock::duration::rep timeout,
        void (*callback)(Buffer8 context, bool success), Buffer8 callback_context) {
        operation_thread_check();

        if (storage_[storage_id].operation.load(std::memory_order::relaxed).mode
            != Operation::Mode::NONE) [[unlikely]]
            throw std::runtime_error("Illegal checked write: Data is being operated!");

        store_data(storage_[storage_id], data);
        storage_[storage_id].timeout = std::chrono::steady_clock::duration(timeout);
        storage_[storage_id].callback = callback;
        storage_[storage_id].callback_context = callback_context;
        storage_[storage_id].operation.store(
            Operation{.mode = Operation::Mode::WRITE, .state = Operation::State::WAITING},
            std::memory_order::release);
    }

    auto realtime_get_joint_actual_position() -> const std::atomic<double> (&)[5][4] {
        return pdo_read_position_;
    }

    void realtime_set_joint_target_position(const double (&positions)[5][4]) {
        operation_thread_check();

        if (realtime_controller_) [[unlikely]]
            std::terminate(); // Logically impossible, only for protection

        pdo_write_async_unchecked(true, positions, 0);
    }

    void attach_realtime_controller(device::IRealtimeController* controller, bool enable_upstream) {
        operation_thread_check();

        std::unique_ptr<device::IRealtimeController> guard(controller);

        if (realtime_controller_)
            throw std::logic_error("A realtime controller is already attached.");
        if (latency_tester_)
            throw std::logic_error("Latency testing is underway.");

        realtime_controller_ = std::move(guard);
        pdo_thread_ = std::jthread{[this, enable_upstream](const std::stop_token& stop_token) {
            pdo_thread_main(stop_token, enable_upstream);
        }};
    }

    device::IRealtimeController* detach_realtime_controller() {
        operation_thread_check();

        if (!realtime_controller_)
            throw std::logic_error("No realtime controller attached.");

        pdo_thread_.request_stop();
        pdo_thread_.join();

        return realtime_controller_.release();
    }

    void start_latency_test() {
        operation_thread_check();

        if (realtime_controller_)
            throw std::logic_error("A realtime controller is already attached.");
        if (latency_tester_)
            throw std::logic_error("Latency testing is underway.");

        auto latency_tester = std::make_unique<LatencyTester>(pdo_builder_);
        {
            std::lock_guard guard{latency_tester_mutex_};
            latency_tester_ = std::move(latency_tester);
        }

        pdo_thread_ = std::jthread{
            [this](const std::stop_token& stop_token) { latency_tester_->spin(stop_token); }};
    }

    void stop_latency_test() {
        operation_thread_check();

        if (!latency_tester_)
            throw std::logic_error("Latency testing is not started.");

        pdo_thread_.request_stop();
        pdo_thread_.join();

        {
            std::lock_guard guard{latency_tester_mutex_};
            latency_tester_.reset();
        }
    }

    Buffer8 get(int storage_id) { return load_data(storage_[storage_id]); }

    void disable_thread_safe_check() { operation_thread_id_ = std::thread::id{}; }

    std::vector<std::byte> raw_sdo_read(
        uint16_t index, uint8_t sub_index, std::chrono::steady_clock::duration timeout) {
        operation_thread_check();

        // Find an available slot
        RawSdoUnit* unit = nullptr;
        for (auto& u : raw_sdo_units_) {
            bool expected = false;
            if (u.in_use.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
                unit = &u;
                break;
            }
        }
        if (!unit)
            throw std::runtime_error("No available raw SDO slot. Too many concurrent operations.");

        // Setup the unit
        {
            std::lock_guard lock{unit->mutex};
            unit->index = index;
            unit->sub_index = sub_index;
            unit->mode = RawSdoUnit::Mode::READ;
            unit->state = RawSdoUnit::State::PENDING;
            unit->read_result.clear();
            unit->timeout_point = std::chrono::steady_clock::now() + timeout;
        }

        // Wait for completion
        std::vector<std::byte> result;
        {
            std::unique_lock lock{unit->mutex};
            unit->cv.wait(lock, [&] {
                return unit->state == RawSdoUnit::State::SUCCESS
                    || unit->state == RawSdoUnit::State::FAILED;
            });

            if (unit->state == RawSdoUnit::State::SUCCESS) {
                result = std::move(unit->read_result);
            }
            auto state = unit->state;
            unit->state = RawSdoUnit::State::IDLE;
            unit->mode = RawSdoUnit::Mode::NONE;
            unit->in_use.store(false, std::memory_order_release);

            if (state == RawSdoUnit::State::FAILED)
                throw device::TimeoutError(
                    std::format(
                        "Raw SDO read timed out: index=0x{:04X}, sub_index={}", index, sub_index));
        }

        return result;
    }

    void raw_sdo_write(
        uint16_t index, uint8_t sub_index, std::span<const std::byte> data,
        std::chrono::steady_clock::duration timeout) {
        operation_thread_check();

        if (data.size() != 1 && data.size() != 2 && data.size() != 4 && data.size() != 8)
            throw std::invalid_argument(
                std::format(
                    "Raw SDO write data size must be 1, 2, 4, or 8 bytes, got {}", data.size()));

        // Find an available slot
        RawSdoUnit* unit = nullptr;
        for (auto& u : raw_sdo_units_) {
            bool expected = false;
            if (u.in_use.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
                unit = &u;
                break;
            }
        }
        if (!unit)
            throw std::runtime_error("No available raw SDO slot. Too many concurrent operations.");

        // Setup the unit with cached write data (actual send is done by sdo_thread)
        {
            std::lock_guard lock{unit->mutex};
            unit->index = index;
            unit->sub_index = sub_index;
            unit->mode = RawSdoUnit::Mode::WRITE;
            unit->state = RawSdoUnit::State::PENDING;
            unit->timeout_point = std::chrono::steady_clock::now() + timeout;
            // Cache write data for sdo_thread to send
            std::memcpy(unit->write_data.data(), data.data(), data.size());
            unit->write_data_size = static_cast<uint8_t>(data.size());
        }

        // Wait for completion (sdo_thread will send the write request)
        {
            std::unique_lock lock{unit->mutex};
            unit->cv.wait(lock, [&] {
                return unit->state == RawSdoUnit::State::SUCCESS
                    || unit->state == RawSdoUnit::State::FAILED;
            });

            auto state = unit->state;
            unit->state = RawSdoUnit::State::IDLE;
            unit->mode = RawSdoUnit::Mode::NONE;
            unit->in_use.store(false, std::memory_order_release);

            if (state == RawSdoUnit::State::FAILED)
                throw device::TimeoutError(
                    std::format(
                        "Raw SDO write timed out: index=0x{:04X}, sub_index={}", index, sub_index));
        }
    }

private:
    struct Operation {
        enum class Mode : uint16_t {
            NONE = 0,

            READ,
            WRITE
        } mode;
        enum class State : uint16_t {
            SUCCESS = 0,

            WAITING,

            READING,

            WRITING,
            WRITING_CONFIRMING,
        } state;
    };
    struct alignas(64) StorageUnit {
        constexpr StorageUnit()
            : version(0) {};

        StorageInfo info;

        std::atomic<Operation> operation =
            Operation{.mode = Operation::Mode::NONE, .state = Operation::State::SUCCESS};
        static_assert(decltype(StorageUnit::operation)::is_always_lock_free);

        std::atomic<uint32_t> version;
        std::atomic<Buffer8> value;
        static_assert(decltype(StorageUnit::version)::is_always_lock_free);
        static_assert(decltype(StorageUnit::value)::is_always_lock_free);

        union {
            std::chrono::steady_clock::duration timeout;
            std::chrono::steady_clock::time_point timeout_point;
            static_assert(std::is_trivially_destructible_v<decltype(timeout)>);
            static_assert(std::is_trivially_destructible_v<decltype(timeout_point)>);
        };

        void (*callback)(Buffer8 context, bool success);
        Buffer8 callback_context;
    };
    static_assert(sizeof(StorageUnit) == 64);

    // Raw SDO unit for debugging - allows arbitrary index/sub_index access
    struct RawSdoUnit {
        std::mutex mutex;
        std::condition_variable cv;

        std::atomic<bool> in_use{false};
        uint16_t index = 0;
        uint8_t sub_index = 0;

        enum class Mode : uint8_t { NONE, READ, WRITE } mode = Mode::NONE;
        enum class State : uint8_t {
            IDLE,
            PENDING,
            READING,
            WRITING,
            SUCCESS,
            FAILED
        } state = State::IDLE;

        std::vector<std::byte> read_result;
        std::chrono::steady_clock::time_point timeout_point;

        // Write data cache - used to defer write operations to sdo_thread
        std::array<std::byte, 8> write_data{};
        uint8_t write_data_size = 0;
    };

    static constexpr size_t RAW_SDO_SLOT_COUNT = 4;

    struct ErrorDefinition {
        uint8_t bit;
        const char* description;
        const char* remedy;
        logging::Level level;
    };

    static constexpr const char kDefaultRemedy[] =
        "Possible hardware damage, please contact customer service.";
    static constexpr ErrorDefinition kErrorDefinitions[]{
        { 0,                     "ADC failure",                         kDefaultRemedy, logging::Level::CRITICAL},
        { 1,      "Driver communication fault",                         kDefaultRemedy,      logging::Level::ERR},
        { 2,           "Driver fault reported",                         kDefaultRemedy,      logging::Level::ERR},
        { 3,    "Encoder1 communication fault",                         kDefaultRemedy, logging::Level::CRITICAL},
        { 4,         "Encoder1 noise detected",                         kDefaultRemedy,      logging::Level::ERR},
        { 5,                 "Bus overvoltage",                         kDefaultRemedy,      logging::Level::ERR},
        { 6,                "Bus undervoltage",                         kDefaultRemedy,      logging::Level::ERR},
        { 7,      "Transmission slip detected",                         kDefaultRemedy, logging::Level::CRITICAL},
        { 8,               "Phase overcurrent",                         kDefaultRemedy,      logging::Level::ERR},
        {13,                 "Overtemperature", "Try improve cooling and reduce load.",      logging::Level::ERR},
        {14,              "Board info invalid",                         kDefaultRemedy, logging::Level::CRITICAL},
        {16,    "Encoder2 communication error",                         kDefaultRemedy,     logging::Level::WARN},
        {17,         "Encoder2 noise detected",                         kDefaultRemedy,     logging::Level::WARN},
        {18,               "Flash erase error",                         kDefaultRemedy,     logging::Level::WARN},
        {19,              "Flash verify error",                         kDefaultRemedy,     logging::Level::WARN},
        {20,               "Flash write error",                         kDefaultRemedy,     logging::Level::WARN},
        {21, "User config verification failed",                         kDefaultRemedy,     logging::Level::WARN},
        {22, "Flash write count limit reached",                         kDefaultRemedy,     logging::Level::WARN},
    };

    void operation_thread_check() const {
        if (operation_thread_id_ == std::thread::id{})
            return;
        if (operation_thread_id_ != std::this_thread::get_id()) [[unlikely]]
            throw std::runtime_error(
                "Thread safety violation: \n"
                "  Operation must be called from the construction thread by default. \n"
                "  If you want to perform operations in multiple threads, call:\n"
                "      disable_thread_safe_check();\n"
                "  And use mutex to ensure that ONLY ONE THREAD is operating at the same time.");
    }

    static void store_data(StorageUnit& storage, Buffer8 data) {
        if (storage.info.policy & StorageInfo::CONTROL_WORD) {
            storage.value.store(
                Buffer8{static_cast<uint16_t>(data.as<bool>() ? 1 : 5)},
                std::memory_order::relaxed);
        } else if (storage.info.policy & StorageInfo::POSITION) {
            auto value = to_raw_position(data.as<double>());
            if (storage.info.policy & StorageInfo::POSITION_REVERSED)
                value = -value;
            storage.value.store(Buffer8{value}, std::memory_order::relaxed);
        } else
            storage.value.store(data, std::memory_order::relaxed);
    }

    static Buffer8 load_data(StorageUnit& storage) {
        Buffer8 data = storage.value.load(std::memory_order::relaxed);

        if (storage.info.policy & StorageInfo::CONTROL_WORD) {
            return Buffer8{data.as<uint16_t>() == 1};
        } else if (storage.info.policy & StorageInfo::POSITION) {
            auto value = extract_raw_position(data.as<int32_t>());
            if (storage.info.policy & StorageInfo::POSITION_REVERSED)
                value = -value;
            return Buffer8{value};
        }

        return data;
    }

    static int32_t to_raw_position(double angle) {
        return static_cast<int32_t>(std::round(
            std::clamp<double>(
                angle * (std::numeric_limits<int32_t>::max() / (2 * std::numbers::pi)),
                std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max())));
    }

    static constexpr double extract_raw_position(int32_t angle) {
        return angle * (2 * std::numbers::pi / std::numeric_limits<int32_t>::max());
    }

    void receive_transfer_completed_callback(const std::byte* buffer, size_t size) {
        if (logger_.should_log(logging::Level::TRACE))
            logger_.trace("RX [{} bytes] {:Xp}", size, spdlog::to_hex(buffer, buffer + size));

        auto pointer = buffer;
        auto sentinel = pointer + size;

        try {
            const auto& header = read_frame_struct<protocol::Header>(pointer, sentinel);
            if (header.type == 0x21)
                read_sdo_frame(pointer, sentinel);
            else if (header.type == 0x11)
                read_pdo_frame(pointer, sentinel);
            else
                throw std::runtime_error{std::format("Invalid header type: 0x{:02X}", header.type)};
        } catch (const std::runtime_error& ex) {
            logger_.error("RX Frame parsing failed at offset {}", pointer - buffer);
            logger_.error(ex.what());
            logger_.error(
                "RX Frame dump [{} bytes] {:Xp}", size, spdlog::to_hex(buffer, buffer + size));
        }
    }

    void read_sdo_frame(const std::byte*& pointer, const std::byte* sentinel) {
        while (pointer < sentinel) {
            auto control = static_cast<uint8_t>(*pointer);
            if (control == 0x35)
                read_sdo_operation_read_success<uint8_t>(pointer, sentinel);
            else if (control == 0x37)
                read_sdo_operation_read_success<uint16_t>(pointer, sentinel);
            else if (control == 0x39)
                read_sdo_operation_read_success<uint32_t>(pointer, sentinel);
            else if (control == 0x3D)
                read_sdo_operation_read_success<uint64_t>(pointer, sentinel);
            else if (control == 0x33)
                read_sdo_operation_read_failed(pointer, sentinel);
            else if (control == 0x21)
                read_sdo_operation_write_success(pointer, sentinel);
            else if (control == 0x23)
                read_sdo_operation_write_failed(pointer, sentinel);
            else if (control == 0x00)
                break;
            else
                throw std::runtime_error(
                    std::format("Invalid SDO command specifier: 0x{:02X}", control));
        }
    }

    template <typename T>
    void read_sdo_operation_read_success(const std::byte*& pointer, const std::byte* sentinel) {
        const auto& data =
            read_frame_struct<protocol::sdo::ReadResultSuccess<T>>(pointer, sentinel);

        // First check if this is a raw SDO operation response
        if (handle_raw_sdo_read_response(data.header.index, data.header.sub_index, data.value))
            return;

        StorageUnit& storage = find_storage_by_index(data.header.index, data.header.sub_index);
        auto operation = storage.operation.load(std::memory_order::acquire);

        logger_.debug(
            "SDO Read Success: 0x{:04X}.{} ({}), Mode={}, State={}",
            static_cast<uint16_t>(data.header.index), data.header.sub_index,
            static_cast<void*>(&storage), static_cast<int>(operation.mode),
            static_cast<int>(operation.state));

        if (operation.mode == Operation::Mode::NONE) [[unlikely]]
            return;

        if (operation.state == Operation::State::READING) {
            storage.value.store(Buffer8{data.value}, std::memory_order::relaxed);
            auto new_version = storage.version.load(std::memory_order::relaxed) + 1;
            if (new_version == 0)
                new_version = 1;
            storage.version.store(new_version, std::memory_order::release);

            operation.state = Operation::State::SUCCESS;
            storage.operation.store(operation, std::memory_order::release);
        } else if (operation.state == Operation::State::WRITING_CONFIRMING) {
            if (data.value == storage.value.load(std::memory_order::relaxed).as<T>()) {
                operation.state = Operation::State::SUCCESS;
                storage.operation.store(operation, std::memory_order::relaxed);
            } else {
                operation.state = Operation::State::WRITING;
                storage.operation.store(operation, std::memory_order::relaxed);
            }
        }
    }

    static void
        read_sdo_operation_read_failed(const std::byte*& pointer, const std::byte* sentinel) {
        read_frame_struct<protocol::sdo::ReadResultError>(pointer, sentinel);
    }

    void read_sdo_operation_write_success(const std::byte*& pointer, const std::byte* sentinel) {
        const auto& data = read_frame_struct<protocol::sdo::WriteResultSuccess>(pointer, sentinel);

        // First check if this is a raw SDO operation response
        if (handle_raw_sdo_write_response(data.header.index, data.header.sub_index))
            return;

        StorageUnit& storage = find_storage_by_index(data.header.index, data.header.sub_index);

        auto operation = storage.operation.load(std::memory_order::acquire);
        if (operation.mode == Operation::Mode::NONE) [[unlikely]]
            return;

        if (operation.state == Operation::State::WRITING) {
            operation.state = Operation::State::SUCCESS;
            storage.operation.store(operation, std::memory_order::relaxed);
        }
    }

    static void
        read_sdo_operation_write_failed(const std::byte*& pointer, const std::byte* sentinel) {
        read_frame_struct<protocol::sdo::WriteResultError>(pointer, sentinel);
    }

    StorageUnit& find_storage_by_index(uint16_t index, uint8_t sub_index) {
        auto it = index_storage_map_.find(
            std::bit_cast<uint32_t>(IndexMapKey{.index = index, .sub_index = sub_index}));
        if (it == index_storage_map_.end())
            throw std::runtime_error{std::format(
                "SDO object not found: index=0x{:04X}, sub-index=0x{:02X}", index, sub_index)};
        return *it->second;
    }

    // Thread-safe helper to handle raw SDO read response
    template <typename T>
    bool handle_raw_sdo_read_response(uint16_t index, uint8_t sub_index, T value) {
        for (auto& unit : raw_sdo_units_) {
            if (!unit.in_use.load(std::memory_order_acquire))
                continue;
            std::lock_guard lock{unit.mutex};
            if (unit.index == index && unit.sub_index == sub_index
                && unit.state == RawSdoUnit::State::READING
                && unit.mode == RawSdoUnit::Mode::READ) {
                unit.read_result.resize(sizeof(T));
                std::memcpy(unit.read_result.data(), &value, sizeof(T));
                unit.state = RawSdoUnit::State::SUCCESS;
                unit.cv.notify_one();
                return true;
            }
        }
        return false;
    }

    // Thread-safe helper to handle raw SDO write response
    bool handle_raw_sdo_write_response(uint16_t index, uint8_t sub_index) {
        for (auto& unit : raw_sdo_units_) {
            if (!unit.in_use.load(std::memory_order_acquire))
                continue;
            std::lock_guard lock{unit.mutex};
            if (unit.index == index && unit.sub_index == sub_index
                && unit.state == RawSdoUnit::State::WRITING
                && unit.mode == RawSdoUnit::Mode::WRITE) {
                unit.state = RawSdoUnit::State::SUCCESS;
                unit.cv.notify_one();
                return true;
            }
        }
        return false;
    }

    void sdo_thread_main(const std::stop_token& stop_token) {
        constexpr double update_rate = 199.0;
        constexpr auto update_period =
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double>(1.0 / update_rate));

        while (!stop_token.stop_requested()) {
            auto now = std::chrono::steady_clock::now();

            for (size_t i = 0; i < storage_unit_count_; i++) {
                auto& storage = storage_[i];
                auto operation = storage.operation.load(std::memory_order::acquire);

                if (operation.mode == Operation::Mode::NONE)
                    continue;

                if (storage.info.policy & Handler::StorageInfo::MASKED)
                    operation.state = Operation::State::SUCCESS;
                if (operation.state == Operation::State::SUCCESS) {
                    auto callback = storage.callback;
                    auto context = storage.callback_context;
                    operation.mode = Operation::Mode::NONE;
                    storage.operation.store(operation, std::memory_order::release);
                    if (callback)
                        callback(context, true);
                }

                if (operation.state == Operation::State::WAITING) {
                    if (storage.timeout < std::chrono::steady_clock::duration::zero()
                        || now > std::chrono::steady_clock::time_point::max() - storage.timeout)
                        // Treat negative or overflowed timeout as never expires
                        storage.timeout_point = std::chrono::steady_clock::time_point::max();
                    else
                        storage.timeout_point = now + storage.timeout;

                    operation.state =
                        (operation.mode == Operation::Mode::READ ? Operation::State::READING
                                                                 : Operation::State::WRITING);
                    storage.operation.store(operation, std::memory_order::relaxed);
                } else if (now >= storage.timeout_point) {
                    auto callback = storage.callback;
                    auto context = storage.callback_context;
                    operation.mode = Operation::Mode::NONE;
                    storage.operation.store(operation, std::memory_order::release);
                    if (callback)
                        callback(context, false);
                } else if (
                    operation.state == Operation::State::READING
                    || operation.state == Operation::State::WRITING_CONFIRMING) {
                    logger_.debug(
                        "SDO Read Request: 0x{:04X}.{} ({}), Mode={}, State={}",
                        static_cast<uint16_t>(storage.info.index), storage.info.sub_index,
                        static_cast<void*>(&storage), static_cast<int>(operation.mode),
                        static_cast<int>(operation.state));
                    read_async_unchecked_internal(storage.info.index, storage.info.sub_index);
                } else if (operation.state == Operation::State::WRITING) {
                    operation.state = Operation::State::WRITING_CONFIRMING;
                    storage.operation.store(operation, std::memory_order::relaxed);
                    if (storage.info.size == StorageInfo::Size::_1)
                        write_async_unchecked_internal(
                            storage.value.load(std::memory_order::relaxed).as<uint8_t>(),
                            storage.info.index, storage.info.sub_index);
                    else if (storage.info.size == StorageInfo::Size::_2)
                        write_async_unchecked_internal(
                            storage.value.load(std::memory_order::relaxed).as<uint16_t>(),
                            storage.info.index, storage.info.sub_index);
                    else if (storage.info.size == StorageInfo::Size::_4)
                        write_async_unchecked_internal(
                            storage.value.load(std::memory_order::relaxed).as<uint32_t>(),
                            storage.info.index, storage.info.sub_index);
                    else if (storage.info.size == StorageInfo::Size::_8)
                        write_async_unchecked_internal(
                            storage.value.load(std::memory_order::relaxed).as<uint64_t>(),
                            storage.info.index, storage.info.sub_index);
                }
            }

            // Process raw SDO operations
            for (auto& unit : raw_sdo_units_) {
                if (!unit.in_use.load(std::memory_order_acquire))
                    continue;

                std::lock_guard lock{unit.mutex};
                // Check timeout for PENDING, READING, or WRITING states
                if (unit.state == RawSdoUnit::State::PENDING
                    || unit.state == RawSdoUnit::State::READING
                    || unit.state == RawSdoUnit::State::WRITING) {
                    if (now >= unit.timeout_point) {
                        unit.state = RawSdoUnit::State::FAILED;
                        unit.cv.notify_one();
                        continue;
                    }
                }
                // Only send request once when in PENDING state
                if (unit.state == RawSdoUnit::State::PENDING) {
                    if (unit.mode == RawSdoUnit::Mode::READ) {
                        read_async_unchecked_internal(unit.index, unit.sub_index);
                        unit.state = RawSdoUnit::State::READING;
                    } else if (unit.mode == RawSdoUnit::Mode::WRITE) {
                        // Send write request from sdo_thread (avoids data race on sdo_builder_)
                        if (unit.write_data_size == 1) {
                            uint8_t value;
                            std::memcpy(&value, unit.write_data.data(), 1);
                            write_async_unchecked_internal(value, unit.index, unit.sub_index);
                        } else if (unit.write_data_size == 2) {
                            uint16_t value;
                            std::memcpy(&value, unit.write_data.data(), 2);
                            write_async_unchecked_internal(value, unit.index, unit.sub_index);
                        } else if (unit.write_data_size == 4) {
                            uint32_t value;
                            std::memcpy(&value, unit.write_data.data(), 4);
                            write_async_unchecked_internal(value, unit.index, unit.sub_index);
                        } else if (unit.write_data_size == 8) {
                            uint64_t value;
                            std::memcpy(&value, unit.write_data.data(), 8);
                            write_async_unchecked_internal(value, unit.index, unit.sub_index);
                        }
                        unit.state = RawSdoUnit::State::WRITING;
                    }
                }
            }

            sdo_builder_.finalize();

            std::this_thread::sleep_for(update_period);
        }
    }

    void update_pdo_positions(const int32_t (&positions)[5][4]) {
        for (int i = 0; i < 5; i++)
            for (int j = 0; j < 4; j++) {
                double value = extract_raw_position(positions[i][j]);
                if (j == 0 && i != 0)
                    value = -value;
                pdo_read_position_[i][j].store(value, std::memory_order::relaxed);
            }
    }

    void update_pdo_positions(const protocol::pdo::JointPosCurErr (&joint)[5][4]) {
        for (int i = 0; i < 5; i++)
            for (int j = 0; j < 4; j++) {
                double value = extract_raw_position(joint[i][j].position);
                if (j == 0 && i != 0)
                    value = -value;
                pdo_read_position_[i][j].store(value, std::memory_order::relaxed);
            }
    }

    void update_pdo_error_codes(const protocol::pdo::JointPosCurErr (&joint)[5][4]) {
        for (int i = 0; i < 5; i++)
            for (int j = 0; j < 4; j++) {
                auto new_code = joint[i][j].error_code;
                auto previous =
                    pdo_read_error_code_[i][j].exchange(new_code, std::memory_order::relaxed);
                handle_error_code_update(i, j, previous, new_code);
            }
    }

    void handle_error_code_update(int finger, int joint, uint32_t previous, uint32_t current) {
        if (current == previous)
            return;

        uint32_t newly_set = current & ~previous;
        if (newly_set == 0)
            return;

        for (const auto& def : kErrorDefinitions) {
            const uint32_t mask = uint32_t(1) << def.bit;
            if ((newly_set & mask) == 0)
                continue;

            log_error_event(finger, joint, def);
            newly_set &= ~mask;
        }

        if (newly_set)
            logger_.error(
                "Joint Motor F{}J{} Reports unknown exception(s): 0x{:X}", finger + 1, joint + 1,
                newly_set);
    }

    void log_error_event(int finger, int joint, const ErrorDefinition& def) {
        if (!logger_.should_log(def.level))
            return;

        logger_.log(
            def.level, "Joint Motor F{}J{} Reports an exception: {}.", finger + 1, joint + 1,
            def.description);
        logger_.log(def.level, "Hint: {}", def.remedy);
    }

    void read_pdo_frame(const std::byte*& pointer, const std::byte* sentinel) {
        const auto& header = read_frame_struct<protocol::pdo::Header>(pointer, sentinel);

        if (header.read_id == 0x01) {
            logger_.debug("TPDO 0x01 Received");
            const auto& data = read_frame_struct<protocol::pdo::CommandResult>(pointer, sentinel);
            update_pdo_positions(data.positions);

            pdo_read_result_version_.store(
                pdo_read_result_version_.load(std::memory_order::relaxed) + 1,
                std::memory_order::release);
        } else if (header.read_id == 0x02) {
            logger_.debug("TPDO 0x02 Received");
            const auto& data =
                read_frame_struct<protocol::pdo::CommandResultPosCurErr>(pointer, sentinel);
            update_pdo_positions(data.joint);
            update_pdo_error_codes(data.joint);

            pdo_read_result_version_.store(
                pdo_read_result_version_.load(std::memory_order::relaxed) + 1,
                std::memory_order::release);
        } else if (header.read_id == 0xD0) {
            const auto& data =
                read_frame_struct<protocol::pdo::LatencyTestResult>(pointer, sentinel);
            std::unique_lock guard{latency_tester_mutex_, std::try_to_lock};
            if (guard.owns_lock()) {
                if (latency_tester_)
                    latency_tester_->read_result(data);
            }
        } else
            throw std::runtime_error(
                std::format("PDO frame invalid: read_id == 0x{:02X}", header.read_id));
    }

    void pdo_thread_main(const std::stop_token& stop_token, bool upstream_enabled) {
        constexpr double update_rate = 500.0;
        realtime_controller_->setup(update_rate);

        if (upstream_enabled) {
            const uint64_t old_version = pdo_read_result_version_.load(std::memory_order::relaxed);
            utility::TickExecutor{[&](const utility::TickContext&) -> bool {
                pdo_read_async_unchecked();
                return pdo_read_result_version_.load(std::memory_order::acquire) == old_version;
            }}.spin(update_rate, stop_token);

            utility::TickExecutor{[&](const utility::TickContext& context) {
                device::IRealtimeController::JointPositions positions;
                for (int i = 0; i < 5; i++)
                    for (int j = 0; j < 4; j++)
                        positions.value[i][j] =
                            pdo_read_position_[i][j].load(std::memory_order::relaxed);

                auto target_positions = realtime_controller_->step(&positions);

                pdo_write_async_unchecked(
                    true, target_positions.value,
                    static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                              context.scheduled_update_time - context.begin_time)
                                              .count()));
            }}.spin(update_rate, stop_token);
        } else {
            utility::TickExecutor{[&](const utility::TickContext& context) {
                auto target_positions = realtime_controller_->step(nullptr);
                pdo_write_async_unchecked(
                    false, target_positions.value,
                    static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                              context.scheduled_update_time - context.begin_time)
                                              .count()));
            }}.spin(update_rate, stop_token);
        }
    }

    template <typename Struct>
    static const Struct& read_frame_struct(const std::byte*& pointer, const std::byte* sentinel) {
        static_assert(alignof(Struct) == 1);
        const std::size_t required = sizeof(Struct);
        const std::ptrdiff_t remaining = sentinel - pointer;
        if (remaining < static_cast<std::ptrdiff_t>(required)) {
            throw std::runtime_error(
                std::format(
                    "{} truncated: requires {} bytes, but {} remain", typeid(Struct).name(),
                    required, remaining));
        }

        const auto& data = *reinterpret_cast<const Struct*>(pointer);
        pointer += required;
        return data;
    }

    void read_async_unchecked_internal(uint16_t index, uint8_t sub_index) {
        std::byte* buffer = sdo_builder_.allocate(sizeof(protocol::sdo::Read));
        new (buffer) protocol::sdo::Read{
            .index = index,
            .sub_index = sub_index,
        };
    }

    template <protocol::is_type_erased_integral T>
    void write_async_unchecked_internal(T value, uint16_t index, uint8_t sub_index) {
        std::byte* buffer = sdo_builder_.allocate(sizeof(protocol::sdo::Write<T>));
        new (buffer) protocol::sdo::Write<T>{
            .index = index,
            .sub_index = sub_index,
            .value = value,
        };
    }

    void pdo_read_async_unchecked() {
        std::byte* buffer = pdo_builder_.allocate(sizeof(protocol::pdo::Read));
        new (buffer) protocol::pdo::Read{};
        pdo_builder_.finalize();
    }

    void pdo_write_async_unchecked(
        bool upstream_enabled, const double (&target_positions)[5][4], uint32_t timestamp) {
        std::byte* buffer = pdo_builder_.allocate(sizeof(protocol::pdo::Write));
        auto payload = new (buffer) protocol::pdo::Write{};
        payload->read_id = upstream_enabled ? 0x01 : 0x00;

        for (int i = 0; i < 5; i++)
            for (int j = 0; j < 4; j++) {
                payload->target_positions[i][j] = to_raw_position(target_positions[i][j]);
                if (j == 0 && i != 0)
                    payload->target_positions[i][j] = -payload->target_positions[i][j];
            }
        payload->timestamp = timestamp;

        pdo_builder_.finalize();
    }

    template <size_t size>
    using SizeToUIntType = std::conditional_t<
        size == 1, uint8_t,
        std::conditional_t<
            size == 2, uint16_t,
            std::conditional_t<
                size == 4, uint32_t, std::conditional_t<size == 8, uint64_t, void>>>>;

    logging::Logger& logger_;

    std::thread::id operation_thread_id_;

    size_t storage_unit_count_;
    std::unique_ptr<StorageUnit[]> storage_;

    struct IndexMapKey {
        uint16_t index;
        uint8_t sub_index;
        const uint8_t padding = 0;
    };
    std::map<uint32_t, StorageUnit*> index_storage_map_;

    std::atomic<double> pdo_read_position_[5][4]{};
    std::atomic<uint32_t> pdo_read_error_code_[5][4]{};
    std::atomic<uint64_t> pdo_read_result_version_ = 0;
    static_assert(std::atomic<double>::is_always_lock_free);
    static_assert(std::atomic<uint32_t>::is_always_lock_free);
    static_assert(std::atomic<uint64_t>::is_always_lock_free);

    std::unique_ptr<transport::ITransport> transport_;
    FrameBuilder sdo_builder_;
    FrameBuilder pdo_builder_;

    std::unique_ptr<LatencyTester> latency_tester_;
    std::mutex latency_tester_mutex_;

    std::jthread sdo_thread_;

    std::unique_ptr<device::IRealtimeController> realtime_controller_;
    std::jthread pdo_thread_;

    std::array<RawSdoUnit, RAW_SDO_SLOT_COUNT> raw_sdo_units_;
};

WUJIHANDCPP_API Handler::Handler(
    uint16_t usb_vid, int32_t usb_pid, const char* serial_number, size_t storage_unit_count) {
    impl_ = new Impl{
        transport::create_usb_transport(usb_vid, usb_pid, serial_number), storage_unit_count};
}

WUJIHANDCPP_API Handler::~Handler() { delete impl_; }

WUJIHANDCPP_API void Handler::init_storage_info(int storage_id, StorageInfo info) {
    impl_->init_storage_info(storage_id, info);
}

WUJIHANDCPP_API void Handler::start_transmit_receive() { impl_->start_transmit_receive(); }

WUJIHANDCPP_API void Handler::read_async_unchecked(
    int storage_id, std::chrono::steady_clock::duration::rep timeout) {
    impl_->read_async_unchecked(storage_id, timeout);
}

WUJIHANDCPP_API void Handler::read_async(
    int storage_id, std::chrono::steady_clock::duration::rep timeout,
    void (*callback)(Buffer8 context, bool success), Buffer8 callback_context) {
    impl_->read_async(storage_id, timeout, callback, callback_context);
}

WUJIHANDCPP_API void Handler::write_async_unchecked(
    Buffer8 data, int storage_id, std::chrono::steady_clock::duration::rep timeout) {
    impl_->write_async_unchecked(data, storage_id, timeout);
}

WUJIHANDCPP_API void Handler::write_async(
    Buffer8 data, int storage_id, std::chrono::steady_clock::duration::rep timeout,
    void (*callback)(Buffer8 context, bool success), Buffer8 callback_context) {
    impl_->write_async(data, storage_id, timeout, callback, callback_context);
}

WUJIHANDCPP_API auto Handler::realtime_get_joint_actual_position()
    -> const std::atomic<double> (&)[5][4] {
    return impl_->realtime_get_joint_actual_position();
}

WUJIHANDCPP_API void Handler::realtime_set_joint_target_position(const double (&positions)[5][4]) {
    impl_->realtime_set_joint_target_position(positions);
}

WUJIHANDCPP_API void Handler::attach_realtime_controller(
    device::IRealtimeController* controller, bool enable_upstream) {
    impl_->attach_realtime_controller(controller, enable_upstream);
}

WUJIHANDCPP_API device::IRealtimeController* Handler::detach_realtime_controller() {
    return impl_->detach_realtime_controller();
}

WUJIHANDCPP_API void Handler::start_latency_test() { impl_->start_latency_test(); }

WUJIHANDCPP_API void Handler::stop_latency_test() { impl_->stop_latency_test(); }

WUJIHANDCPP_API Handler::Buffer8 Handler::get(int storage_id) { return impl_->get(storage_id); }

WUJIHANDCPP_API void Handler::disable_thread_safe_check() {
    return impl_->disable_thread_safe_check();
}

WUJIHANDCPP_API std::vector<std::byte> Handler::raw_sdo_read(
    uint16_t index, uint8_t sub_index, std::chrono::steady_clock::duration timeout) {
    return impl_->raw_sdo_read(index, sub_index, timeout);
}

WUJIHANDCPP_API void Handler::raw_sdo_write(
    uint16_t index, uint8_t sub_index, std::span<const std::byte> data,
    std::chrono::steady_clock::duration timeout) {
    impl_->raw_sdo_write(index, sub_index, data, timeout);
}

} // namespace wujihandcpp::protocol
