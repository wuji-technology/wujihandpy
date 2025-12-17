#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include <array>
#include <atomic>
#include <bit>
#include <string>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using socket_t = SOCKET;
constexpr socket_t INVALID_SOCKET_VALUE = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
constexpr socket_t INVALID_SOCKET_VALUE = -1;
#endif

#include "logging/logging.hpp"
#include "protocol/scope/scope_pdo.hpp"

namespace wujihandcpp::protocol {

// VOFA+ JustFloat forwarder for TPDO_SCOPE_C12 debug data
class VofaForwarder {
public:
    static constexpr int FINGER_COUNT = 5;
    static constexpr int JOINT_PER_FINGER = 4;
    static constexpr int TOTAL_JOINTS = FINGER_COUNT * JOINT_PER_FINGER;
    static constexpr int FLOATS_PER_JOINT = 12;
    static constexpr uint32_t VOFA_TAIL = 0x7F800000;  // VOFA JustFloat tail marker

    explicit VofaForwarder()
        : logger_(logging::get_logger())
        , socket_(INVALID_SOCKET_VALUE)
        , target_port_(0)
        , joint_mask_(0xFFFFF)  // All 20 joints enabled by default
        , enabled_(false) {
#ifdef _WIN32
        WSADATA wsa_data;
        WSAStartup(MAKEWORD(2, 2), &wsa_data);
#endif
    }

    ~VofaForwarder() {
        close_socket();
#ifdef _WIN32
        WSACleanup();
#endif
    }

    // Configure VOFA forwarding target
    bool configure(const std::string& ip, uint16_t port, uint32_t joint_mask = 0xFFFFF) {
        close_socket();

        target_ip_ = ip;
        target_port_ = port;
        joint_mask_.store(joint_mask, std::memory_order::relaxed);

        socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket_ == INVALID_SOCKET_VALUE) {
            logger_.error("VofaForwarder: failed to create UDP socket");
            return false;
        }

        std::memset(&target_addr_, 0, sizeof(target_addr_));
        target_addr_.sin_family = AF_INET;
        target_addr_.sin_port = htons(port);
        if (inet_pton(AF_INET, ip.c_str(), &target_addr_.sin_addr) <= 0) {
            logger_.error("VofaForwarder: invalid IP address: {}", ip);
            close_socket();
            return false;
        }

        logger_.info("VofaForwarder: configured - target {}:{}, joint_mask 0x{:05X}", ip, port, joint_mask);
        return true;
    }

    void set_enabled(bool enabled) {
        enabled_.store(enabled, std::memory_order::relaxed);
        logger_.info("VofaForwarder: {}", enabled ? "enabled" : "disabled");
    }

    bool is_enabled() const {
        return enabled_.load(std::memory_order::relaxed);
    }

    void set_joint_mask(uint32_t mask) {
        joint_mask_.store(mask, std::memory_order::relaxed);
    }

    uint32_t get_joint_mask() const {
        return joint_mask_.load(std::memory_order::relaxed);
    }

    // Store received scope data
    void store_scope_data(const pdo::ScopeC12Result& data) {
        for (int f = 0; f < FINGER_COUNT; f++) {
            for (int j = 0; j < JOINT_PER_FINGER; j++) {
                for (int k = 0; k < FLOATS_PER_JOINT; k++) {
                    scope_data_[f][j][k].store(
                        data.joint_datas[f][j].values[k], std::memory_order::relaxed);
                }
            }
        }
        data_version_.fetch_add(1, std::memory_order::release);

        if (enabled_.load(std::memory_order::relaxed) && socket_ != INVALID_SOCKET_VALUE) {
            forward_to_vofa(data);
        }
    }

    std::array<float, FLOATS_PER_JOINT> get_joint_scope_data(int finger_id, int joint_id) const {
        std::array<float, FLOATS_PER_JOINT> result;
        if (finger_id < 0 || finger_id >= FINGER_COUNT || joint_id < 0 || joint_id >= JOINT_PER_FINGER) {
            result.fill(0.0f);
            return result;
        }
        for (int k = 0; k < FLOATS_PER_JOINT; k++) {
            result[k] = scope_data_[finger_id][joint_id][k].load(std::memory_order::relaxed);
        }
        return result;
    }

    std::array<std::array<std::array<float, FLOATS_PER_JOINT>, JOINT_PER_FINGER>, FINGER_COUNT>
    get_all_scope_data() const {
        std::array<std::array<std::array<float, FLOATS_PER_JOINT>, JOINT_PER_FINGER>, FINGER_COUNT> result;
        for (int f = 0; f < FINGER_COUNT; f++) {
            for (int j = 0; j < JOINT_PER_FINGER; j++) {
                for (int k = 0; k < FLOATS_PER_JOINT; k++) {
                    result[f][j][k] = scope_data_[f][j][k].load(std::memory_order::relaxed);
                }
            }
        }
        return result;
    }

    uint64_t get_data_version() const {
        return data_version_.load(std::memory_order::acquire);
    }

private:
    void close_socket() {
        if (socket_ != INVALID_SOCKET_VALUE) {
#ifdef _WIN32
            closesocket(socket_);
#else
            close(socket_);
#endif
            socket_ = INVALID_SOCKET_VALUE;
        }
    }

    void forward_to_vofa(const pdo::ScopeC12Result& data) {
        uint32_t mask = joint_mask_.load(std::memory_order::relaxed);

        int enabled_joints = std::popcount(mask);
        if (enabled_joints == 0) return;

        // VOFA JustFloat format: float[N] + tail
        size_t float_count = static_cast<size_t>(enabled_joints) * FLOATS_PER_JOINT;
        size_t packet_size = float_count * sizeof(float) + sizeof(uint32_t);

        std::array<uint8_t, 1024> buffer;  // Max 964 bytes
        float* float_ptr = reinterpret_cast<float*>(buffer.data());

        int joint_index = 0;
        for (int f = 0; f < FINGER_COUNT; f++) {
            for (int j = 0; j < JOINT_PER_FINGER; j++) {
                int global_joint_id = f * JOINT_PER_FINGER + j;
                if (mask & (1u << global_joint_id)) {
                    for (int k = 0; k < FLOATS_PER_JOINT; k++) {
                        float_ptr[joint_index * FLOATS_PER_JOINT + k] = data.joint_datas[f][j].values[k];
                    }
                    joint_index++;
                }
            }
        }

        uint32_t* tail_ptr = reinterpret_cast<uint32_t*>(float_ptr + float_count);
        *tail_ptr = VOFA_TAIL;

        int sent = sendto(
            socket_,
            reinterpret_cast<const char*>(buffer.data()),
            static_cast<int>(packet_size),
            0,
            reinterpret_cast<const sockaddr*>(&target_addr_),
            static_cast<int>(sizeof(target_addr_)));

        if (sent < 0) {
            logger_.warn("VofaForwarder: UDP send failed");
        }
        (void)sent;
    }

    logging::Logger& logger_;

    socket_t socket_;
    sockaddr_in target_addr_;
    std::string target_ip_;
    uint16_t target_port_;

    std::atomic<uint32_t> joint_mask_;
    std::atomic<bool> enabled_;

    std::atomic<float> scope_data_[FINGER_COUNT][JOINT_PER_FINGER][FLOATS_PER_JOINT];
    std::atomic<uint64_t> data_version_{0};
};

} // namespace wujihandcpp::protocol
