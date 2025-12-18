#pragma once

#include <atomic>
#include <stdexcept>

namespace wujihandcpp {
namespace device {

class IController {
public:
    virtual ~IController() = default;

    virtual auto get_joint_actual_position() -> const std::atomic<double> (&)[5][4] {
        throw std::logic_error("Upstream is disabled.");
    };

    virtual void set_joint_target_position(const double (&positions)[5][4]) = 0;
};

class IRealtimeController {
public:
    virtual ~IRealtimeController() noexcept = default;

    virtual void setup(double frequency) noexcept = 0;

    struct JointPositions {
        double value[5][4];
    };
    virtual JointPositions step(JointPositions* actual) noexcept = 0;
};

template <typename FilterT, bool upstream_enabled>
class FilteredController;

template <typename FilterT>
class FilteredController<FilterT, false> : public IRealtimeController {
public:
    explicit FilteredController(const double (&initial)[5][4], const FilterT& filter)
        : filter_(filter) {
        for (size_t i = 0; i < 5; ++i)
            for (size_t j = 0; j < 4; ++j)
                units_[i][j].reset(static_cast<const FilterT&>(filter_), initial[i][j]);
    }

    void setup(double frequency) noexcept override { filter_.setup(frequency); }

    JointPositions step(JointPositions* actual) noexcept override {
        (void)actual;

        JointPositions result;
        for (size_t i = 0; i < 5; i++)
            for (size_t j = 0; j < 4; j++)
                result.value[i][j] = units_[i][j].step(static_cast<const FilterT&>(filter_));
        return result;
    }

    void set(const double (&positions)[5][4]) {
        for (size_t i = 0; i < 5; i++)
            for (size_t j = 0; j < 4; j++)
                units_[i][j].input(static_cast<const FilterT&>(filter_), positions[i][j]);
    }

private:
    FilterT filter_;
    typename FilterT::Unit units_[5][4];
};

template <typename FilterT>
class FilteredController<FilterT, true> : public FilteredController<FilterT, false> {
    typedef FilteredController<FilterT, false> Base;
    typedef typename Base::JointPositions JointPositions;

public:
    explicit FilteredController(const double (&initial)[5][4], const FilterT& filter)
        : Base(initial, filter) {
        for (size_t i = 0; i < 5; ++i)
            for (size_t j = 0; j < 4; ++j)
                actual_[i][j].store(initial[i][j], std::memory_order_relaxed);
    }

    JointPositions step(JointPositions* actual) noexcept override {
        for (size_t i = 0; i < 5; i++)
            for (size_t j = 0; j < 4; j++)
                actual_[i][j].store(actual->value[i][j], std::memory_order_relaxed);

        return Base::step(actual);
    }

    auto get() -> const std::atomic<double> (&)[5][4] { return actual_; }

private:
    std::atomic<double> actual_[5][4] = {};
};

} // namespace device
} // namespace wujihandcpp
