#include "GradientBridge.hpp"

#include "MixedFilament.hpp"

#include <algorithm>
#include <cmath>

namespace Slic3r {

std::vector<double> sample_multi_filament_gradient(const std::vector<unsigned int> &components,
                                                   const std::vector<double>       &ratios,
                                                   double                           t,
                                                   double                           middle_window)
{
    std::vector<double> weights(components.size(), 0.);
    if (components.empty())
        return weights;
    if (components.size() == 1) {
        weights.front() = 1.;
        return weights;
    }

    FullSpectrum::MixedFilamentDefinition def;
    unsigned int                          max_id = 0;
    for (size_t i = 0; i < components.size(); ++i) {
        FullSpectrum::MixedFilamentWeightedComponent c;
        c.filament.id = components[i];
        c.percent     = std::max(1, int(std::lround(100. * (i < ratios.size() ? ratios[i] : 1. / components.size()))));
        def.recipe.blend.components.push_back(c);
        max_id = std::max(max_id, components[i]);
    }
    def.behavior.gradient.enabled = true;

    const FullSpectrum::MixedGradientSample s = FullSpectrum::sample_mixed_gradient(def, max_id, t, middle_window);
    auto index_of = [&](unsigned int id) { return size_t(std::find(components.begin(), components.end(), id) - components.begin()); };
    const size_t a = index_of(s.component_a);
    const size_t b = index_of(s.component_b);
    if (a >= components.size()) {
        weights.front() = 1.;
        return weights;
    }
    if (b >= components.size() || a == b || s.mix_b_percent <= 0) {
        weights[a] = 1.;
        return weights;
    }
    weights[b] = std::clamp(s.mix_b_percent / 100., 0., 1.);
    weights[a] = 1. - weights[b];
    return weights;
}

} // namespace Slic3r
