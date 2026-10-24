// Full Spectrum project import: converted native slots must resolve every layer to the same
// physical filament as Full Spectrum's own resolver (the reference implementation).
#include <catch2/catch.hpp>

#include "libslic3r/FilamentMixer.hpp"
#include "libslic3r/Format/FullSpectrumProjectImport.hpp"
#include "libslic3r/FullSpectrum/MixedFilament.hpp"
#include "libslic3r/miniz_extension.hpp"

#include <boost/filesystem.hpp>

#include <algorithm>

using namespace Slic3r;
using FullSpectrum::MixedFilamentManager;

namespace {

const std::vector<std::string> k_colours{"#FF0000", "#00FF00", "#0000FF", "#FFFFFF"};

// Serialize a manager the way a Full Spectrum project stores it ("mixed_filament_definitions").
std::string serialize(MixedFilamentManager &mgr) { return mgr.serialize_custom_entries(); }

// Native resolution of a converted slot on an object layer (cycle mode only).
unsigned int native_resolve(const FullSpectrumNativeSlot &slot, size_t layer)
{
    std::vector<std::vector<unsigned int>> groups;
    REQUIRE(parse_mixed_pattern(slot.pattern, parse_mixed_components(slot.components), groups));
    return resolve_mixed_pattern(mixed_pattern_layer_sequence(groups), layer);
}

// Every converted slot must be valid for Bambu's own validator.
void require_valid(const FullSpectrumImportResult &res, size_t num_physical)
{
    std::vector<unsigned char> is_mixed(num_physical, 0);
    std::vector<std::string>   comps(num_physical), ratios(num_physical), ranges(num_physical), curves(num_physical), patterns(num_physical);
    std::vector<unsigned char> gradient(num_physical, 0);
    for (const FullSpectrumNativeSlot &s : res.slots) {
        is_mixed.push_back(1);
        comps.push_back(s.components);
        ratios.push_back(s.ratios);
        gradient.push_back(s.gradient ? 1 : 0);
        ranges.push_back(s.gradient_range);
        curves.emplace_back();
        patterns.push_back(s.pattern);
    }
    const auto errors = validate_mixed_filament_params(is_mixed, comps, ratios, gradient, ranges, curves, patterns);
    for (const auto &kv : errors)
        INFO(kv.first << ": " << kv.second);
    REQUIRE(errors.empty());
}

} // namespace

TEST_CASE("Full Spectrum import reproduces layer cadence of pair mixes", "[FullSpectrumImport]")
{
    for (int mix_b : {10, 25, 33, 50, 67, 80}) {
        MixedFilamentManager mgr;
        mgr.auto_generate(k_colours);
        mgr.add_custom_filament(1, 3, mix_b, k_colours);

        FullSpectrumProjectSettings settings;
        settings.mixed_filament_definitions = serialize(mgr);

        // Reference manager, prepared exactly as Full Spectrum does before slicing.
        MixedFilamentManager ref;
        ref.auto_generate(k_colours);
        ref.load_custom_entries(settings.mixed_filament_definitions, k_colours);
        ref.apply_gradient_settings(0, float(settings.layer_height), float(settings.min_sublayer_height), false);

        const FullSpectrumImportResult res = convert_full_spectrum_mixed_filaments(settings, k_colours, 28);
        REQUIRE(res.slots.size() == ref.visible_count());
        REQUIRE_FALSE(res.needs_sublayers);
        require_valid(res, k_colours.size());

        for (size_t k = 0; k < res.slots.size(); ++k) {
            const unsigned int virtual_id = unsigned(k_colours.size() + 1 + k);
            INFO("mix_b=" << mix_b << " virtual id " << virtual_id << " pattern " << res.slots[k].pattern);
            REQUIRE_FALSE(res.slots[k].pattern.empty());
            for (size_t layer = 0; layer < 200; ++layer)
                REQUIRE(native_resolve(res.slots[k], layer) ==
                        ref.resolve(virtual_id, k_colours.size(), int(layer), float(layer + 1) * 0.2f, 0.2f));
        }
    }
}

TEST_CASE("Full Spectrum import keeps manual patterns exact", "[FullSpectrumImport]")
{
    MixedFilamentManager mgr;
    mgr.auto_generate(k_colours);
    mgr.add_custom_filament(2, 4, 50, k_colours);
    mgr.mixed_filaments().back().manual_pattern = "1121";
    FullSpectrumProjectSettings settings;
    settings.mixed_filament_definitions = serialize(mgr);

    const FullSpectrumImportResult res = convert_full_spectrum_mixed_filaments(settings, k_colours, 28);
    REQUIRE_FALSE(res.slots.empty());
    require_valid(res, k_colours.size());
    // "1121" in Full Spectrum's legacy encoding is A A B A with A = 2 and B = 4.
    REQUIRE(res.slots.back().pattern == "2/2/4/2");
}

