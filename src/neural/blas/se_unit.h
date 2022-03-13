#pragma once

#include <vector>

template<bool kIsValueHead>
class GlobalPool {
public:
    GlobalPool() = delete;
    static void Forward(const size_t board_size,
                        const size_t channels,
                        const std::vector<float> &input,
                        std::vector<float> &output);

private:
    static constexpr size_t kMaxBSize = 19;
    static constexpr size_t kMinBSize = 7;
    static constexpr float kAvgBSize = (kMaxBSize + kMinBSize) / 2.f;
    static constexpr float kFactor = 0.1f;
    static constexpr float kFactorPow = kFactor * kFactor;
};

class SEUnit {
public:
    SEUnit() = delete;
    static void Forward(const size_t board_size,
                        const size_t channels,
                        const size_t se_size,
                        std::vector<float> &input,
                        const std::vector<float> &residual,
                        const std::vector<float> &weights_w1,
                        const std::vector<float> &weights_b1,
                        const std::vector<float> &weights_w2,
                        const std::vector<float> &weights_b2);

private:
    static void SEProcess(const size_t board_size,
                          const size_t channels,
                          std::vector<float> &input,
                          const std::vector<float> &residual,
                          const std::vector<float> &scale);

};
