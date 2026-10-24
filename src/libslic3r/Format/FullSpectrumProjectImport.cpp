#include "FullSpectrumProjectImport.hpp"

#include "../FilamentMixer.hpp"
#include "../FullSpectrum/MixedFilament.hpp"
#include "../LocalesUtils.hpp"

#include <algorithm>
#include <cstdio>
#include <numeric>
#include <optional>

namespace Slic3r {

namespace {

using FullSpectrum::MixedFilamentDefinition;
using FullSpectrum::MixedFilamentManager;

// Longest layer period that is stored as a cycle pattern; longer cadences fall back to ratios.
constexpr size_t k_max_pattern_period = 256;
// Layers sampled to detect the period (several periods of the longest accepted one).
constexpr size_t k_sampled_layers = 4 * k_max_pattern_period;

std::string format_ratio(double v)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.4f", v);
    return buf;
}

std::string join_ids(const std::vector<unsigned int> &ids)
{
    std::string out;
    for (size_t i = 0; i < ids.size(); ++i)
        out += (i ? "," : "") + std::to_string(ids[i]);
    return out;
}

// Ratios with every entry > 0 (as validate_mixed_filament_params requires) summing to 1.
std::string ratios_from_weights(const std::vector<double> &weights)
{
    std::vector<double> w = weights;
    for (double &v : w)
        v = std::max(v, 1e-3);
    const double total = std::accumulate(w.begin(), w.end(), 0.);
    std::string out;
    double      used = 0.;
    for (size_t i = 0; i < w.size(); ++i) {
        const double r = (i + 1 == w.size()) ? std::max(1e-4, 1. - used) : w[i] / total;
        used += r;
        out += (i ? "," : "") + format_ratio(r);
    }
    return out;
}

// Smallest period p <= k_max_pattern_period with seq[i] == seq[i % p] for all samples.
std::optional<size_t> find_period(const std::vector<unsigned int> &seq)
{
    for (size_t p = 1; p <= k_max_pattern_period && p <= seq.size(); ++p) {
        bool ok = true;
        for (size_t i = p; i < seq.size() && ok; ++i)
            ok = seq[i] == seq[i - p];
        if (ok)
            return p;
    }
    return std::nullopt;
}

} // namespace