TEST_CASE("Full Spectrum import maps Local-Z mixes and gradients to sub-layers", "[FullSpectrumImport]")
{
    MixedFilamentManager mgr;
    mgr.auto_generate(k_colours);
    mgr.add_custom_filament(1, 2, 30, k_colours);
    mgr.mixed_filaments().back().gradient_enabled = true;
    mgr.mixed_filaments().back().gradient_start   = 0.85f;
    mgr.mixed_filaments().back().gradient_end     = 0.15f;

    FullSpectrumProjectSettings settings;
    settings.mixed_filament_definitions = serialize(mgr);
    settings.local_z_mode               = true;
    settings.min_sublayer_height        = 0.05;

    const FullSpectrumImportResult res = convert_full_spectrum_mixed_filaments(settings, k_colours, 28);
    REQUIRE(res.needs_sublayers);
    REQUIRE(res.min_sublayer_height == Approx(0.05));
    require_valid(res, k_colours.size());
    const FullSpectrumNativeSlot &g = res.slots.back();
    REQUIRE(g.gradient);
    REQUIRE(g.components == "1,2");
    REQUIRE(g.gradient_range == "0.85,0.15");
    // Ordinary rows use sub-layer ratio mode when Local-Z is on for all mixes.
    REQUIRE(res.slots.front().uses_sublayers);
    REQUIRE(res.slots.front().pattern.empty());
}

TEST_CASE("Full Spectrum import respects the slot limit and keeps ids aligned", "[FullSpectrumImport]")
{
    MixedFilamentManager mgr;
    mgr.auto_generate(k_colours); // 6 auto pairs for 4 colours
    FullSpectrumProjectSettings settings;
    settings.mixed_filament_definitions = serialize(mgr);

    const FullSpectrumImportResult all = convert_full_spectrum_mixed_filaments(settings, k_colours, 28);
    REQUIRE(all.slots.size() == 6);
    const FullSpectrumImportResult capped = convert_full_spectrum_mixed_filaments(settings, k_colours, 4);
    REQUIRE(capped.slots.size() == 4);
    REQUIRE(capped.dropped_rows == 2);
    REQUIRE_FALSE(capped.warnings.empty());
    for (size_t k = 0; k < capped.slots.size(); ++k)
        REQUIRE(capped.slots[k].components == all.slots[k].components);
}

TEST_CASE("Full Spectrum import of an empty project is a no-op", "[FullSpectrumImport]")
{
    FullSpectrumProjectSettings settings;
    REQUIRE(convert_full_spectrum_mixed_filaments(settings, k_colours, 28).slots.empty());
    settings.mixed_filament_definitions = "1,2,1,0,50";
    REQUIRE(convert_full_spectrum_mixed_filaments(settings, {"#FF0000"}, 28).slots.empty());
}

TEST_CASE("Full Spectrum settings are read from a 3MF project", "[FullSpectrumImport]")
{
    namespace fs = boost::filesystem;
    const fs::path path = fs::temp_directory_path() / fs::unique_path("fs_import_%%%%%%%%.3mf");
    const std::string json = R"({
        "name": "project_settings",
        "filament_colour": ["#FF0000", "#00FF00", "#0000FF"],
        "layer_height": "0.16",
        "dithering_local_z_mode": "1",
        "mixed_filament_gradient_mode": "0",
        "mixed_filament_advanced_dithering": "1",
        "mixed_filament_height_lower_bound": "0.05",
        "mixed_filament_definitions": "1,2,1,1,40"
    })";
    {
        mz_zip_archive zip;
        mz_zip_zero_struct(&zip);
        REQUIRE(open_zip_writer(&zip, path.string()));
        REQUIRE(mz_zip_writer_add_mem(&zip, "Metadata/project_settings.config", json.data(), json.size(), MZ_DEFAULT_COMPRESSION));
        REQUIRE(mz_zip_writer_finalize_archive(&zip));
        close_zip_writer(&zip);
    }
    FullSpectrumProjectSettings settings;
    std::vector<std::string>    colours;
    REQUIRE(read_full_spectrum_project_settings(path.string(), settings, colours));
    fs::remove(path);
    REQUIRE(settings.mixed_filament_definitions == "1,2,1,1,40");
    REQUIRE(settings.local_z_mode);
    REQUIRE_FALSE(settings.gradient_height_mode);
    REQUIRE(settings.advanced_dithering);
    REQUIRE(settings.layer_height == Approx(0.16));
    REQUIRE(settings.min_sublayer_height == Approx(0.05));
    REQUIRE(colours == std::vector<std::string>{"#FF0000", "#00FF00", "#0000FF"});

    REQUIRE_FALSE(read_full_spectrum_project_settings((fs::temp_directory_path() / "does_not_exist.3mf").string(), settings, colours));
}
