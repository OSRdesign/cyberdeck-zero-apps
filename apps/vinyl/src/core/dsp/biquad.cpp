/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl's biquad (see biquad.hpp).
 */
#include "biquad.hpp"

#include <algorithm>
#include <cmath>
#include <complex>

namespace vinyl {

namespace {
constexpr double kPi = 3.14159265358979323846;
}  // namespace

BiquadCoef biquad_peaking(double fs, double f0, double q, double gain_db) {
    BiquadCoef c;
    if (!(fs > 0) || !(f0 > 0) || !(q > 0) || f0 >= fs / 2) return c; /* a pass-through */
    const double a = std::pow(10.0, gain_db / 40.0);
    const double w0 = 2 * kPi * f0 / fs;
    const double cw = std::cos(w0), alpha = std::sin(w0) / (2 * q);
    const double a0 = 1 + alpha / a;
    c.b0 = (1 + alpha * a) / a0;
    c.b1 = (-2 * cw) / a0;
    c.b2 = (1 - alpha * a) / a0;
    c.a1 = (-2 * cw) / a0;
    c.a2 = (1 - alpha / a) / a0;
    return c;
}

double biquad_response_db(const BiquadCoef &c, double f, double fs) {
    const double w = 2 * kPi * f / fs;
    const std::complex<double> z1 = std::polar(1.0, -w), z2 = z1 * z1;
    const std::complex<double> num = c.b0 + c.b1 * z1 + c.b2 * z2;
    const std::complex<double> den = 1.0 + c.a1 * z1 + c.a2 * z2;
    const double mag = std::abs(num) / std::max(1e-300, std::abs(den));
    return 20 * std::log10(std::max(1e-300, mag));
}

void biquad_process(const BiquadCoef &c, BiquadState &st, float *io, size_t frames) {
    const double b0 = c.b0, b1 = c.b1, b2 = c.b2, a1 = c.a1, a2 = c.a2;
    double s1l = st.s1l, s2l = st.s2l, s1r = st.s1r, s2r = st.s2r;
    for (size_t f = 0; f < frames; ++f) {
        const double xl = io[2 * f], xr = io[2 * f + 1];
        const double yl = b0 * xl + s1l;
        const double yr = b0 * xr + s1r;
        s1l = b1 * xl - a1 * yl + s2l;
        s1r = b1 * xr - a1 * yr + s2r;
        s2l = b2 * xl - a2 * yl;
        s2r = b2 * xr - a2 * yr;
        io[2 * f] = static_cast<float>(yl);
        io[2 * f + 1] = static_cast<float>(yr);
    }
    auto flush = [](double &v) {
        if (std::fabs(v) < 1e-30) v = 0;
    };
    flush(s1l);
    flush(s2l);
    flush(s1r);
    flush(s2r);
    st.s1l = s1l;
    st.s2l = s2l;
    st.s1r = s1r;
    st.s2r = s2r;
}

}  // namespace vinyl
