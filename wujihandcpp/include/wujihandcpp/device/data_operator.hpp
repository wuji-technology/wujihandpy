#pragma once

#include <cstdint>

#include <chrono>
#include <type_traits>

#include "wujihandcpp/device/latch.hpp"
#include "wujihandcpp/protocol/handler.hpp"

#if __cplusplus >= 202002L
# define SDK_CPP20_REQUIRES(...) requires(__VA_ARGS__)
#else
# define SDK_CPP20_REQUIRES(...)
#endif

namespace wujihandcpp {
namespace device {

template <typename T>
class DataOperator {
    using Handler = protocol::Handler;
    using Buffer8 = protocol::Handler::Buffer8;
    using StorageInfo = protocol::Handler::StorageInfo;

    template <typename U>
    friend class DataOperator;

    template <typename Data, typename F>
    typename std::enable_if<std::is_same<typename Data::Base, T>::value>::type iterate(F&& f) {
        T& self = *static_cast<T*>(this);
        f(self.storage_offset_ + T::Datas::template index<Data>());
    }

    template <typename Data, typename F>
    typename std::enable_if<!std::is_same<typename Data::Base, T>::value>::type iterate(F&& f) {
        T& self = *static_cast<T*>(this);
        for (int i = 0; i < T::sub_count_; i++)
            self.sub(i).template iterate<Data>(f);
    }

public:
    static std::chrono::steady_clock::duration default_timeout() {
        return std::chrono::milliseconds(500);
    }

    template <typename Data>
    SDK_CPP20_REQUIRES(Data::readable)
    auto read(std::chrono::steady_clock::duration timeout = default_timeout()) ->
        typename std::enable_if<
            std::is_same<typename Data::Base, T>::value, typename Data::ValueType>::type {
        static_assert(Data::readable, "");

        Latch latch;
        read_async<Data>(latch, timeout);
        latch.wait();
        return get<Data>();
    }

    template <typename Data>
    SDK_CPP20_REQUIRES(Data::readable)
    auto read(std::chrono::steady_clock::duration timeout = default_timeout()) ->
        typename std::enable_if<!std::is_same<typename Data::Base, T>::value, void>::type {
        static_assert(Data::readable, "");

        Latch latch;
        read_async<Data>(latch, timeout);
        latch.wait();
    }

    template <typename Data>
    SDK_CPP20_REQUIRES(Data::readable)
    void read_async(Latch& latch, std::chrono::steady_clock::duration timeout = default_timeout()) {
        static_assert(Data::readable, "");

        Handler& handler = static_cast<T*>(this)->handler_;
        iterate<Data>([&](int storage_id) {
            latch.count_up();

            Buffer8 callback_context{&latch};
            handler.read_async(
                storage_id, timeout.count(),
                [](Buffer8 context, bool success) { (context.as<Latch*>())->count_down(success); },
                callback_context);
        });
    }

    template <typename Data, typename F>
    SDK_CPP20_REQUIRES(
        Data::readable && sizeof(F) <= 8 && alignof(F) <= 8
        && std::is_trivially_copyable_v<F> && std::is_trivially_destructible_v<F>
        && requires(bool success, const F& f) { f(success); })
    void read_async(const F& f, std::chrono::steady_clock::duration timeout = default_timeout()) {
        static_assert(Data::readable, "");

        static_assert(sizeof(F) <= 8, "");
        static_assert(alignof(F) <= 8, "");
        static_assert(std::is_trivially_copyable<F>::value, "");
        static_assert(std::is_trivially_destructible<F>::value, "");

        Handler& handler = static_cast<T*>(this)->handler_;
        iterate<Data>([&](int storage_id) {
            Buffer8 callback_context{f};
            handler.read_async(
                storage_id, timeout.count(),
                [](Buffer8 context, bool success) { context.as<F>()(success); }, callback_context);
        });
    }

    template <typename Data>
    SDK_CPP20_REQUIRES(Data::readable)
    void read_async_unchecked(std::chrono::steady_clock::duration timeout = default_timeout()) {
        static_assert(Data::readable, "");

        Handler& handler = static_cast<T*>(this)->handler_;
        iterate<Data>(
            [&](int storage_id) { handler.read_async_unchecked(storage_id, timeout.count()); });
    }

    template <typename Data>
    auto get() -> typename std::enable_if<
        std::is_same<typename Data::Base, T>::value, typename Data::ValueType>::type {

        Handler& handler = static_cast<T*>(this)->handler_;
        typename Data::ValueType value;
        iterate<Data>([&](int storage_id) {
            value = handler.get(storage_id).as<typename Data::ValueType>();
        });
        return value;
    }

    template <typename Data>
    SDK_CPP20_REQUIRES(Data::writable)
    void write(
        typename Data::ValueType value,
        std::chrono::steady_clock::duration timeout = default_timeout()) {
        static_assert(Data::writable, "");

        Latch latch;
        write_async<Data>(latch, value, timeout);
        latch.wait();
    }

