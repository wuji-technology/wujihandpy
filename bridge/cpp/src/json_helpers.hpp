#pragma once

#include <atomic>
#include <cstdint>

#include <nlohmann/json.hpp>

namespace wujihand_bridge {

/// Convert a 5x4 atomic<double> array (from realtime controller) to a JSON 2D array.
inline nlohmann::json atomic_array_to_json(const std::atomic<double> (&arr)[5][4]) {
    nlohmann::json result = nlohmann::json::array();
    for (int i = 0; i < 5; i++) {
        nlohmann::json row = nlohmann::json::array();
        for (int j = 0; j < 4; j++)
            row.push_back(arr[i][j].load(std::memory_order_relaxed));
        result.push_back(std::move(row));
    }
    return result;
}

/// Convert a 5x4 double array to a JSON 2D array.
inline nlohmann::json double_array_to_json(const double (&arr)[5][4]) {
    nlohmann::json result = nlohmann::json::array();
    for (int i = 0; i < 5; i++) {
        nlohmann::json row = nlohmann::json::array();
        for (int j = 0; j < 4; j++)
            row.push_back(arr[i][j]);
        result.push_back(std::move(row));
    }
    return result;
}

/// Validate JSON is a 5x4 2D array.
inline void validate_5x4(const nlohmann::json& j, const char* func_name) {
    if (!j.is_array() || j.size() != 5)
        throw std::invalid_argument(std::string(func_name) + ": expected 5x4 array, got outer size " + std::to_string(j.size()));
    for (int i = 0; i < 5; i++) {
        if (!j[i].is_array() || j[i].size() != 4)
            throw std::invalid_argument(std::string(func_name) + ": row " + std::to_string(i) + " expected 4 elements, got " + std::to_string(j[i].size()));
    }
}

/// Parse a JSON 2D array into a double[5][4].
inline void json_to_array(const nlohmann::json& j, double (&out)[5][4]) {
    validate_5x4(j, "json_to_array");
    for (int i = 0; i < 5; i++)
        for (int k = 0; k < 4; k++)
            out[i][k] = j[i][k].get<double>();
}

/// Parse a JSON 2D array into a bool[5][4].
inline void json_to_bool_array(const nlohmann::json& j, bool (&out)[5][4]) {
    validate_5x4(j, "json_to_bool_array");
    for (int i = 0; i < 5; i++)
        for (int k = 0; k < 4; k++)
            out[i][k] = j[i][k].get<bool>();
}

/// Parse a JSON 2D array into a uint16_t[5][4].
inline void json_to_uint16_array(const nlohmann::json& j, uint16_t (&out)[5][4]) {
    validate_5x4(j, "json_to_uint16_array");
    for (int i = 0; i < 5; i++)
        for (int k = 0; k < 4; k++)
            out[i][k] = j[i][k].get<uint16_t>();
}

/// Build a JSON 2D array from per-joint scalar values (template version).
template <typename T>
inline nlohmann::json scalar_array_to_json(const T (&arr)[5][4]) {
    nlohmann::json result = nlohmann::json::array();
    for (int i = 0; i < 5; i++) {
        nlohmann::json row = nlohmann::json::array();
        for (int j = 0; j < 4; j++)
            row.push_back(arr[i][j]);
        result.push_back(std::move(row));
    }
    return result;
}

} // namespace wujihand_bridge
