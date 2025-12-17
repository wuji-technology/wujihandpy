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
#include "protocol/protocol.hpp"

namespace wujihandcpp::protocol {

// VOFA+ JustFloat 格式转发器
// 将 TPDO_SCOPE_C12 数据通过 UDP 转发给 VOFA+ 软件
class VofaForwarder {
public:
    static constexpr int FINGER_COUNT = 5;
    static constexpr int JOINT_PER_FINGER = 4;
    static constexpr int TOTAL_JOINTS = FINGER_COUNT * JOINT_PER_FINGER;
    static constexpr int FLOATS_PER_JOINT = 12;
    static constexpr uint32_t VOFA_TAIL = 0x7F800000;  // VOFA JustFloat 协议尾标识

    explicit VofaForwarder()
        : logger_(logging::get_logger())
        , socket_(INVALID_SOCKET_VALUE)
        , target_port_(0)
        , joint_mask_(0xFFFFF)  // 默认所有 20 个关节都启用
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

    // 配置 VOFA 转发目标
    // ip: 目标 IP 地址 (如 "192.168.1.100")
    // port: 目标端口号
    // joint_mask: 关节掩码，bit0=finger0_joint0, bit1=finger0_joint1, ...
    bool configure(const std::string& ip, uint16_t port, uint32_t joint_mask = 0xFFFFF) {
        close_socket();

        target_ip_ = ip;
        target_port_ = port;
        joint_mask_.store(joint_mask, std::memory_order::relaxed);

        // 创建 UDP socket
        socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket_ == INVALID_SOCKET_VALUE) {
            logger_.error("VofaForwarder: 创建 UDP socket 失败");
            return false;
        }

        // 配置目标地址
        std::memset(&target_addr_, 0, sizeof(target_addr_));
        target_addr_.sin_family = AF_INET;
        target_addr_.sin_port = htons(port);
        if (inet_pton(AF_INET, ip.c_str(), &target_addr_.sin_addr) <= 0) {
            logger_.error("VofaForwarder: 无效的 IP 地址: {}", ip);
            close_socket();
            return false;
        }

        logger_.info("VofaForwarder: 配置完成 - 目标 {}:{}, 关节掩码 0x{:05X}", ip, port, joint_mask);
        return true;
    }

    // 启用/禁用转发
    void set_enabled(bool enabled) {
        enabled_.store(enabled, std::memory_order::relaxed);
        logger_.info("VofaForwarder: {}", enabled ? "已启用" : "已禁用");
    }

    bool is_enabled() const {
        return enabled_.load(std::memory_order::relaxed);
    }

    // 设置关节掩码
    void set_joint_mask(uint32_t mask) {
        joint_mask_.store(mask, std::memory_order::relaxed);
    }

    uint32_t get_joint_mask() const {
        return joint_mask_.load(std::memory_order::relaxed);
    }

    // 存储收到的 scope 数据（供 Python API 读取）
    void store_scope_data(const pdo::ScopeC12Result& data) {
        // 原子更新数据
        for (int f = 0; f < FINGER_COUNT; f++) {
            for (int j = 0; j < JOINT_PER_FINGER; j++) {
                for (int k = 0; k < FLOATS_PER_JOINT; k++) {
                    scope_data_[f][j][k].store(
                        data.joint_datas[f][j].values[k], std::memory_order::relaxed);
                }
            }
        }
        data_version_.fetch_add(1, std::memory_order::release);

        // 如果启用了转发，发送到 VOFA
        if (enabled_.load(std::memory_order::relaxed) && socket_ != INVALID_SOCKET_VALUE) {
            forward_to_vofa(data);
        }
    }

    // 获取指定关节的 scope 数据（Python API 用）
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

    // 获取所有关节的 scope 数据
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

        // 计算启用的关节数量
        int enabled_joints = std::popcount(mask);
        if (enabled_joints == 0) return;

        // 准备 VOFA JustFloat 格式数据包
        // 格式: float[N] + 0x0000807f
        size_t float_count = static_cast<size_t>(enabled_joints) * FLOATS_PER_JOINT;
        size_t packet_size = float_count * sizeof(float) + sizeof(uint32_t);

        // 使用栈上的缓冲区（最大 20*12*4 + 4 = 964 字节）
        std::array<uint8_t, 1024> buffer;
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

        // 添加 VOFA 尾部标识
        uint32_t* tail_ptr = reinterpret_cast<uint32_t*>(float_ptr + float_count);
        *tail_ptr = VOFA_TAIL;

        // 发送 UDP 数据包
        int sent = sendto(
            socket_,
            reinterpret_cast<const char*>(buffer.data()),
            static_cast<int>(packet_size),
            0,
            reinterpret_cast<const sockaddr*>(&target_addr_),
            static_cast<int>(sizeof(target_addr_)));

        if (sent < 0) {
            logger_.warn("VofaForwarder: UDP 发送失败");
        }
        (void)sent;  // 避免未使用变量警告
    }

    logging::Logger& logger_;

    socket_t socket_;
    sockaddr_in target_addr_;
    std::string target_ip_;
    uint16_t target_port_;

    std::atomic<uint32_t> joint_mask_;
    std::atomic<bool> enabled_;

    // 原子存储的 scope 数据
    std::atomic<float> scope_data_[FINGER_COUNT][JOINT_PER_FINGER][FLOATS_PER_JOINT];
    std::atomic<uint64_t> data_version_{0};
};

} // namespace wujihandcpp::protocol