    template <typename Data>
    SDK_CPP20_REQUIRES(Data::writable)
    void write_async(
        Latch& latch, typename Data::ValueType value,
        std::chrono::steady_clock::duration timeout = default_timeout()) {
        static_assert(Data::writable, "");

        Handler& handler = static_cast<T*>(this)->handler_;
        iterate<Data>([&](int storage_id) {
            latch.count_up();

            Buffer8 callback_context{&latch};
            handler.write_async(
                Buffer8{value}, storage_id, timeout.count(),
                [](Buffer8 context, bool success) { (context.as<Latch*>())->count_down(success); },
                callback_context);
        });
    }

    template <typename Data, typename F>
    SDK_CPP20_REQUIRES(
        Data::writable && sizeof(F) <= 8 && alignof(F) <= 8
        && std::is_trivially_copyable_v<F> && std::is_trivially_destructible_v<F>
        && requires(bool success, const F& f) { f(success); })
    void write_async(
        const F& f, typename Data::ValueType value,
        std::chrono::steady_clock::duration timeout = default_timeout()) {
        static_assert(Data::writable, "");

        static_assert(sizeof(F) <= 8, "");
        static_assert(alignof(F) <= 8, "");
        static_assert(std::is_trivially_copyable<F>::value, "");
        static_assert(std::is_trivially_destructible<F>::value, "");

        Handler& handler = static_cast<T*>(this)->handler_;
        iterate<Data>([&](int storage_id) {
            Buffer8 callback_context{f};
            handler.write_async(
                Buffer8{value}, storage_id, timeout.count(),
                [](Buffer8 context, bool success) { context.as<F>()(success); }, callback_context);
        });
    }

    template <typename Data>
    SDK_CPP20_REQUIRES(Data::writable)
    void write_async_unchecked(
        typename Data::ValueType value,
        std::chrono::steady_clock::duration timeout = default_timeout()) {
        static_assert(Data::writable, "");

        Handler& handler = static_cast<T*>(this)->handler_;
        iterate<Data>([&](int storage_id) {
            handler.write_async_unchecked(Buffer8{value}, storage_id, timeout.count());
        });
    }

private:
    template <typename U>
    constexpr static decltype(std::declval<typename U::Sub>(), int()) data_count_internal(int) {
        return T::Datas::count + T::sub_count_ * T::Sub::data_count();
    }

    template <typename>
    constexpr static int data_count_internal(...) {
        return T::Datas::count;
    }

    class StorageInitializer {
    public:
        explicit StorageInitializer(T& self, uint32_t mask, uint32_t i, uint32_t shape)
            : self_(self)
            , mask_(mask)
            , i_(i)
            , shape_(shape) {}

        template <int index, typename U>
        void operator()() const {
            Handler& handler = self_.handler_;
            StorageInfo info = U::info(i_);
            info.index += self_.index_offset_;

            auto flat_i = ((i_ & 0xFF00) >> 8) * (shape_ & 0xFF) + (i_ & 0xFF);
            if (shape_ && (mask_ & (1ul << flat_i)))
                info.policy |= StorageInfo::MASKED;

            handler.init_storage_info(self_.storage_offset_ + index, info);

            // std::cout << std::format(
            //     "[{:3}]: {{{:04X}, {:04X}, {:2}}} 0x{:02X}, {:2} ({}) = {:04X} {}\n",
            //     self_.storage_offset_ + index, i_, shape_, flat_i, (int)info.index,
            //     (int)info.sub_index,
            //     info.size == StorageInfo::Size::_1 ? 1
            //                                        : (info.size == StorageInfo::Size::_2   ? 2
            //                                           : info.size == StorageInfo::Size::_4 ? 4
            //                                                                                : 8),
            //     (int)info.policy, info.policy & StorageInfo::MASKED ? "(MASKED)" : "");
        }

    private:
        T& self_;
        uint32_t mask_;
        uint32_t i_, shape_;
    };

    template <typename U>
    static decltype(std::declval<typename U::Sub>(), void())
        init_storage_info_internal(U& self, uint32_t mask, uint32_t i, uint32_t shape) {
        i <<= 8;
        shape <<= 8;
        shape |= U::sub_count_;
        for (int j = 0; j < U::sub_count_; i++, j++)
            self.sub(j).init_storage_info(mask, i, shape);
    }

    template <typename U>
    static void init_storage_info_internal(...) {}

protected:
    static constexpr int data_count() { return data_count_internal<T>(0); }

    void init_storage_info(uint32_t mask, uint32_t i = 0, uint32_t shape = 0) {
        T& self = *static_cast<T*>(this);
        auto initializer = StorageInitializer(self, mask, i, shape);
        T::Datas::iterate(initializer);

        init_storage_info_internal<T>(self, mask, i, shape);
    }
};

} // namespace device
} // namespace wujihandcpp