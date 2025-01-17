#pragma once

#include <array>
#include <sstream>
#include <stdexcept>
#include <cmath>

// From the: https://www.johndcook.com/blog/cpp_phi_inverse/
static double RationalApprox(double t) {
    // Abramowitz and Stegun formula 26.2.23.
    // The absolute value of the error should be less than 4.5 e-4.
    constexpr double c[3] = {2.515517f, 0.802853f, 0.010328f};
    constexpr double d[3] = {1.432788f, 0.189269f, 0.001308f};
    return t - ((c[2]*t + c[1])*t + c[0]) / 
                   (((d[2]*t + d[1])*t + d[0])*t + 1.0);
}

// From the: https://www.johndcook.com/blog/cpp_phi_inverse/
static double NormalCdfInverse(double p) {
    if (p <= 0.0 || p >= 1.0) {
        auto err = std::ostringstream{};
        err << "Invalid input argument (" << p 
               << "); must be larger than 0 but less than 1.";
        throw std::invalid_argument(err.str());
    }

    // See article above for explanation of this section.
    if (p < 0.5) {
        // F^-1(p) = - G^-1(p)
        return -RationalApprox(std::sqrt(-2.0*log(p)));
    } else {
        // F^-1(p) = G^-1(1-p)
        return RationalApprox(std::sqrt(-2.0*log(1-p)));
    }
}

// From the: https://github.com/lightvector/KataGo/blob/master/cpp/core/fancymath.h
static double NormToTApprox(double z, double degrees_of_freedom) {
    double n = degrees_of_freedom + 2;
    if (degrees_of_freedom > 8) {
        n-=1;
        return std::sqrt(n * std::exp(z * z * (n-1.5) / ((n-1) * (n-1))) - n);
    }
    return std::sqrt(n * std::exp(z * z * (n-0.853999327911f) / ((n-1.044042304114f) * (n-0.954115472059f))) - n);
}

class LcbEntries {
public:
    LcbEntries() = default;

    void Initialize(float complement_probability);
 
    float CachedTQuantile(int v);

    static LcbEntries& Get();

private:
    static constexpr int kEntrySize = 1000;

    std::array<float, kEntrySize> z_lookup_table_;
};

inline LcbEntries& LcbEntries::Get() {
    static LcbEntries l;
    return l;
}

inline void LcbEntries::Initialize(float complement_probability) {
    const double z = NormalCdfInverse(1.f - complement_probability);

    for (int i = 0; i < kEntrySize; ++i) {
        z_lookup_table_[i] = NormToTApprox(z, i);
    }
}

inline float LcbEntries::CachedTQuantile(int v) {
    if (v < 1) {
        return z_lookup_table_[0];
    }
    if (v < kEntrySize) {
        return z_lookup_table_[v - 1];
    }
    // z approaches constant when v is high enough.
    // With default lookup table size the function is flat enough that we
    // can just return the last entry for all v bigger than it.
    return z_lookup_table_[kEntrySize - 1];
}
