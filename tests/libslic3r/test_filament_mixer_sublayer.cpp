// Tests for the Full Spectrum minimum sublayer height port (clamp_mixed_sublayer_heights).
#include <catch2/catch.hpp>

#include "libslic3r/FilamentMixer.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <utility>

using namespace Slic3r;

namespace {

// Reference: OrcaSlicer-FullSpectrum src/libslic3r/MixedFilament/Common.cpp
// mixed_filament_local_z_pair_heights() at cc152a84dd, copied verbatim apart from EPSILON.
std::pair<double, double> fs_reference_pair_heights(double nominal_layer_height, double min_sublayer_height, int mix_b_percent)
{
    const double EPSILON = 1e-4;
    const double height  = std::max(0.0, nominal_layer_height);
    if (height <= EPSILON)
        return {0.0, 0.0};

    const int mix_b = std::clamp(mix_b_percent, 0, 100);
    if (mix_b <= 0)
        return {height, 0.0};
    if (mix_b >= 100)
        return {0.0, height};

    const double minimum = std::max(0.01, min_sublayer_height);
    if (height < 2.0 * minimum - EPSILON)
        return mix_b < 50 ? std::pair<double, double>{height, 0.0} : std::pair<double, double>{0.0, height};

    const double height_b = std::clamp(height * double(mix_b) / 100.0, minimum, height - minimum);
    return {height - height_b, height_b};
}

double sum(const std::vector<double> &v) { return std::accumulate(v.begin(), v.end(), 0.); }

} // namespace

TEST_CASE("Minimum sublayer height matches Full Spectrum for two components", "[FilamentMixer][FullSpectrum]")
{
    for (double h : {0.08, 0.1, 0.12, 0.16, 0.2, 0.24, 0.28}) {
        for (double min_h : {0.02, 0.04, 0.06, 0.08}) {
            for (int mix_b = 1; mix_b <= 99; ++mix_b) {
                const auto   ref = fs_reference_pair_heights(h, min_h, mix_b);
                const double b   = h * mix_b / 100.0;
                const auto   got = clamp_mixed_sublayer_heights({h - b, b}, min_h);
                INFO("h=" << h << " min=" << min_h << " mix_b=" << mix_b);
                REQUIRE(got.size() == 2);
                REQUIRE(got[0] == Approx(ref.first).margin(1e-6));
                REQUIRE(got[1] == Approx(ref.second).margin(1e-6));
            }
        }
    }
}

TEST_CASE("Minimum sublayer height documented example", "[FilamentMixer][FullSpectrum]")
{
    // Full Spectrum handoff doc: 0.20 mm layer, 0.06 mm minimum, 75/25 -> 0.14/0.06.
    const auto got = clamp_mixed_sublayer_heights({0.15, 0.05}, 0.06);
    REQUIRE(got[0] == Approx(0.14));
    REQUIRE(got[1] == Approx(0.06));
}

TEST_CASE("Minimum sublayer height with three or more components", "[FilamentMixer][FullSpectrum]")
{
    SECTION("disabled limit is a no-op") {
        const std::vector<double> in{0.1, 0.02, 0.08};
        REQUIRE(clamp_mixed_sublayer_heights(in, 0.) == in);
    }
    SECTION("small components are raised, the sum is preserved") {
        const auto got = clamp_mixed_sublayer_heights({0.14, 0.02, 0.04}, 0.05);
        REQUIRE(sum(got) == Approx(0.2));
        for (double h : got)
            REQUIRE(h >= 0.05 - 1e-9);
        REQUIRE(got[0] == Approx(0.10)); // the large one absorbs the difference
    }
    SECTION("too thin for all components: smallest are dropped") {
        // 0.12 mm cannot hold three 0.05 mm passes, so the smallest (0.01) is dropped.
        const auto got = clamp_mixed_sublayer_heights({0.07, 0.01, 0.04}, 0.05);
        REQUIRE(got[1] == 0.);
        REQUIRE(sum(got) == Approx(0.12));
        REQUIRE(got[0] >= 0.05);
        REQUIRE(got[2] >= 0.05 - 1e-9);
    }
    SECTION("collapses to one component when nothing else fits") {
        const auto got = clamp_mixed_sublayer_heights({0.03, 0.04, 0.01}, 0.05);
        REQUIRE(std::count_if(got.begin(), got.end(), [](double h) { return h > 0.; }) == 1);
        REQUIRE(got[1] == Approx(0.08));
    }
    SECTION("zero entries stay zero") {
        const auto got = clamp_mixed_sublayer_heights({0.0, 0.18, 0.02}, 0.05);
        REQUIRE(got[0] == 0.);
        REQUIRE(got[2] == Approx(0.05));
        REQUIRE(got[1] == Approx(0.15));
    }
}
