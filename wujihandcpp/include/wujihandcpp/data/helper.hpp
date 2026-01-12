#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include <string>

#include "wujihandcpp/protocol/handler.hpp"
#include "wujihandcpp/utility/api.hpp"

namespace wujihandcpp {
namespace data {

using StorageInfo = protocol::Handler::StorageInfo;

template <typename Base_, uint16_t index_, uint8_t sub_index_, typename ValueType_>
struct ReadOnlyData {
    using Base = Base_;

    ReadOnlyData() = delete;

    static constexpr bool readable = true;
    static constexpr bool writable = false;

    static constexpr uint16_t index = index_;
    static constexpr uint8_t sub_index = sub_index_;

    using ValueType = ValueType_;

    static constexpr StorageInfo info(uint32_t) {
        return StorageInfo{sizeof(ValueType), index, sub_index, 0};
    }
}; // namespace data

template <typename Base_, uint16_t index_, uint8_t sub_index_, typename ValueType_>
struct WriteOnlyData {
    using Base = Base_;

    WriteOnlyData() = delete;

    static constexpr bool readable = false;
    static constexpr bool writable = true;

    static constexpr uint16_t index = index_;
    static constexpr uint8_t sub_index = sub_index_;

    using ValueType = ValueType_;

    static constexpr StorageInfo info(uint32_t) {
        return StorageInfo{sizeof(ValueType), index, sub_index, 0};
    }
};

template <typename Base_, uint16_t index_, uint8_t sub_index_, typename ValueType_>
struct ReadWriteData {
    using Base = Base_;

    ReadWriteData() = delete;

    static constexpr bool readable = true;
    static constexpr bool writable = true;

    static constexpr uint16_t index = index_;
    static constexpr uint8_t sub_index = sub_index_;

    using ValueType = ValueType_;

    static constexpr StorageInfo info(uint32_t) {
        return StorageInfo{sizeof(ValueType), index, sub_index, 0};
    }
};

struct alignas(uint32_t) FirmwareVersionData {
    FirmwareVersionData() = default;

    explicit FirmwareVersionData(uint32_t version) {
        std::memcpy(this, &version, sizeof(FirmwareVersionData));
    }

    constexpr FirmwareVersionData(uint8_t major, uint8_t minor, uint8_t patch, char pre = '\0')
        : major(major)
        , minor(minor)
        , patch(patch)
        , pre(pre) {}

    uint8_t major;
    uint8_t minor;
    uint8_t patch;
    char pre;

    bool operator==(const FirmwareVersionData& other) const {
        return major == other.major && minor == other.minor && patch == other.patch
            && pre == other.pre;
    }

    bool operator!=(const FirmwareVersionData& other) const { return !(*this == other); }

    bool operator<(const FirmwareVersionData& other) const {
        if (major != other.major)
            return major < other.major;
        if (minor != other.minor)
            return minor < other.minor;
        if (patch != other.patch)
            return patch < other.patch;
        return pre < other.pre;
    }

    bool operator>(const FirmwareVersionData& other) const { return other < *this; }

    bool operator<=(const FirmwareVersionData& other) const {
        return *this < other || *this == other;
    }

    bool operator>=(const FirmwareVersionData& other) const {
        return *this > other || *this == other;
    }

    std::string to_string() const {
        std::string result;
        result.resize(string_length());
        write_to_string(&result[0]);
        return result;
    }

private:
    WUJIHANDCPP_API size_t string_length() const;

    WUJIHANDCPP_API void write_to_string(char* dst) const;
};
static_assert(sizeof(FirmwareVersionData) == 4, "");

} // namespace data
} // namespace wujihandcpp