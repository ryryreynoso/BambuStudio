// Multi-filament gradients: the native bridge must reproduce Full Spectrum's sample_mixed_gradient().
#include <catch2/catch.hpp>

#include "libslic3r/FullSpectrum/GradientBridge.hpp"
#include "libslic3r/FullSpectrum/MixedFilament.hpp"

#include <numeric>

using namespace Slic3r;

TEST_CASE("Multi-filament gradient walks adjacent pairs", "[FullSpectrum][Gradient]")
{
    const std::vector<unsigned int> comps{1, 2, 3};
    const std::vector<double>       ratios{1. / 3, 1. / 3, 1. / 3};

    // Bottom and top are solid first and last filament.
    REQUIRE(sample_multi_filament_gradient(comps, ratios, 0.0) == std::vector<double>{1., 0., 0.});
    REQUIRE(sample_multi_filament_gradient(comps, ratios, 1.0) == std::vector<double>{0., 0., 1.});
    // The middle filament owns a solid band around the middle stop.
    REQUIRE(sample_multi_filament_gradient(comps, ratios, 0.5) == std::vector<double>{0., 1., 0.});

    // Below the band only 1/2 blend, above it only 2/3; weights always sum to 1.
    for (double t = 0.01; t < 1.0; t += 0.01) {
        const auto w = sample_multi_filament_gradient(comps, ratios, t);
        REQUIRE(std::accumulate(w.begin(), w.end(), 0.) == Approx(1.));
        if (t < 0.39)
            REQUIRE(w[2] == 0.);
        if (t > 0.61)
            REQUIRE(w[0] == 0.);
    }
    // Monotone handover from filament 1 to filament 2 in the lower transition.
    REQUIRE(sample_multi_filament_gradient(comps, ratios, 0.1)[1] < sample_multi_filament_gradient(comps, ratios, 0.3)[1]);
}

TEST_CASE("Multi-filament gradient matches Full Spectrum sampling", "[FullSpectrum][Gradient]")
{
    const std::vector<unsigned int> comps{4, 1, 3, 2};
    const std::vector<double>       ratios{0.4, 0.2, 0.2, 0.2};

    FullSpectrum::MixedFilamentDefinition def;
    for (size_t i = 0; i < comps.size(); ++i) {
        FullSpectrum::MixedFilamentWeightedComponent c;
        c.filament.id = comps[i];
        c.percent     = int(ratios[i] * 100);
        def.recipe.blend.components.push_back(c);
    }
    def.behavior.gradient.enabled = true;

    for (int step = 0; step <= 200; ++step) {
        const double t   = step / 200.;
        const auto   ref = FullSpectrum::sample_mixed_gradient(def, 4, t, kFullSpectrumMiddleFilamentWindow);
        const auto   w   = sample_multi_filament_gradient(comps, ratios, t);
        INFO("t=" << t);
        auto weight_of = [&](unsigned int id) {
            return w[size_t(std::find(comps.begin(), comps.end(), id) - comps.begin())];
        };
        if (ref.component_a == ref.component_b || ref.mix_b_percent == 0) {
            REQUIRE(weight_of(ref.component_a) == Approx(1.));
        } else {
            REQUIRE(weight_of(ref.component_b) == Approx(ref.mix_b_percent / 100.));
            REQUIRE(weight_of(ref.component_a) == Approx(1. - ref.mix_b_percent / 100.));
        }
    }
}