FullSpectrumImportResult convert_full_spectrum_mixed_filaments(const FullSpectrumProjectSettings &settings,
                                                               const std::vector<std::string>    &physical_colours,
                                                               size_t                             max_slots)
{
    CNumericLocalesSetter    locale_setter;
    FullSpectrumImportResult result;
    const size_t             n = physical_colours.size();
    if (n < 2 || settings.mixed_filament_definitions.empty())
        return result;

    // Same sequence Full Spectrum runs before slicing (PrintApply.cpp, Print::apply).
    MixedFilamentManager manager;
    manager.auto_generate(physical_colours);
    manager.load_custom_entries(settings.mixed_filament_definitions, physical_colours);
    const float nominal_height = std::max(0.01f, float(settings.layer_height));
    const float min_sublayer   = std::max(0.01f, float(settings.min_sublayer_height));
    manager.apply_gradient_settings(settings.gradient_height_mode ? 1 : 0, nominal_height, min_sublayer, settings.advanced_dithering);
    manager.refresh_display_colors(physical_colours);

    const size_t visible = manager.visible_count();
    result.min_sublayer_height = min_sublayer;
    if (visible > max_slots) {
        result.dropped_rows = visible - max_slots;
        result.warnings.push_back(std::to_string(result.dropped_rows) + " Full Spectrum mixed filament(s) exceed the " +
                                  std::to_string(max_slots) + " available slots and were not imported.");
    }

    for (size_t k = 0; k < std::min(visible, max_slots); ++k) {
        const unsigned int                     virtual_id = unsigned(n + 1 + k);
        const std::optional<MixedFilamentDefinition> def_opt = manager.mixed_filament_definition_from_id(virtual_id, n);
        FullSpectrumNativeSlot                 slot;
        const std::string                      label = "Mixed filament " + std::to_string(virtual_id);
        if (!def_opt) {
            // Keep the slot so later virtual ids stay aligned.
            slot.components = "1,2";
            slot.ratios     = "0.5000,0.5000";
            result.warnings.push_back(label + ": unreadable Full Spectrum row, replaced by a 50/50 mix of filaments 1 and 2.");
            result.slots.push_back(std::move(slot));
            continue;
        }
        const MixedFilamentDefinition &def = *def_opt;

        std::vector<unsigned int> comps    = def.recipe.blend.component_ids(n);
        std::vector<int>          percents = def.recipe.blend.component_percents(n);
        if (percents.size() != comps.size())
            percents.assign(comps.size(), 1);
        slot.display_color = def.presentation.display_color;

        const bool has_pattern     = def.recipe.manual_pattern.has_value();
        const bool multi_group     = has_pattern && def.recipe.manual_pattern->groups.size() > 1;
        const bool gradient        = def.behavior.gradient.enabled && !has_pattern;
        const bool local_z         = !has_pattern && (settings.local_z_mode || gradient);
        const bool has_bias        = def.behavior.surface_bias.perimeter_modulation ||
                                     std::any_of(def.behavior.surface_bias.component_offsets_mm.begin(),
                                                 def.behavior.surface_bias.component_offsets_mm.end(), [](float v) { return v != 0.f; }) ||
                                     def.behavior.surface_bias.component_a_offset_mm != 0.f ||
                                     def.behavior.surface_bias.component_b_offset_mm != 0.f;
        if (has_bias)
            result.warnings.push_back(label + ": surface bias is not supported yet and was ignored.");

        if (gradient && comps.size() == 2) {
            // Bambu gradients run component 1's ratio from start to end over the slot's Z range,
            // clamped to [kGradientMinRatio, kGradientMaxRatio].
            auto clamp_ratio = [](float v) { return std::clamp(double(v), kGradientMinRatio, kGradientMaxRatio); };
            const double start = clamp_ratio(def.behavior.gradient.component_a_start);
            const double end   = clamp_ratio(def.behavior.gradient.component_a_end);
            if (start != double(def.behavior.gradient.component_a_start) || end != double(def.behavior.gradient.component_a_end))
                result.warnings.push_back(label + ": gradient range clamped to 10-90%.");
            if (!def.behavior.gradient.stop_positions.empty())
                result.warnings.push_back(label + ": custom gradient stop positions are approximated by a linear gradient.");
            slot.components     = join_ids(comps);
            slot.ratios         = ratios_from_weights({start, 1. - start});
            slot.gradient       = true;
            slot.gradient_range = format_ratio(start).substr(0, 4) + "," + format_ratio(end).substr(0, 4);
            slot.uses_sublayers = true;
        } else if (local_z) {
            if (gradient)
                result.warnings.push_back(label + ": gradients with more than two filaments are not supported yet; imported as a "
                                                  "fixed mix with sub-layers.");
            slot.components     = join_ids(comps);
            slot.ratios         = ratios_from_weights(std::vector<double>(percents.begin(), percents.end()));
            slot.uses_sublayers = true;
        } else {
            if (multi_group)
                result.warnings.push_back(label + ": per-perimeter cycle groups are not supported yet; imported as a layer "
                                                  "cycle of the combined sequence.");
            // Ask Full Spectrum's resolver which filament every layer gets and keep its period
            // as a native cycle pattern, so the layer sequence matches Full Spectrum exactly.
            std::vector<unsigned int> seq;
            seq.reserve(k_sampled_layers);
            for (size_t layer = 0; layer < k_sampled_layers; ++layer) {
                const float print_z = float(layer + 1) * nominal_height;
                seq.push_back(manager.resolve(virtual_id, n, int(layer), print_z, nominal_height));
            }
            const std::optional<size_t> period = find_period(seq);
            if (period) {
                std::vector<unsigned int> cycle(seq.begin(), seq.begin() + *period);
                for (unsigned int id : cycle)
                    if (std::find(comps.begin(), comps.end(), id) == comps.end()) {
                        comps.push_back(id);
                        percents.push_back(0);
                    }
                // Weights from the cycle itself, so preview colors match what is printed.
                std::vector<double> weights(comps.size(), 0.);
                for (unsigned int id : cycle)
                    weights[size_t(std::find(comps.begin(), comps.end(), id) - comps.begin())] += 1.;
                std::vector<std::vector<unsigned int>> groups{cycle};
                if (comps.size() >= 2) {
                    slot.components = join_ids(comps);
                    slot.ratios     = ratios_from_weights(weights);
                    slot.pattern    = serialize_mixed_pattern(groups);
                }
            }
            if (slot.components.empty()) {
                result.warnings.push_back(label + ": layer cadence could not be reproduced exactly; imported as a ratio mix.");
                slot.components = join_ids(comps);
                slot.ratios     = ratios_from_weights(std::vector<double>(percents.begin(), percents.end()));
            }
        }

        if (comps.size() < 2) {
            slot.components = "1,2";
            slot.ratios     = "0.5000,0.5000";
            slot.pattern.clear();
            slot.gradient = false;
            slot.gradient_range.clear();
            result.warnings.push_back(label + ": fewer than two physical components, replaced by a 50/50 mix of filaments 1 and 2.");
        }
        result.needs_sublayers |= slot.uses_sublayers;
        result.slots.push_back(std::move(slot));
    }
    return result;
}

} // namespace Slic3r
