#pragma once

#include <atomic>

namespace wujihandcpp {
namespace filter {

class LowPass {
public:
    class Unit {
    public:
        Unit() = default;

        void reset(const LowPass&, double initial = 0.0) noexcept {
            inbox_.store(initial, std::memory_order_relaxed);
            output_ = initial;
        }

        void input(const LowPass&, double value) noexcept {
            inbox_.store(value, std::memory_order_relaxed);
        }

        double step(const LowPass& context) noexcept {
            const double x = inbox_.load(std::memory_order_relaxed);
            output_ = context.alpha_ * x + (1.0 - context.alpha_) * output_;
            return output_;
        }

    private:
        std::atomic<double> inbox_;
        double output_;
    };

    explicit LowPass(double cutoff_freq) noexcept
        : cutoff_freq_(cutoff_freq){};

    double cutoff_freq() const noexcept { return cutoff_freq_; }

    static double calculate_alpha(double cutoff_freq, double sampling_freq) {
        const double pi = 3.141592653589793;
        double dt = 1.0 / sampling_freq;
        double rc = 1.0 / (2 * pi * cutoff_freq);
        return dt / (dt + rc);
    }

    void setup(double sampling_freq) noexcept {
        alpha_ = calculate_alpha(cutoff_freq_, sampling_freq);
    }

private:
    const double cutoff_freq_;
    double alpha_;
};

} // namespace filter
} // namespace wujihandcpp
