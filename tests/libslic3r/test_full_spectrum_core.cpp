// Full Spectrum core tests, ported from OrcaSlicer-FullSpectrum
// tests/libslic3r/test_mixed_filament.cpp (cc152a84dd, AGPL-3.0).
// Only the cases that exercise the vendored core (src/libslic3r/FullSpectrum) are included;
// cases that depend on OrcaSlicer's Print/Model/3MF/ToolOrdering code are not applicable here.
#include <catch2/catch.hpp>

#include "libslic3r/FullSpectrum/MixedFilament.hpp"
#include "libslic3r/FullSpectrum/MixedFilament/PerimeterModulation.hpp"

#include <algorithm>
#include <clocale>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::FullSpectrum;

namespace {

class ScopedCommaNumericLocale
{
public:
    ScopedCommaNumericLocale()
    {
        if (const char *current = std::setlocale(LC_NUMERIC, nullptr))
            m_original = current;

#ifdef _WIN32
        m_previous_thread_locale_mode = _configthreadlocale(_ENABLE_PER_THREAD_LOCALE);
#endif

        static constexpr const char *candidates[] = {
#ifdef _WIN32
            "German_Germany.1252",
            "German_Germany",
#else
            "de_DE.UTF-8",
            "de_DE.utf8",
            "de_DE",
#endif
        };

        for (const char *candidate : candidates) {
            if (std::setlocale(LC_NUMERIC, candidate) == nullptr)
                continue;
            const std::lconv *conventions = std::localeconv();
            if (conventions != nullptr && conventions->decimal_point != nullptr && conventions->decimal_point[0] == ',') {
                m_active = true;
                break;
            }
        }

        if (!m_active && !m_original.empty())
            std::setlocale(LC_NUMERIC, m_original.c_str());
    }

    ~ScopedCommaNumericLocale()
    {
        if (!m_original.empty())
            std::setlocale(LC_NUMERIC, m_original.c_str());
#ifdef _WIN32
        if (m_previous_thread_locale_mode != -1)
            _configthreadlocale(m_previous_thread_locale_mode);
#endif
    }

    bool active() const { return m_active; }

private:
    std::string m_original;
    bool        m_active = false;
#ifdef _WIN32
    int m_previous_thread_locale_mode = -1;
#endif
};

static std::vector<std::string> split_rows(const std::string &serialized)
{
    std::vector<std::string> rows;
    std::stringstream ss(serialized);
    std::string row;
    while (std::getline(ss, row, ';')) {
        if (!row.empty())
            rows.push_back(row);
    }
    return rows;
}

static std::string join_rows(const std::vector<std::string> &rows)
{
    std::ostringstream ss;
    for (size_t i = 0; i < rows.size(); ++i) {
        if (i != 0)
            ss << ';';
        ss << rows[i];
    }
    return ss.str();
}

static unsigned int virtual_id_for_stable_id(const std::vector<MixedFilamentLegacyRow> &mixed, size_t num_physical, uint64_t stable_id)
{
    unsigned int next_virtual_id = unsigned(num_physical + 1);
    for (const MixedFilamentLegacyRow &mf : mixed) {
        if (mf.deleted)
            continue;
        if (mf.stable_id == stable_id)
            return next_virtual_id;
        ++next_virtual_id;
    }
    return 0;
}

static void set_legacy_row(MixedFilamentManager &manager,
                           size_t                index,
                           const MixedFilamentLegacyRow  &row,
                           const std::vector<std::string> &colors)
{
    REQUIRE(manager.set_mixed_filament_legacy_row(index, row, colors.size(), colors));
}

struct MixedAutoGenerateGuard
{
    explicit MixedAutoGenerateGuard(bool enabled)
        : previous(MixedFilamentManager::auto_generate_enabled())
    {
        MixedFilamentManager::set_auto_generate_enabled(enabled);
    }

    ~MixedAutoGenerateGuard()
    {
        MixedFilamentManager::set_auto_generate_enabled(previous);
    }

    bool previous = true;
};

} // namespace

namespace {

/// Build a MixedFilamentManager seeded with `n` physical colours and an
/// optional list of custom mixed rows.  Returns the manager so callers can
/// inspect its mixed_filaments() vector.
static MixedFilamentManager build_manager(size_t n,
                                          const std::vector<MixedFilament> &extra_rows = {})
{
    std::vector<std::string> colours(n, "#FF0000");
    for (size_t i = 1; i < n; ++i)
        colours[i] = "#0000FF"; // placeholder
    // Disable auto-generate so m_mixed stays exactly what we build.
    MixedAutoGenerateGuard guard(false);
    MixedFilamentManager    mgr;
    // The constructor doesn't need colours; add_custom adds mixed rows.
    // We don't need add_batch — just push rows directly via the mutable accessor.
    for (const MixedFilament &row : extra_rows) {
        MixedFilament mf = row;
        mf.enabled       = true;
        mf.deleted       = false;
        mf.custom        = true;
        mf.origin_auto   = false;
        // stable_id is left 0: these tests never serialize (load_custom_entries
        // is what assigns/validates it), and compute_redundant_filaments ignores
        // it.  Tests that need stable_id (e.g. the differential oracle) set it
        // explicitly after building.
        mgr.mixed_filaments().push_back(std::move(mf));
    }
    return mgr;
}

/// Helper to build a minimal enabled mixed row for cascade tests.
static MixedFilament make_row(unsigned int a, unsigned int b,
                              const std::string &gradient_ids = {},
                              const std::string &manual_pattern_str = {})
{
    MixedFilament mf;
    mf.component_a            = a;
    mf.component_b            = b;
    mf.enabled                = true;
    mf.deleted                = false;
    mf.gradient_component_ids = gradient_ids;
    mf.manual_pattern         = manual_pattern_str;
    mf.stable_id              = 0;
    mf.custom                 = true;
    return mf;
}

} // namespace

namespace {

// Asserts the differential invariant (see comment above) for the kept_mixed rows.
// `rows` must be enabled & non-deleted; each is tagged with a unique 1-based
// stable_id so survival can be tracked across remove_physical_filament's rebuild.
void expect_kept_mixed_matches_runtime(
    size_t                           num_physical,
    const std::vector<unsigned int> &kept_physical,
    const std::vector<unsigned int> &kept_mixed,
    std::vector<MixedFilament>       rows)
{
    for (size_t i = 0; i < rows.size(); ++i)
        rows[i].stable_id = static_cast<uint64_t>(i + 1);

    // (1) compute's prediction.
    auto red = compute_redundant_filaments(num_physical, kept_physical, kept_mixed, rows);
    const std::set<unsigned int> compute_redundant(red.redundant_mixed.begin(),
                                                    red.redundant_mixed.end());

    // (2) ground truth: replay the cleanup's physical-deletion cascade.
    //     Descending order is required -- cleanup iterates red.redundant_physical
    //     (descending) and remove_physical_filament renumbers components > id,
    //     so deleting in ascending order would invalidate the higher ids still
    //     pending deletion.
    // Iterate red.redundant_physical directly: it is the exact descending, survivor-
    // floor-respecting set the real cleanup deletes (compute force-keeps phys 1 when
    // nothing is kept, so phys 1 is never in redundant_physical and the cleanup never
    // deletes it). Recomputing the delete set from kept_physical instead would delete
    // phys 1 when the floor fires, diverging from the real cleanup and making the
    // oracle compare compute against the wrong ground truth.
    auto mgr = build_manager(num_physical, rows);
    for (unsigned int pid : red.redundant_physical)
        mgr.remove_physical_filament(pid);

    std::set<uint64_t> survivor_sids;
    for (const MixedFilament &mf : mgr.mixed_filaments())
        if (mf.stable_id != 0)
            survivor_sids.insert(mf.stable_id);

    // (3) invariant: a KEPT row is flagged redundant by compute  <=>  it did NOT
    //     survive the runtime cascade. Checking only kept_mixed rows is intentional:
    //     rows outside the kept set are dropped by match policy, not by physical
    //     dependency, so remove_physical_filament has no opinion on them.
    for (unsigned int vid : kept_mixed) {
        if (vid <= num_physical) continue;           // not a virtual id
        const size_t k = vid - num_physical - 1;      // 0-based enabled-row index
        if (k >= rows.size()) continue;
        const uint64_t sid = rows[k].stable_id;
        if (sid == 0) continue;

        const bool compute_says_redundant = compute_redundant.count(vid) > 0;
        const bool survived               = survivor_sids.count(sid) > 0;
        INFO("vid=" << vid << " stable_id=" << sid
             << " compute_redundant=" << (compute_says_redundant ? "yes" : "no")
             << " survived=" << (survived ? "yes" : "no")
             << " (invariant expects: redundant == !survived)");
        CHECK(compute_says_redundant != survived);
    }
}

} // namespace

TEST_CASE("Mixed filament apparent mix percent follows the signed bias target", "[MixedFilament]")
{
    CHECK(MixedFilamentManager::apparent_mix_b_percent(50, 0.00f, 0.00f, 0.4f) == 50);
    CHECK(MixedFilamentManager::apparent_mix_b_percent(50, 0.00f, 0.02f, 0.4f) == 45);
    CHECK(MixedFilamentManager::apparent_mix_b_percent(50, 0.02f, 0.00f, 0.4f) == 55);
    CHECK(MixedFilamentManager::apparent_mix_b_percent(50, -0.02f, 0.00f, 0.4f) == 45);
    CHECK(MixedFilamentManager::apparent_mix_b_percent(50, 0.00f, -0.02f, 0.4f) == 55);
}

TEST_CASE("Local-Z minimum sublayer height clamps direct nominal-layer ratios", "[MixedFilament][LocalZ]")
{
    const auto quarter_b = mixed_filament_local_z_pair_heights(0.20, 0.04, 25);
    CHECK(quarter_b.first == Approx(0.15));
    CHECK(quarter_b.second == Approx(0.05));

    const auto clamped_b = mixed_filament_local_z_pair_heights(0.20, 0.04, 10);
    CHECK(clamped_b.first == Approx(0.16));
    CHECK(clamped_b.second == Approx(0.04));

    const auto all_a = mixed_filament_local_z_pair_heights(0.20, 0.04, 0);
    CHECK(all_a.first == Approx(0.20));
    CHECK(all_a.second == Approx(0.0));

    const auto all_b = mixed_filament_local_z_pair_heights(0.20, 0.04, 100);
    CHECK(all_b.first == Approx(0.0));
    CHECK(all_b.second == Approx(0.20));
}

TEST_CASE("Gradients opt into Local-Z without enabling the global override", "[MixedFilament][LocalZ]")
{
    MixedFilamentDefinition ordinary;
    ordinary.recipe.blend.components = {{{1}, 50}, {{2}, 50}};

    MixedFilamentDefinition gradient = ordinary;
    gradient.behavior.gradient.enabled = true;

    CHECK_FALSE(mixed_filament_definition_uses_local_z(ordinary, false));
    CHECK(mixed_filament_definition_uses_local_z(ordinary, true));
    CHECK(mixed_filament_definition_uses_local_z(gradient, false));

    gradient.recipe.manual_pattern = MixedFilamentManualPattern{{{{1}}, {{2}}}};
    CHECK_FALSE(mixed_filament_definition_uses_local_z(gradient, false));

    gradient.recipe.manual_pattern.reset();
    gradient.visibility.tombstoned = true;
    CHECK_FALSE(mixed_filament_definition_uses_local_z(gradient, true));
}

TEST_CASE("Object-assigned Local-Z rows opt into full-domain scope", "[MixedFilament][LocalZ]")
{
    CHECK_FALSE(mixed_filament_local_z_uses_full_domain(false, false));
    CHECK(mixed_filament_local_z_uses_full_domain(false, true));
    CHECK(mixed_filament_local_z_uses_full_domain(true, false));
    CHECK(mixed_filament_local_z_uses_full_domain(true, true));
}

TEST_CASE("Local-Z planner leaves physical and inactive mixed paint at nominal height", "[MixedFilament][LocalZ]")
{
    CHECK_FALSE(mixed_filament_local_z_painted_override_uses_planner(false, false));
    CHECK_FALSE(mixed_filament_local_z_painted_override_uses_planner(false, true));
    CHECK_FALSE(mixed_filament_local_z_painted_override_uses_planner(true, false));
    CHECK(mixed_filament_local_z_painted_override_uses_planner(true, true));
}

TEST_CASE("Local-Z preview reports the effective direct thickness ratio", "[MixedFilament][LocalZ]")
{
    MixedFilamentLegacyRow row;
    row.component_a   = 1;
    row.component_b   = 2;
    row.mix_b_percent = 25;
    row.custom        = true;

    const MixedFilamentDefinition definition = mixed_filament_definition_from_legacy_row(row, 2);
    MixedFilamentPreviewSettings settings;
    settings.nominal_layer_height = 0.20;
    settings.min_sublayer_height  = 0.04;
    settings.local_z_mode         = true;

    const std::vector<double> passes = mixed_filament_local_z_preview_pass_heights(0.20, 0.04, 0.0, 0.0, 25);
    REQUIRE(passes.size() == 2);
    CHECK(passes[0] == Approx(0.15));
    CHECK(passes[1] == Approx(0.05));
    CHECK(mixed_filament_effective_local_z_preview_mix_b_percent(definition, settings) == 25);

    settings.min_sublayer_height = 0.06;
    CHECK(mixed_filament_effective_local_z_preview_mix_b_percent(definition, settings) == 30);

    settings.local_z_mode = false;
    CHECK(mixed_filament_effective_local_z_preview_mix_b_percent(definition, settings) == 25);
    CHECK(mixed_filament_supports_bias_apparent_color(definition, settings, true));

    MixedFilamentDefinition gradient = definition;
    gradient.behavior.gradient.enabled = true;
    settings.nominal_layer_height          = 0.08;
    settings.gradient_nominal_layer_height = 0.20;
    settings.preferred_a_height            = 0.03;
    settings.preferred_b_height            = 0.03;
    CHECK(mixed_filament_effective_local_z_preview_mix_b_percent(gradient, settings) == 30);
    CHECK_FALSE(mixed_filament_supports_bias_apparent_color(gradient, settings, true));
}

TEST_CASE("Mixed filament bias helper maps signed bias to a one-sided safe offset pair", "[MixedFilament]")
{
    const auto [offset_a, offset_b] = MixedFilamentManager::surface_offset_pair_from_signed_bias(0.06f, 0.4f);
    CHECK_THAT(offset_a, Catch::Matchers::WithinRel(0.0f, 0.001));
    CHECK_THAT(offset_b, Catch::Matchers::WithinRel(0.06f, 0.001));

    CHECK_THAT(MixedFilamentManager::bias_ui_value_from_surface_offsets(offset_a, offset_b, 0.4f), Catch::Matchers::WithinRel(0.06f, 0.001));

    CHECK_THAT(MixedFilamentManager::bias_ui_value_from_surface_offsets(0.02f, 0.0f, 0.4f), Catch::Matchers::WithinRel(-0.02f, 0.001));
    CHECK_THAT(MixedFilamentManager::bias_ui_value_from_surface_offsets(-0.02f, 0.0f, 0.4f), Catch::Matchers::WithinRel(0.02f, 0.001));

    const auto [negative_a, negative_b] = MixedFilamentManager::surface_offset_pair_from_signed_bias(-0.06f, 0.4f);
    CHECK_THAT(negative_a, Catch::Matchers::WithinRel(0.06f, 0.001));
    CHECK_THAT(negative_b, Catch::Matchers::WithinRel(0.0f, 0.001));

    const auto [unclamped_a, unclamped_b] = MixedFilamentManager::surface_offset_pair_from_signed_bias(0.30f, 0.4f);
    CHECK_THAT(unclamped_a, Catch::Matchers::WithinRel(0.0f, 0.001));
    CHECK_THAT(unclamped_b, Catch::Matchers::WithinRel(0.30f, 0.001));

    const auto [unclamped_negative_a, unclamped_negative_b] = MixedFilamentManager::surface_offset_pair_from_signed_bias(-0.30f, 0.4f);
    CHECK_THAT(unclamped_negative_a, Catch::Matchers::WithinRel(0.30f, 0.001));
    CHECK_THAT(unclamped_negative_b, Catch::Matchers::WithinRel(0.0f, 0.001));

    const auto [clamped_a, clamped_b] = MixedFilamentManager::surface_offset_pair_from_signed_bias(0.40f, 0.4f);
    CHECK_THAT(clamped_a, Catch::Matchers::WithinRel(0.0f, 0.001));
    CHECK_THAT(clamped_b, Catch::Matchers::WithinRel(0.35f, 0.001));

    const auto [clamped_negative_a, clamped_negative_b] = MixedFilamentManager::surface_offset_pair_from_signed_bias(-0.40f, 0.4f);
    CHECK_THAT(clamped_negative_a, Catch::Matchers::WithinRel(0.35f, 0.001));
    CHECK_THAT(clamped_negative_b, Catch::Matchers::WithinRel(0.0f, 0.001));
}

TEST_CASE("Mixed filament component surface offsets follow the signed bias target across alternating layers", "[MixedFilament]")
{
    const std::vector<std::string> colors = {"#FF0000", "#FFFF00"};

    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, colors);
    REQUIRE(mgr.mixed_filament_legacy_rows().size() == 1);

    MixedFilamentLegacyRow row = mgr.mixed_filament_legacy_rows().front();
    row.manual_pattern.clear();
    row.distribution_mode = int(MixedFilamentLegacyRow::Simple);
    row.ratio_a = 1;
    row.ratio_b = 1;
    set_legacy_row(mgr, 0, row, colors);

    {
        const auto [offset_a, offset_b] = MixedFilamentManager::surface_offset_pair_from_signed_bias(0.05f, 0.4f);
        row.component_a_surface_offset = offset_a;
        row.component_b_surface_offset = offset_b;
        set_legacy_row(mgr, 0, row, colors);

        CHECK(mgr.component_surface_offset(3, 2, 0) == Approx(0.0f));
        CHECK(mgr.component_surface_offset(3, 2, 1) == Approx(0.05f));
        CHECK(mgr.component_surface_offset(3, 2, 2) == Approx(0.0f));
        CHECK(mgr.component_surface_offset(3, 2, 3) == Approx(0.05f));
    }

    {
        row.component_a_surface_offset = 0.05f;
        row.component_b_surface_offset = 0.0f;
        set_legacy_row(mgr, 0, row, colors);

        CHECK(mgr.component_surface_offset(3, 2, 0) == Approx(0.05f));
        CHECK(mgr.component_surface_offset(3, 2, 1) == Approx(0.0f));
        CHECK(mgr.component_surface_offset(3, 2, 2) == Approx(0.05f));
        CHECK(mgr.component_surface_offset(3, 2, 3) == Approx(0.0f));
    }

    {
        const auto [offset_a, offset_b] = MixedFilamentManager::surface_offset_pair_from_signed_bias(-0.05f, 0.4f);
        row.component_a_surface_offset = offset_a;
        row.component_b_surface_offset = offset_b;
        set_legacy_row(mgr, 0, row, colors);

        CHECK(mgr.component_surface_offset(3, 2, 0) == Approx(0.05f));
        CHECK(mgr.component_surface_offset(3, 2, 1) == Approx(0.0f));
        CHECK(mgr.component_surface_offset(3, 2, 2) == Approx(0.05f));
        CHECK(mgr.component_surface_offset(3, 2, 3) == Approx(0.0f));
    }
}

TEST_CASE("Multi-component mixed filament bias treats the first component against the remaining group", "[MixedFilament]")
{
    const std::vector<std::string> colors = {"#FF0000", "#FFFF00", "#0000FF"};
    MixedFilamentDefinition definition;
    definition.source.kind = MixedFilamentSourceKind::Custom;
    definition.recipe.kind = MixedFilamentRecipeKind::WeightedBlend;
    definition.recipe.blend.components = {{{1}, 40}, {{2}, 30}, {{3}, 30}};
    definition.behavior.distribution = MixedFilamentDistributionMode::Simple;

    MixedFilamentPreviewSettings preview_settings;
    CHECK(mixed_filament_supports_bias_apparent_color(definition, preview_settings, true));

    const auto [offset_a, offset_b] = MixedFilamentManager::surface_offset_pair_from_signed_bias(0.04f, 0.4f);
    definition.behavior.surface_bias.component_a_offset_mm = offset_a;
    definition.behavior.surface_bias.component_b_offset_mm = offset_b;
    CHECK(MixedFilamentManager::apparent_component_percentages({40, 30, 30}, offset_a, offset_b, 0.4f) ==
          std::vector<int>{50, 25, 25});

    MixedFilamentManager manager;
    manager.set_mixed_filament_definitions({definition}, colors);
    std::set<unsigned int> resolved_components;
    for (int layer_idx = 0; layer_idx < 20; ++layer_idx) {
        const unsigned int resolved = manager.resolve(4, 3, layer_idx);
        resolved_components.insert(resolved);
        CHECK(manager.component_surface_offset(4, 3, layer_idx) == Approx(resolved == 1 ? 0.0f : 0.04f));
    }
    CHECK(resolved_components == std::set<unsigned int>{1, 2, 3});

    const auto [negative_a, negative_b] = MixedFilamentManager::surface_offset_pair_from_signed_bias(-0.04f, 0.4f);
    definition.behavior.surface_bias.component_a_offset_mm = negative_a;
    definition.behavior.surface_bias.component_b_offset_mm = negative_b;
    CHECK(MixedFilamentManager::apparent_component_percentages({40, 30, 30}, negative_a, negative_b, 0.4f) ==
          std::vector<int>{30, 35, 35});
    manager.set_mixed_filament_definitions({definition}, colors);
    for (int layer_idx = 0; layer_idx < 20; ++layer_idx) {
        const unsigned int resolved = manager.resolve(4, 3, layer_idx);
        CHECK(manager.component_surface_offset(4, 3, layer_idx) == Approx(resolved == 1 ? 0.04f : 0.0f));
    }
}

TEST_CASE("Surface-bias encoding reproduces requested apparent component percentages", "[MixedFilament][SurfaceBias]")
{
    SECTION("two components") {
        const std::vector<int> actual = {50, 50};
        const std::vector<int> target = {75, 25};
        const std::vector<float> offsets = mixed_filament_surface_offsets_for_apparent_percentages(actual, target, 0.4f);
        REQUIRE(offsets.size() == 2);
        CHECK(offsets[0] == Approx(-0.05f));
        CHECK(offsets[1] == Approx(0.05f));
        CHECK(MixedFilamentManager::apparent_component_percentages(actual, offsets, 0.4f) == target);
    }

    SECTION("four components") {
        const std::vector<int> actual = {25, 25, 25, 25};
        const std::vector<int> target = {55, 25, 15, 5};
        const std::vector<float> offsets = mixed_filament_surface_offsets_for_apparent_percentages(actual, target, 0.4f);
        REQUIRE(offsets.size() == 4);
        CHECK(MixedFilamentManager::apparent_component_percentages(actual, offsets, 0.4f) == target);
    }

    SECTION("fractional residual keeps the normal cadence")
    {
        const std::vector<int>   base    = {60, 40};
        const std::vector<float> offsets = mixed_filament_surface_offsets_for_apparent_weights(base, {59.4, 40.6}, 0.4f);
        REQUIRE(offsets.size() == 2);
        CHECK(offsets[0] == Approx(0.0012f));
        CHECK(offsets[1] == Approx(-0.0012f));
        CHECK(std::abs(offsets[0]) < 0.01f);
        CHECK(std::abs(offsets[1]) < 0.01f);
    }

    CHECK(mixed_filament_surface_offsets_for_apparent_percentages({50, 50}, {100}, 0.4f).empty());
}

TEST_CASE("Perimeter modulation moves only centerline points near the object boundary", "[MixedFilament][PerimeterModulation]")
{
    ExPolygon slice;
    slice.contour.points = {
        Point(scale_(0.0), scale_(0.0)),
        Point(scale_(10.0), scale_(0.0)),
        Point(scale_(10.0), scale_(10.0)),
        Point(scale_(0.0), scale_(10.0))
    };
    const ExPolygons layer_slices{slice};

    Polyline contracted({Point(scale_(0.2), scale_(1.0)), Point(scale_(0.2), scale_(9.0))});
    REQUIRE(apply_mixed_filament_perimeter_modulation(contracted, layer_slices, 0.1f, 0.2f, 0.6f));
    CHECK(unscale<double>(contracted.points.front().x()) == Approx(0.3).margin(1e-4));
    CHECK(unscale<double>(contracted.points.back().x()) == Approx(0.3).margin(1e-4));

    Polyline expanded({Point(scale_(0.2), scale_(1.0)), Point(scale_(0.2), scale_(9.0))});
    REQUIRE(apply_mixed_filament_perimeter_modulation(expanded, layer_slices, -0.1f, 0.2f, 0.6f));
    CHECK(unscale<double>(expanded.points.front().x()) == Approx(0.1).margin(1e-4));

    Polyline internal({Point(scale_(5.0), scale_(1.0)), Point(scale_(5.0), scale_(9.0))});
    CHECK_FALSE(apply_mixed_filament_perimeter_modulation(internal, layer_slices, -0.1f, 0.2f, 0.6f));
    CHECK(unscale<double>(internal.points.front().x()) == Approx(5.0).margin(1e-4));
}

TEST_CASE("Perimeter-modulated recipes use strict equal layer cadence", "[MixedFilament][PerimeterModulation]")
{
    const std::vector<std::string> colors = {"#00FFFF", "#FF00FF", "#FFFF00", "#000000"};

    auto add_recipe = [&colors](MixedFilamentManager &manager, std::vector<MixedFilamentWeightedComponent> components) {
        MixedFilamentDefinition definition;
        definition.source.kind                                = MixedFilamentSourceKind::Custom;
        definition.recipe.kind                                = MixedFilamentRecipeKind::WeightedBlend;
        definition.recipe.blend.components                    = std::move(components);
        definition.behavior.distribution                      = MixedFilamentDistributionMode::LayerCycle;
        definition.behavior.surface_bias.perimeter_modulation = true;
        return manager.add_custom_filament_definition(std::move(definition), colors);
    };

    SECTION("pair is 1:1 even when exposure weights are unequal")
    {
        MixedFilamentManager manager;
        REQUIRE(add_recipe(manager, {{{1}, 85}, {{2}, 15}}));
        for (int layer = 0; layer < 8; ++layer) {
            CHECK(manager.resolve(5, colors.size(), layer) == unsigned(layer % 2 + 1));
            CHECK(manager.resolve_perimeter(5, colors.size(), layer, 0) == unsigned(layer % 2 + 1));
        }
    }

    SECTION("triple is 1:1:1")
    {
        MixedFilamentManager manager;
        REQUIRE(add_recipe(manager, {{{1}, 70}, {{2}, 20}, {{3}, 10}}));
        for (int layer = 0; layer < 12; ++layer) {
            CHECK(manager.resolve(5, colors.size(), layer) == unsigned(layer % 3 + 1));
            CHECK(manager.resolve_perimeter(5, colors.size(), layer, 1) == unsigned(layer % 3 + 1));
        }
    }

    SECTION("four-way is 1:1:1:1")
    {
        MixedFilamentManager manager;
        REQUIRE(add_recipe(manager, {{{1}, 55}, {{2}, 25}, {{3}, 15}, {{4}, 5}}));
        for (int layer = 0; layer < 16; ++layer) {
            CHECK(manager.resolve(5, colors.size(), layer) == unsigned(layer % 4 + 1));
            CHECK(manager.resolve_perimeter(5, colors.size(), layer, 2) == unsigned(layer % 4 + 1));
        }
    }
}

TEST_CASE("Mixed filament typed definition resolves legacy manual pattern tokens", "[MixedFilament]")
{
    MixedFilamentLegacyRow row;
    row.component_a = 4;
    row.component_b = 2;
    row.stable_id = 123;
    row.custom = true;
    row.origin_auto = false;
    row.mix_b_percent = 50;
    row.ratio_a = 3;
    row.ratio_b = 1;
    row.manual_pattern = MixedFilamentManager::normalize_manual_pattern("1/2, 3/1");
    row.local_z_max_sublayers = 5;
    row.component_a_surface_offset = 0.02f;
    row.component_b_surface_offset = -0.01f;
    row.display_color = "#123456";

    const MixedFilamentDefinition definition = mixed_filament_definition_from_legacy_row(row, 9);
    CHECK(definition.identity.stable_id == 123);
    CHECK(definition.source.kind == MixedFilamentSourceKind::Custom);
    CHECK(definition.recipe.kind == MixedFilamentRecipeKind::ManualPattern);
    REQUIRE(definition.recipe.blend.components.size() == 3);
    CHECK(definition.recipe.blend.components[0].filament.id == 4);
    CHECK(definition.recipe.blend.components[0].percent == 50);
    CHECK(definition.recipe.blend.components[1].filament.id == 2);
    CHECK(definition.recipe.blend.components[1].percent == 25);
    CHECK(definition.recipe.blend.components[2].filament.id == 3);
    CHECK(definition.recipe.blend.components[2].percent == 25);
    CHECK(definition.behavior.layer_cadence.component_a_layers == 3);
    CHECK(definition.behavior.layer_cadence.component_b_layers == 1);
    CHECK(definition.behavior.local_z.max_sublayers == 5);
    CHECK(definition.behavior.surface_bias.component_a_offset_mm == Approx(0.02f));
    CHECK(definition.behavior.surface_bias.component_b_offset_mm == Approx(-0.01f));
    CHECK(definition.presentation.display_color == "#123456");

    REQUIRE(definition.recipe.manual_pattern);
    REQUIRE(definition.recipe.manual_pattern->groups.size() == 2);
    REQUIRE(definition.recipe.manual_pattern->groups[0].size() == 2);
    REQUIRE(definition.recipe.manual_pattern->groups[1].size() == 2);
    CHECK(definition.recipe.manual_pattern->groups[0][0].id == 4);
    CHECK(definition.recipe.manual_pattern->groups[0][1].id == 2);
    CHECK(definition.recipe.manual_pattern->groups[1][0].id == 3);
    CHECK(definition.recipe.manual_pattern->groups[1][1].id == 4);

    CHECK(mixed_filament_manual_pattern_sequence(definition, 9) == std::vector<unsigned int>{4, 2, 3, 4});
    CHECK(mixed_filament_manual_pattern_preview_sequence(definition, 9, 2) == std::vector<unsigned int>{4, 3, 2, 4});

    const MixedFilamentLegacyRow rebuilt = mixed_filament_legacy_row_from_definition(definition);
    CHECK(rebuilt.component_a == 4);
    CHECK(rebuilt.component_b == 2);
    CHECK(rebuilt.manual_pattern == "12,31");
    CHECK(rebuilt.mix_b_percent == 25);
    CHECK(rebuilt.gradient_component_ids.empty());
    CHECK(rebuilt.gradient_component_weights.empty());
}

TEST_CASE("Mixed filament manual pattern aggregate keeps legacy primary pair", "[MixedFilament]")
{
    MixedFilamentLegacyRow row;
    row.component_a = 1;
    row.component_b = 2;
    row.mix_b_percent = 50;
    row.manual_pattern = MixedFilamentManager::normalize_manual_pattern("2,12");

    const MixedFilamentDefinition definition = mixed_filament_definition_from_legacy_row(row, 2);
    REQUIRE(definition.recipe.manual_pattern);
    const MixedFilamentPrimaryPairView pair = definition.recipe.blend.primary_pair_or(0, 0);
    CHECK(pair.component_a.id == 1);
    CHECK(pair.component_b.id == 2);
    CHECK(definition.recipe.blend.component_ids(2) == std::vector<unsigned int>{1, 2});
    CHECK(definition.recipe.blend.component_percents(2) == std::vector<int>{33, 67});

    const MixedFilamentLegacyRow rebuilt = mixed_filament_legacy_row_from_definition(definition);
    CHECK(rebuilt.component_a == 1);
    CHECK(rebuilt.component_b == 2);
    CHECK(rebuilt.manual_pattern == "2,12");
}

TEST_CASE("Mixed filament typed definition exposes weighted blend components and weights", "[MixedFilament]")
{
    MixedFilamentLegacyRow row;
    row.component_a = 1;
    row.component_b = 2;
    row.stable_id = 456;
    row.custom = true;
    row.gradient_component_ids = "312";
    row.gradient_component_weights = "50/25/25";
    row.distribution_mode = int(MixedFilamentLegacyRow::LayerCycle);

    const MixedFilamentDefinition definition = mixed_filament_definition_from_legacy_row(row, 9);
    CHECK(definition.recipe.kind == MixedFilamentRecipeKind::WeightedBlend);
    CHECK(definition.behavior.distribution == MixedFilamentDistributionMode::LayerCycle);

    REQUIRE(definition.recipe.blend.components.size() == 3);
    CHECK(definition.recipe.blend.components[0].filament.id == 1);
    CHECK(definition.recipe.blend.components[0].percent == 25);
    CHECK(definition.recipe.blend.components[1].filament.id == 2);
    CHECK(definition.recipe.blend.components[1].percent == 25);
    CHECK(definition.recipe.blend.components[2].filament.id == 3);
    CHECK(definition.recipe.blend.components[2].percent == 50);
    CHECK(definition.recipe.blend.component_ids(9) == std::vector<unsigned int>{1, 2, 3});
    CHECK(definition.recipe.blend.component_percents(9) == std::vector<int>{25, 25, 50});
    CHECK(definition.recipe.blend.component_ids(2) == std::vector<unsigned int>{1, 2});
    CHECK(definition.recipe.blend.component_percents(2) == std::vector<int>{25, 25});

    const MixedFilamentLegacyRow rebuilt = mixed_filament_legacy_row_from_definition(definition);
    CHECK(rebuilt.gradient_component_ids == "123");
    CHECK(rebuilt.gradient_component_weights == "25/25/50");
    CHECK(rebuilt.manual_pattern.empty());
}

TEST_CASE("Mixed filament legacy gradient decoding filters unavailable physical components", "[MixedFilament]")
{
    MixedFilamentLegacyRow row;
    row.component_a = 1;
    row.component_b = 2;
    row.mix_b_percent = 50;
    row.gradient_component_ids = "1235";
    row.gradient_component_weights = "10/20/30/40";
    row.distribution_mode = int(MixedFilamentLegacyRow::LayerCycle);

    MixedFilamentDefinition definition = mixed_filament_definition_from_legacy_row(row, 3);
    CHECK(definition.recipe.blend.component_ids(3) == std::vector<unsigned int>{1, 2, 3});
    CHECK(definition.recipe.blend.component_percents(3) == std::vector<int>{17, 33, 50});
    CHECK(definition.behavior.distribution == MixedFilamentDistributionMode::LayerCycle);

    row.gradient_component_ids = "125";
    row.gradient_component_weights = "10/20/70";
    definition = mixed_filament_definition_from_legacy_row(row, 2);
    CHECK(definition.recipe.blend.component_ids(2) == std::vector<unsigned int>{1, 2});
    CHECK(definition.behavior.distribution == MixedFilamentDistributionMode::LayerCycle);

    row.manual_pattern = MixedFilamentManager::normalize_manual_pattern("125");
    definition = mixed_filament_definition_from_legacy_row(row, 2);
    CHECK(mixed_filament_manual_pattern_sequence(definition, 2) == std::vector<unsigned int>{1, 2});
}

TEST_CASE("Mixed filament manager accepts typed definitions at the boundary", "[MixedFilament]")
{
    const std::vector<std::string> colors = {"#FF0000", "#00FF00", "#0000FF"};

    MixedFilamentDefinition definition;
    definition.recipe.kind = MixedFilamentRecipeKind::WeightedBlend;
    definition.recipe.blend = MixedFilamentWeightedBlend{
        {
            {MixedFilamentPhysicalRef{1}, 50},
            {MixedFilamentPhysicalRef{2}, 25},
            {MixedFilamentPhysicalRef{3}, 25}
        }
    };
    definition.behavior.distribution = MixedFilamentDistributionMode::LayerCycle;

    MixedFilamentManager mgr;
    REQUIRE(mgr.add_custom_filament_definition(definition, colors));
    REQUIRE(mgr.mixed_filament_legacy_rows().size() == 1);

    const std::vector<MixedFilamentDefinition> definitions = mgr.mixed_filament_definitions(colors.size());
    REQUIRE(definitions.size() == 1);
    CHECK(definitions.front().source.kind == MixedFilamentSourceKind::Custom);
    CHECK(definitions.front().identity.stable_id != 0);
    CHECK(definitions.front().recipe.blend.component_ids(colors.size()) == std::vector<unsigned int>{1, 2, 3});

    MixedFilamentDefinition edited = definitions.front();
    edited.recipe.blend.components = {
        {MixedFilamentPhysicalRef{1}, 35},
        {MixedFilamentPhysicalRef{2}, 65}
    };
    edited.recipe.kind = MixedFilamentRecipeKind::WeightedBlend;
    edited.behavior.distribution = MixedFilamentDistributionMode::Simple;
    REQUIRE(mgr.set_mixed_filament_definition(0, edited, colors));

    const auto roundtrip = mgr.mixed_filament_definition_from_id(4, colors.size());
    REQUIRE(roundtrip);
    CHECK(roundtrip->identity.stable_id == definitions.front().identity.stable_id);
    CHECK(roundtrip->recipe.kind == MixedFilamentRecipeKind::WeightedBlend);
    CHECK(roundtrip->recipe.blend.is_pair());
    CHECK(roundtrip->recipe.blend.primary_pair_or().component_b_percent == 65);
    CHECK(roundtrip->recipe.blend.component_ids(colors.size()) == std::vector<unsigned int>{1, 2});
}

TEST_CASE("Mixed filament perimeter resolver uses grouped manual patterns by inset", "[MixedFilament]")
{
    const std::vector<std::string> colors = {"#00FFFF", "#FF00FF"};

    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, colors);
    REQUIRE(mgr.mixed_filament_legacy_rows().size() == 1);

    MixedFilamentLegacyRow row = mgr.mixed_filament_legacy_rows().front();
    row.manual_pattern = MixedFilamentManager::normalize_manual_pattern("12,21");
    REQUIRE(row.manual_pattern == "12,21");
    set_legacy_row(mgr, 0, row, colors);

    const unsigned int mixed_filament_id = 3;
    CHECK(mgr.resolve(mixed_filament_id, 2, 0) == 1);
    CHECK(mgr.resolve(mixed_filament_id, 2, 1) == 2);

    CHECK(mgr.resolve_perimeter(mixed_filament_id, 2, 0, 0) == 1);
    CHECK(mgr.resolve_perimeter(mixed_filament_id, 2, 1, 0) == 2);
    CHECK(mgr.resolve_perimeter(mixed_filament_id, 2, 0, 1) == 2);
    CHECK(mgr.resolve_perimeter(mixed_filament_id, 2, 1, 1) == 1);
    CHECK(mgr.resolve_perimeter(mixed_filament_id, 2, 0, 3) == 2);
    CHECK(mgr.resolve_perimeter(mixed_filament_id, 2, 1, 3) == 1);

    const std::vector<unsigned int> ordered_layer0 = mgr.ordered_perimeter_extruders(mixed_filament_id, 2, 0);
    const std::vector<unsigned int> ordered_layer1 = mgr.ordered_perimeter_extruders(mixed_filament_id, 2, 1);
    REQUIRE(ordered_layer0.size() == 2);
    REQUIRE(ordered_layer1.size() == 2);
    CHECK(ordered_layer0[0] == 1);
    CHECK(ordered_layer0[1] == 2);
    CHECK(ordered_layer1[0] == 2);
    CHECK(ordered_layer1[1] == 1);
}

TEST_CASE("Grouped manual perimeter patterns keep grouped resolution on collapsed single-tool layers", "[MixedFilament]")
{
    const std::vector<std::string> colors = {"#00FFFF", "#FF00FF"};

    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, colors);
    REQUIRE(mgr.mixed_filament_legacy_rows().size() == 1);

    MixedFilamentLegacyRow row = mgr.mixed_filament_legacy_rows().front();
    row.manual_pattern = MixedFilamentManager::normalize_manual_pattern("2,12");
    REQUIRE(row.manual_pattern == "2,12");
    set_legacy_row(mgr, 0, row, colors);

    const unsigned int mixed_filament_id = 3;

    // The flattened row cadence resolves this layer to component A, but both
    // perimeter groups collapse onto physical filament 2. G-code generation
    // and tool ordering must keep using the grouped perimeter result here.
    CHECK(mgr.resolve(mixed_filament_id, 2, 1) == 1);

    const std::vector<unsigned int> ordered_layer1 = mgr.ordered_perimeter_extruders(mixed_filament_id, 2, 1);
    REQUIRE(ordered_layer1.size() == 1);
    CHECK(ordered_layer1.front() == 2);

    CHECK(mgr.resolve_perimeter(mixed_filament_id, 2, 1, 0) == 2);
    CHECK(mgr.resolve_perimeter(mixed_filament_id, 2, 1, 1) == 2);
    CHECK(mgr.resolve_perimeter(mixed_filament_id, 2, 1, 2) == 2);
}

TEST_CASE("Grouped manual perimeter patterns resolve overlapping singleton inner groups", "[MixedFilament]")
{
    const std::vector<std::string> colors = {"#00FFFF", "#FF00FF"};

    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, colors);
    REQUIRE(mgr.mixed_filament_legacy_rows().size() == 1);

    MixedFilamentLegacyRow row = mgr.mixed_filament_legacy_rows().front();
    row.manual_pattern = MixedFilamentManager::normalize_manual_pattern("12,1");
    REQUIRE(row.manual_pattern == "12,1");
    set_legacy_row(mgr, 0, row, colors);

    const unsigned int mixed_filament_id = 3;

    const std::vector<unsigned int> ordered_layer0 = mgr.ordered_perimeter_extruders(mixed_filament_id, 2, 0);
    const std::vector<unsigned int> ordered_layer1 = mgr.ordered_perimeter_extruders(mixed_filament_id, 2, 1);

    REQUIRE(ordered_layer0.size() == 1);
    CHECK(ordered_layer0.front() == 1);
    REQUIRE(ordered_layer1.size() == 2);
    CHECK(ordered_layer1[0] == 2);
    CHECK(ordered_layer1[1] == 1);

    CHECK(mgr.resolve_perimeter(mixed_filament_id, 2, 0, 0) == 1);
    CHECK(mgr.resolve_perimeter(mixed_filament_id, 2, 0, 1) == 1);
    CHECK(mgr.resolve_perimeter(mixed_filament_id, 2, 1, 0) == 2);
    CHECK(mgr.resolve_perimeter(mixed_filament_id, 2, 1, 1) == 1);
    CHECK(mgr.resolve_perimeter(mixed_filament_id, 2, 2, 0) == 1);
    CHECK(mgr.resolve_perimeter(mixed_filament_id, 2, 2, 1) == 1);
}

TEST_CASE("Mixed filament painted-region resolver collapses ordinary mixed rows to the active physical extruder", "[MixedFilament]")
{
    const std::vector<std::string> colors = {"#FF0000", "#00FF00"};

    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, colors);
    REQUIRE(mgr.mixed_filament_legacy_rows().size() == 1);

    MixedFilamentLegacyRow row = mgr.mixed_filament_legacy_rows().front();
    row.ratio_a = 1;
    row.ratio_b = 1;
    row.manual_pattern.clear();
    row.distribution_mode = int(MixedFilamentLegacyRow::Simple);
    set_legacy_row(mgr, 0, row, colors);

    CHECK(mgr.effective_painted_region_filament_id(3, 2, 0) == 1);
    CHECK(mgr.effective_painted_region_filament_id(3, 2, 1) == 2);
}

TEST_CASE("Mixed filament painted-region resolver preserves virtual channels for grouped patterns", "[MixedFilament]")
{
    const std::vector<std::string> colors = {"#00FFFF", "#FF00FF"};

    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, colors);
    REQUIRE(mgr.mixed_filament_legacy_rows().size() == 1);

    MixedFilamentLegacyRow row = mgr.mixed_filament_legacy_rows().front();
    row.manual_pattern = MixedFilamentManager::normalize_manual_pattern("12,21");
    set_legacy_row(mgr, 0, row, colors);
    CHECK(mgr.effective_painted_region_filament_id(3, 2, 0) == 3);
    row.component_a_surface_offset = 0.02f;
    row.component_b_surface_offset = -0.02f;
    set_legacy_row(mgr, 0, row, colors);
    CHECK(mgr.component_surface_offset(3, 2, 0) == Approx(0.0f));
}

TEST_CASE("compute_redundant_filaments physical-only", "[MixedFilament][redundant_set]")
{
    // 4 physicals, keep {1,3} → redundant {4,2}
    auto mgr = build_manager(4);
    auto red = compute_redundant_filaments(4, {1, 3}, {}, mgr.mixed_filaments());
    REQUIRE(red.redundant_mixed.empty());
    REQUIRE(red.redundant_physical.size() == 2);
    CHECK(red.redundant_physical[0] == 4); // descending
    CHECK(red.redundant_physical[1] == 2);
    CHECK(red.new_num_physical == 2);
}

TEST_CASE("compute_redundant_filaments mixed-only", "[MixedFilament][redundant_set]")
{
    // 4 physicals, 1 mixed row (v5), keep all physicals but not the mixed
    auto mgr = build_manager(4, {make_row(1, 2)});
    auto red = compute_redundant_filaments(4, {1,2,3,4}, {}, mgr.mixed_filaments());
    REQUIRE(red.redundant_physical.empty());
    REQUIRE(red.redundant_mixed.size() == 1);
    CHECK(red.redundant_mixed[0] == 5); // virtual id = 4+1
    CHECK(red.cascade_mixed_count == 0);
}

TEST_CASE("remove_physical_filament preserves stable_id on survivors", "[MixedFilament][redundant_set]")
{
    // Regression guard for batch-match cleanup (C1 fix): cleanup marks
    // redundant mixed rows by stable_id AFTER physical deletion rebuilds
    // m_mixed, so it relies on remove_physical_filament keeping each
    // survivor's stable_id unchanged (it edits components/pattern only).
    //   row A (1,2) stable_id=100 — references physical 2 -> DROPPED
    //   row B (1,3) stable_id=200 — survives; component 3 > 2 -> renumbered to 2
    auto mgr = build_manager(4, {make_row(1, 2), make_row(1, 3)});
    auto &rows = mgr.mixed_filaments();
    REQUIRE(rows.size() == 2);
    rows[0].stable_id = 100;
    rows[1].stable_id = 200;

    mgr.remove_physical_filament(2); // delete physical 2 (1-based)

    REQUIRE(rows.size() == 1);           // A dropped; B survives the rebuild
    CHECK(rows[0].stable_id == 200);     // identity preserved through rebuild
    CHECK(rows[0].component_a == 1);
    CHECK(rows[0].component_b == 2);     // 3 > deleted 2 -> decremented to 2
}

TEST_CASE("compute_redundant_filaments cascade component-a", "[MixedFilament][redundant_set]")
{
    // 4 physical, keep {1,2,3} (drop 4). Mixed row component_a=4 → cascade
    auto mgr = build_manager(4, {make_row(4, 1)});
    auto red = compute_redundant_filaments(4, {1,2,3}, {5}, mgr.mixed_filaments());
    REQUIRE(red.redundant_physical.size() == 1);
    CHECK(red.redundant_physical[0] == 4);
    REQUIRE(red.redundant_mixed.size() == 1);
    CHECK(red.redundant_mixed[0] == 5);
    CHECK(red.cascade_mixed_count == 1);
}

TEST_CASE("compute_redundant_filaments cascade component-b", "[MixedFilament][redundant_set]")
{
    // 4 physical, keep {1,2,3}. Mixed row component_b=4 → cascade
    auto mgr = build_manager(4, {make_row(1, 4)});
    auto red = compute_redundant_filaments(4, {1,2,3}, {5}, mgr.mixed_filaments());
    REQUIRE(red.redundant_mixed.size() == 1);
    CHECK(red.redundant_mixed[0] == 5);
    CHECK(red.cascade_mixed_count == 1);
}

TEST_CASE("compute_redundant_filaments cascade gradient-ids", "[MixedFilament][redundant_set]")
{
    // 4 physical, keep {1,2,3}. Mixed row gradient_component_ids="14" → cascade
    auto mgr = build_manager(4, {make_row(1, 2, "14")});
    auto red = compute_redundant_filaments(4, {1,2,3}, {5}, mgr.mixed_filaments());
    REQUIRE(red.redundant_mixed.size() == 1);
    CHECK(red.redundant_mixed[0] == 5);
    CHECK(red.cascade_mixed_count == 1);
}

TEST_CASE("compute_redundant_filaments survivor-floor", "[MixedFilament][redundant_set]")
{
    // Empty kept set → survivor floor keeps only filament 1
    auto mgr = build_manager(4);
    auto red = compute_redundant_filaments(4, {}, {}, mgr.mixed_filaments());
    CHECK(red.new_num_physical == 1);
    REQUIRE(red.redundant_physical.size() == 3);
    CHECK(red.redundant_physical[0] == 4); // descending
    CHECK(red.redundant_physical[1] == 3);
    CHECK(red.redundant_physical[2] == 2);
}

TEST_CASE("compute_redundant_filaments keep-all", "[MixedFilament][redundant_set]")
{
    // 3 physical + 1 mixed, all kept
    auto mgr = build_manager(3, {make_row(1, 2)});
    auto red = compute_redundant_filaments(3, {1,2,3}, {4}, mgr.mixed_filaments());
    CHECK(red.redundant_physical.empty());
    CHECK(red.redundant_mixed.empty());
}

TEST_CASE("compute_redundant_filaments empty-mixed", "[MixedFilament][redundant_set]")
{
    // No mixed rows → no crash
    auto mgr = build_manager(2);
    auto red = compute_redundant_filaments(2, {1}, {}, mgr.mixed_filaments());
    CHECK(red.redundant_physical.size() == 1);
    CHECK(red.redundant_mixed.empty());
}

TEST_CASE("compute_redundant_filaments out-of-range ids", "[MixedFilament][redundant_set]")
{
    // kept_physical_ids={99} → filtered to empty → floor → filament 1 only
    auto mgr = build_manager(4);
    auto red = compute_redundant_filaments(4, {99}, {999}, mgr.mixed_filaments());
    CHECK(red.new_num_physical == 1);
    REQUIRE(red.redundant_physical.size() == 3);
    CHECK(red.redundant_physical[0] == 4); // descending
    CHECK(red.redundant_physical[1] == 3);
    CHECK(red.redundant_physical[2] == 2);
    CHECK(red.redundant_mixed.empty());
}

TEST_CASE("compute_redundant_filaments modern gradient-id format", "[MixedFilament][redundant_set]")
{
    // "/" separated multi-digit IDs
    auto mgr = build_manager(12, {make_row(1, 2, "1/12/3")});
    auto red = compute_redundant_filaments(12, {1,2,3,4,5,6,7,8,9,10,11}, {13}, mgr.mixed_filaments());
    // Physical 12 is not kept → gradient contains 12 → cascade
    REQUIRE(red.redundant_physical.size() == 1);
    CHECK(red.redundant_physical[0] == 12);
    REQUIRE(red.redundant_mixed.size() == 1);
    CHECK(red.cascade_mixed_count == 1);
}

TEST_CASE("add_batch assigned_ids matches virtual-id enumeration", "[MixedFilament][batch_match]")
{
    // 4 physicals, no pre-existing mixed rows.
    std::vector<std::string> colors = {"#FF0000", "#00FF00", "#0000FF", "#FFFF00"};
    MixedAutoGenerateGuard    guard(false);
    MixedFilamentManager      mgr;

    std::vector<MixedFilamentBatchEntry> entries(2);
    entries[0].component_a   = 1;
    entries[0].component_b   = 2;
    entries[0].mix_b_percent = 50;
    entries[1].component_a   = 3;
    entries[1].component_b   = 4;
    entries[1].mix_b_percent = 30;

    std::vector<unsigned int> assigned_ids;
    mgr.add_batch_custom_filaments(entries, colors, &assigned_ids);

    REQUIRE(assigned_ids.size() == 2);
    CHECK(assigned_ids[0] == 5);
    CHECK(assigned_ids[1] == 6);

    // kept_mixed = assigned_ids → both rows kept
    auto red = compute_redundant_filaments(4, {1,2,3,4}, assigned_ids, mgr.mixed_filaments());
    CHECK(red.redundant_physical.empty());
    CHECK(red.redundant_mixed.empty());

    // With only one in kept_mixed, the other is redundant
    auto red2 = compute_redundant_filaments(4, {1,2,3,4}, {assigned_ids[0]}, mgr.mixed_filaments());
    CHECK(red2.redundant_mixed.size() == 1);
    CHECK(red2.redundant_mixed[0] == assigned_ids[1]);
}

TEST_CASE("auto_generate shifts virtual ids before add_batch", "[MixedFilament][batch_match]")
{
    // Reproduces the second-match paint-loss bug:
    //   T0: dialog computes target_filament_id assuming 4 phys + 0 mixed → v5.
    //   T1: set_num_filaments → auto_generate inserts C(4,2)=6 auto rows at v5-v10.
    //   T2: add_batch_custom_filaments assigns actual ids starting from v11.
    //   kept_mixed uses the stale dialog target (v5) → batch row (v11) becomes
    //   redundant → deleted → painting lost.
    std::vector<std::string> colors = {"#FF0000", "#00FF00", "#0000FF", "#FFFF00"};

    // T0: dialog-time state
    size_t num_phys_dialog    = 4;
    size_t existing_mixed_cnt = 0;
    unsigned int dialog_target = unsigned(num_phys_dialog + existing_mixed_cnt + 1);
    CHECK(dialog_target == 5);

    // T1: auto_generate
    MixedAutoGenerateGuard guard(true);
    MixedFilamentManager   mgr;
    mgr.auto_generate(colors);
    CHECK(mgr.enabled_count() == 6);

    // T2: add_batch
    std::vector<MixedFilamentBatchEntry> entries(1);
    entries[0].component_a   = 1;
    entries[0].component_b   = 2;
    entries[0].mix_b_percent = 50;

    std::vector<unsigned int> assigned_ids;
    mgr.add_batch_custom_filaments(entries, colors, &assigned_ids);

    REQUIRE(assigned_ids.size() == 1);
    unsigned int actual_assigned = assigned_ids[0];
    CHECK(actual_assigned == 11);

    // Dialog target ≠ actual assigned
    CHECK(dialog_target != actual_assigned);

    // BUG: kept_mixed with stale dialog_target ({5}) marks the actual batch row
    // (v11) as redundant, along with the other 5 auto rows not in the kept set.
    auto red = compute_redundant_filaments(4, {1,2,3,4}, {dialog_target}, mgr.mixed_filaments());
    REQUIRE(red.redundant_mixed.size() == 6);                // v6-v11 all redundant
    CHECK(red.redundant_mixed[5] == actual_assigned);        // last one = the batch row

    // Fix: kept_mixed with assigned_ids ({11}) keeps the batch row.
    // The 6 auto rows (v5-v10) not in kept_mixed remain redundant.
    auto red_ok = compute_redundant_filaments(4, {1,2,3,4}, assigned_ids, mgr.mixed_filaments());
    REQUIRE(red_ok.redundant_mixed.size() == 6);
    // v5-v10 are auto rows, none are the batch row
    for (unsigned int id : red_ok.redundant_mixed)
        CHECK(id != actual_assigned);
}

TEST_CASE("compute_redundant_filaments manual-pattern cascade legacy", "[MixedFilament][redundant_set]")
{
    // mixed a=1,b=2, manual_pattern="13". Token '3'=literal physical 3.
    // Keep {1,2,4}, delete 3 → manual_pattern refs deleted physical → cascade.
    auto mgr = build_manager(4, {make_row(1, 2, {}, "13")});
    auto red = compute_redundant_filaments(4, {1,2,4}, {5}, mgr.mixed_filaments());
    REQUIRE(red.redundant_physical.size() == 1);
    CHECK(red.redundant_physical[0] == 3);
    REQUIRE(red.redundant_mixed.size() == 1);
    CHECK(red.redundant_mixed[0] == 5);
    CHECK(red.cascade_mixed_count == 1);
}

TEST_CASE("compute_redundant_filaments manual-pattern cascade multi-digit-token", "[MixedFilament][redundant_set]")
{
    // Mixed row with manual_pattern containing a literal direct physical id
    // that is NOT kept.  Same semantics as the modern "/" format but using
    // the legacy encoding path which is what normalize_manual_pattern produces
    // (it drops '/' characters, converting "1/11/2" → "1[11]2" style; the
    // multi-digit normalization is covered by the existing remove_physical_filament
    // test in the source).  Here we test: "14" pattern where token '4' is a
    // literal physical id not in the kept set.
    auto mgr = build_manager(4, {make_row(1, 2, {}, "14")});
    auto red = compute_redundant_filaments(4, {1,2,3}, {5}, mgr.mixed_filaments());
    REQUIRE(red.redundant_physical.size() == 1);
    CHECK(red.redundant_physical[0] == 4);
    REQUIRE(red.redundant_mixed.size() == 1);
    CHECK(red.cascade_mixed_count == 1);
}

TEST_CASE("compute_redundant_filaments manual-pattern token-1-resolves-to-component-a", "[MixedFilament][redundant_set]")
{
    // mixed a=3,b=4, manual_pattern="1". Token '1'→component_a(=3).
    // Keep {1,2,3,4} (3 kept) → no cascade.
    auto mgr = build_manager(4, {make_row(3, 4, {}, "1")});
    auto red = compute_redundant_filaments(4, {1,2,3,4}, {5}, mgr.mixed_filaments());
    CHECK(red.redundant_mixed.empty());
    CHECK(red.cascade_mixed_count == 0);
}

TEST_CASE("compute_redundant_filaments manual-pattern token-2-resolves-to-component-b", "[MixedFilament][redundant_set]")
{
    // mixed a=1,b=4, manual_pattern="2". Token '2'→component_b(=4).
    // Keep {1,2,3}, delete 4 → cascade via component_b, not just via pattern.
    auto mgr = build_manager(4, {make_row(1, 4, {}, "2")});
    auto red = compute_redundant_filaments(4, {1,2,3}, {5}, mgr.mixed_filaments());
    REQUIRE(red.redundant_mixed.size() == 1);
    CHECK(red.cascade_mixed_count == 1);
}

TEST_CASE("compute_redundant_filaments both-components-cascade", "[MixedFilament][redundant_set]")
{
    // mixed a=3,b=4. Keep {1,2}, delete 3 AND 4 → both cascade.
    auto mgr = build_manager(4, {make_row(3, 4)});
    auto red = compute_redundant_filaments(4, {1,2}, {5}, mgr.mixed_filaments());
    REQUIRE(red.redundant_physical.size() == 2);
    REQUIRE(red.redundant_mixed.size() == 1);
    CHECK(red.cascade_mixed_count == 1);
}

TEST_CASE("compute_redundant_filaments all-mixed-deleted", "[MixedFilament][redundant_set]")
{
    // 4 phys + 2 mixed. keep_phys={1,2,3,4}, kept_mixed empty.
    // Both mixed are explicit redundant (not in kept set).
    auto mgr = build_manager(4, {make_row(1, 2), make_row(3, 4)});
    auto red = compute_redundant_filaments(4, {1,2,3,4}, {}, mgr.mixed_filaments());
    CHECK(red.redundant_physical.empty());
    REQUIRE(red.redundant_mixed.size() == 2);
    CHECK(red.redundant_mixed[0] == 5);
    CHECK(red.redundant_mixed[1] == 6);
    CHECK(red.cascade_mixed_count == 0);
}

TEST_CASE("compute_redundant_filaments num-physical-2", "[MixedFilament][redundant_set]")
{
    // Smallest meaningful physical count. 2 phys + 1 mixed.
    auto mgr = build_manager(2, {make_row(1, 2)});
    auto red = compute_redundant_filaments(2, {1}, {3}, mgr.mixed_filaments());
    REQUIRE(red.redundant_physical.size() == 1);
    CHECK(red.redundant_physical[0] == 2);
    // mixed row component_b=2 → deleted physical → cascade (not just kept)
    REQUIRE(red.redundant_mixed.size() == 1);
    CHECK(red.redundant_mixed[0] == 3);
    CHECK(red.cascade_mixed_count == 1);
}

TEST_CASE("compute_redundant_filaments num-physical-0", "[MixedFilament][redundant_set]")
{
    // No physical filaments is a degenerate input: the survivor floor does not
    // apply (nothing to survive) and nothing can be redundant.  Production code
    // never reaches this state (cleanup_unused_filaments_after_batch_match
    // guards on filament_presets.empty()), but this pure helper is exported and
    // test-visible, so the contract must hold.  Before the early-return, the
    // mixed-enumeration loop below would run with virtual_id = num_physical + 1
    // = 1 and emit bogus redundant_mixed entries for any stray rows — this test
    // pins the fixed, well-defined behaviour (empty result, no floor).
    std::vector<MixedFilament> stray{make_row(1, 2)};
    auto red = compute_redundant_filaments(0, {}, {}, stray);
    REQUIRE(red.redundant_physical.empty());
    REQUIRE(red.redundant_mixed.empty());
    CHECK(red.cascade_mixed_count == 0);
}

TEST_CASE("compute_redundant_filaments deleted-rows-skipped", "[MixedFilament][redundant_set]")
{
    // 4 phys. Push a deleted mixed row — compute must skip it.
    // NOTE: build_manager overrides enabled/deleted on extra_rows, so we push
    // the deleted row directly into the manager's mutable list after creation.
    MixedFilament del_row = make_row(1, 2);
    del_row.enabled = false;
    del_row.deleted = true;
    auto mgr = build_manager(4, {make_row(3, 4)});   // v5 = enabled row (3,4)
    mgr.mixed_filaments().insert(mgr.mixed_filaments().begin(), del_row); // pushed as-is at front
    // Now: [deleted(1,2), enabled(3,4)].  virtual_id=5 for the deleted row is
    // skipped (continue); the enabled row also gets virtual_id=5, which is in
    // kept_mixed={5} → kept.  deleted/disabled rows do NOT consume a virtual-ID slot.
    auto red = compute_redundant_filaments(4, {1,2,3,4}, {5}, mgr.mixed_filaments());
    CHECK(red.redundant_mixed.empty());
}

TEST_CASE("B1: kept manual_pattern row whose component_a is dropped must survive (differential)", "[MixedFilament][redundant_set][differential]")
{
    // THE B1 ROOT. a=3 (dropped), b=2, manual_pattern "2" -> component_b=2 (kept).
    // The pattern does NOT reference component_a. At runtime resolve() uses the
    // pattern, so the row works; remove_physical_filament gates component_a behind
    // norm.empty() and PRESERVES the row. But compute checks component_a
    // unconditionally -> cascade -> the cleanup would mark this live row deleted.
    // RED until the component_a check is gated behind norm.empty() (aligned with
    // remove_physical_filament), at which point it turns GREEN.
    std::vector<MixedFilament> rows{ make_row(3, 2, {}, "2") };
    expect_kept_mixed_matches_runtime(4, {1, 2, 4}, {5}, rows);  // drop physical 3
}

TEST_CASE("B1 control: pattern references dropped component_b -> both drop (differential)", "[MixedFilament][redundant_set][differential]")
{
    // Mirror of B1 via component_b: a=1, b=4 (dropped), pattern "2" -> component_b=4.
    // The pattern genuinely references the dropped physical, so both compute
    // (token resolves to 4, not kept) and remove_physical (token "2"->4==deleted)
    // drop the row. GREEN -- documents that the b-path is consistent and that the
    // divergence is component_a only.
    std::vector<MixedFilament> rows{ make_row(1, 4, {}, "2") };
    expect_kept_mixed_matches_runtime(4, {1, 2, 3}, {5}, rows);  // drop physical 4
}

TEST_CASE("B1 control: pattern '1' references dropped component_a -> both drop (differential)", "[MixedFilament][redundant_set][differential]")
{
    // a=3 (dropped), pattern "1" -> component_a=3 (genuinely references the dropped
    // physical). Both compute (token resolves to 3, not kept) and remove_physical
    // (physical_filament_from_token("1")->3==deleted) drop the row. GREEN.
    // Distinguishes "pattern USES deleted component_a" (agree) from B1 "pattern
    // does NOT use component_a but it is dropped" (disagree).
    std::vector<MixedFilament> rows{ make_row(3, 2, {}, "1") };
    expect_kept_mixed_matches_runtime(4, {1, 2, 4}, {5}, rows);  // drop physical 3
}

TEST_CASE("B1 variant: multi-token pattern not touching component_a, a dropped (differential)", "[MixedFilament][redundant_set][differential]")
{
    // a=3 (dropped). pattern "2,4": token '2'->component_b=2 (kept), token '4'->literal 4 (kept).
    // Neither token references physical 3, so remove_physical PRESERVES; compute
    // over-cascades on component_a=3. RED (B1). Confirms B1 is not specific to a
    // single-token pattern.
    std::vector<MixedFilament> rows{ make_row(3, 2, {}, "2,4") };
    expect_kept_mixed_matches_runtime(4, {1, 2, 4}, {5}, rows);  // drop physical 3
}

TEST_CASE("B1 with two kept rows: only the over-cascaded one disagrees (differential)", "[MixedFilament][redundant_set][differential]")
{
    // v5: a=3 dropped, pattern "2"->b=2 kept   -> B1 over-cascade (compute drops, runtime keeps).
    // v6: a=3 dropped, pattern "1"->a=3 dropped -> both agree (drop).
    // The oracle flags exactly the v5 disagreement. RED (B1). Shows B1 can coexist
    // with a correctly-cascaded sibling row.
    std::vector<MixedFilament> rows{ make_row(3, 2, {}, "2"), make_row(3, 2, {}, "1") };
    expect_kept_mixed_matches_runtime(4, {1, 2, 4}, {5, 6}, rows);  // drop physical 3
}

TEST_CASE("B1 control: no pattern, component_a dropped -> both drop via norm-empty branch (differential)", "[MixedFilament][redundant_set][differential]")
{
    // No manual_pattern (norm empty). a=4 (dropped). Both compute (component_a check
    // in the norm-empty branch) and remove_physical (pair check, norm empty) drop
    // the row. GREEN -- confirms the divergence lives ONLY in the norm-non-empty
    // branch (i.e. only when a manual_pattern is present).
    std::vector<MixedFilament> rows{ make_row(4, 1) };
    expect_kept_mixed_matches_runtime(4, {1, 2, 3}, {5}, rows);  // drop physical 4
}

TEST_CASE("compute_redundant_filaments gradient-and-manual-pattern-both-kept", "[MixedFilament][redundant_set]")
{
    // mixed with gradient="13" and manual_pattern="4". Keep {1,2,3} → delete 4.
    // manual_pattern refs deleted id 4 → cascade, even though gradient ids are kept.
    auto mgr = build_manager(4, {make_row(1, 2, "13", "4")});
    // gradient "13": physicals 1 and 3 → both kept {1,3} ✓
    // manual_pattern "4": literal physical 4 → not in {1,2,3} → cascade
    auto red = compute_redundant_filaments(4, {1,2,3}, {5}, mgr.mixed_filaments());
    REQUIRE(red.redundant_physical.size() == 1);
    CHECK(red.redundant_physical[0] == 4);
    REQUIRE(red.redundant_mixed.size() == 1);
    CHECK(red.cascade_mixed_count == 1);
}

TEST_CASE("display_color follows the physical palette: color-keyed matching across a palette change misses", "[MixedFilament][batch_match]")
{
    // RV1 mechanism. WHY the confirm handler matches existing custom mixed rows
    // by IDENTITY (dialog-time vid -> stable_id -> current row) instead of by
    // display color: in recommended mode the handler rewrites filament_colour
    // and calls set_num_filaments BEFORE the in-place block, and
    // update_multi_material_filament_presets -> auto_generate ends in
    // refresh_display_colors on BOTH exits (auto-generate pref on or off), so
    // display_color is already NEW-palette based when that block runs. This
    // test pins the underlying reason: the same recipe yields a different
    // display_color under a different palette, so an equality lookup keyed on
    // the dialog-time color (the rejected design) would silently never find the
    // row. The identity lookup the code actually uses is immune to this.
    MixedAutoGenerateGuard guard(false);
    auto mgr = build_manager(2, {make_row(1, 2)});
    REQUIRE(mgr.mixed_filaments().size() == 1);

    const std::vector<std::string> palette_old = {"#FF0000", "#0000FF"}; // dialog-time
    const std::vector<std::string> palette_new = {"#FFFF00", "#00FF00"}; // post-rewrite

    mgr.refresh_display_colors(palette_old);
    const std::string dialog_time_color = mgr.mixed_filaments()[0].display_color;
    REQUIRE(!dialog_time_color.empty());

    // Control: refresh is deterministic — same palette, same display_color.
    mgr.refresh_display_colors(palette_old);
    CHECK(mgr.mixed_filaments()[0].display_color == dialog_time_color);

    // Palette change (what recommended mode does before the in-place block).
    mgr.refresh_display_colors(palette_new);
    const std::string confirm_time_color = mgr.mixed_filaments()[0].display_color;
    REQUIRE(!confirm_time_color.empty());

    // The equality key the in-place block relies on no longer holds.
    CHECK(confirm_time_color != dialog_time_color);
}

TEST_CASE("add_batch at the filament cap emits per-entry zero ids instead of truncating", "[MixedFilament][batch_match]")
{
    // The confirm handler aligns assigned_ids to mappings by construction
    // order, so the contract is STRICT: one id per input entry, 0 = dropped.
    // The cap path must therefore continue (pushing 0s), never break early.
    MixedAutoGenerateGuard guard(false);
    const std::vector<std::string> colors = {"#FF0000", "#00FF00", "#0000FF", "#FFFF00"};
    MixedFilamentManager mgr;
    // Fill to one slot below the cap: 4 physical + (kMax - 5) mixed = kMax - 1.
    const size_t kMax = MAXIMUM_FILAMENT_NUMBER;
    for (size_t i = 0; i < kMax - 5; ++i)
        mgr.mixed_filaments().push_back(make_row(1, 2));
    REQUIRE(mgr.total_filaments(4) == kMax - 1);

    std::vector<MixedFilamentBatchEntry> entries(3);
    for (auto &e : entries) {
        e.component_a   = 1;
        e.component_b   = 3;
        e.mix_b_percent = 40;
    }

    std::vector<unsigned int> assigned;
    mgr.add_batch_custom_filaments(entries, colors, &assigned);

    REQUIRE(assigned.size() == 3);          // strict 1:1 with entries
    CHECK(assigned[0] == kMax);             // last free slot
    CHECK(assigned[1] == 0);                // dropped at cap
    CHECK(assigned[2] == 0);                // still one zero PER entry
    CHECK(mgr.total_filaments(4) == kMax);
}

TEST_CASE("add_batch clamps out-of-range components and falls back on a==b instead of dropping", "[MixedFilament][batch_match]")
{
    MixedAutoGenerateGuard guard(false);
    const std::vector<std::string> colors = {"#FF0000", "#00FF00", "#0000FF"};
    MixedFilamentManager mgr;

    std::vector<MixedFilamentBatchEntry> entries(3);
    entries[0].component_a = 2;  entries[0].component_b = 2;  // a==b       -> b = 1
    entries[1].component_a = 1;  entries[1].component_b = 1;  // a==b, a==1 -> b = 2
    entries[2].component_a = 99; entries[2].component_b = 1;  // a clamped to n=3

    std::vector<unsigned int> assigned;
    mgr.add_batch_custom_filaments(entries, colors, &assigned);

    REQUIRE(assigned.size() == 3);
    CHECK(assigned[0] == 4);
    CHECK(assigned[1] == 5);
    CHECK(assigned[2] == 6);
    REQUIRE(mgr.mixed_filaments().size() == 3);
    CHECK(mgr.mixed_filaments()[0].component_a == 2);
    CHECK(mgr.mixed_filaments()[0].component_b == 1);
    CHECK(mgr.mixed_filaments()[1].component_a == 1);
    CHECK(mgr.mixed_filaments()[1].component_b == 2);
    CHECK(mgr.mixed_filaments()[2].component_a == 3);
    CHECK(mgr.mixed_filaments()[2].component_b == 1);
}

TEST_CASE("add_batch guards: <2 colours yields all-zero ids, empty entries yield empty ids", "[MixedFilament][batch_match]")
{
    MixedAutoGenerateGuard guard(false);
    MixedFilamentManager mgr;

    std::vector<MixedFilamentBatchEntry> entries(2);
    entries[0].component_a = 1; entries[0].component_b = 2;
    entries[1].component_a = 1; entries[1].component_b = 2;

    std::vector<unsigned int> assigned = {77u}; // stale content must be overwritten
    mgr.add_batch_custom_filaments(entries, {"#FF0000"}, &assigned); // n = 1
    REQUIRE(assigned.size() == 2);              // still 1:1 with entries
    CHECK(assigned[0] == 0);
    CHECK(assigned[1] == 0);
    CHECK(mgr.mixed_filaments().empty());

    std::vector<unsigned int> assigned2 = {77u};
    mgr.add_batch_custom_filaments({}, {"#FF0000", "#00FF00"}, &assigned2);
    CHECK(assigned2.empty());                   // 0 entries -> 0 ids
    CHECK(mgr.mixed_filaments().empty());
}

TEST_CASE("compute_redundant_filaments non-contiguous kept subset [2,6,8,10]", "[MixedFilament][redundant_set]")
{
    // Layer 1: pin the upstream deterministic input that the cleanup loop feeds
    // into the composite remap. This function's output is CORRECT for a
    // non-contiguous kept set; it just produces the {9,7,5,4,3,1} descending
    // deletion list that the (buggy) batch remap then misinterprets.
    auto mgr = build_manager(10, {});
    auto red = compute_redundant_filaments(10, {2, 6, 8, 10}, {}, mgr.mixed_filaments());

    REQUIRE(red.redundant_physical.size() == 6);
    // Descending order — cleanup's batched-delete path requires this (and
    // asserts it at runtime, Plater.cpp:8372-8383).
    CHECK(red.redundant_physical[0] == 9);
    CHECK(red.redundant_physical[1] == 7);
    CHECK(red.redundant_physical[2] == 5);
    CHECK(red.redundant_physical[3] == 4);
    CHECK(red.redundant_physical[4] == 3);
    CHECK(red.redundant_physical[5] == 1);
    CHECK(red.new_num_physical == 4);
}

TEST_CASE("build_mixed_deletion_painting_remap: single middle delete shifts tail down by one", "[MixedFilament][deletion_remap]")
{
    // The reported scenario: 4 physicals, mixed rows v5..v14. After duplicate-
    // recipe merge, v9 is redundant and gets deleted. Every vid > 9 must shift
    // down by one; v9 itself maps to NONE.
    const size_t num_physical = 4;
    const size_t t2_total     = 14; // 4 physical + 10 mixed (v5..v14)
    const std::vector<unsigned int> deleted = {9};

    const auto remap = MixedFilamentManager::build_mixed_deletion_painting_remap(num_physical, t2_total, deleted);

    REQUIRE(remap.size() == t2_total + 1);
    CHECK(remap[0] == 0u); // unused index
    // Physical slots: identity.
    CHECK(remap[1] == 1u);
    CHECK(remap[2] == 2u);
    CHECK(remap[3] == 3u);
    CHECK(remap[4] == 4u);
    // Mixed slots before the deleted one: identity.
    CHECK(remap[5] == 5u);
    CHECK(remap[6] == 6u);
    CHECK(remap[7] == 7u);
    CHECK(remap[8] == 8u);
    // Deleted slot itself -> NONE.
    CHECK(remap[9] == 0u);
    // Mixed slots after the deleted one: shift down by one.
    CHECK(remap[10] == 9u);
    CHECK(remap[11] == 10u);
    CHECK(remap[12] == 11u);
    CHECK(remap[13] == 12u);
    CHECK(remap[14] == 13u);
}

TEST_CASE("build_mixed_deletion_painting_remap: multiple deletes accumulate offset", "[MixedFilament][deletion_remap]")
{
    // Delete v7 and v9 out of v5..v14. Each survivor's new id subtracts the
    // number of deleted vids strictly less than it.
    const size_t num_physical = 4;
    const size_t t2_total     = 14;
    const std::vector<unsigned int> deleted = {7, 9};

    const auto remap = MixedFilamentManager::build_mixed_deletion_painting_remap(num_physical, t2_total, deleted);

    REQUIRE(remap.size() == t2_total + 1);
    CHECK(remap[0] == 0u);
    // Physical + pre-first-delete mixed: identity.
    CHECK(remap[5] == 5u);
    CHECK(remap[6] == 6u);
    // Deleted.
    CHECK(remap[7] == 0u);
    // v8: one deleted vid (7) below it -> 8-1 = 7.
    CHECK(remap[8] == 7u);
    // Deleted.
    CHECK(remap[9] == 0u);
    // v10..v14: two deleted vids (7,9) below each -> subtract 2.
    CHECK(remap[10] == 8u);
    CHECK(remap[11] == 9u);
    CHECK(remap[12] == 10u);
    CHECK(remap[13] == 11u);
    CHECK(remap[14] == 12u);
}

TEST_CASE("build_mixed_deletion_painting_remap: physical slots never move", "[MixedFilament][deletion_remap]")
{
    // Even with mixed deletes, the physical range [1..num_physical] is identity.
    const size_t num_physical = 4;
    const size_t t2_total     = 8;
    const std::vector<unsigned int> deleted = {5, 6};

    const auto remap = MixedFilamentManager::build_mixed_deletion_painting_remap(num_physical, t2_total, deleted);

    REQUIRE(remap.size() == t2_total + 1);
    CHECK(remap[1] == 1u);
    CHECK(remap[2] == 2u);
    CHECK(remap[3] == 3u);
    CHECK(remap[4] == 4u);
}

TEST_CASE("build_mixed_deletion_painting_remap: deleted vids map to NONE", "[MixedFilament][deletion_remap]")
{
    const size_t num_physical = 4;
    const size_t t2_total     = 10;
    const std::vector<unsigned int> deleted = {6, 8};

    const auto remap = MixedFilamentManager::build_mixed_deletion_painting_remap(num_physical, t2_total, deleted);

    CHECK(remap[6] == 0u);
    CHECK(remap[8] == 0u);
}

TEST_CASE("build_mixed_deletion_painting_remap: delete all mixed leaves only physicals", "[MixedFilament][deletion_remap]")
{
    // Deleting every mixed row: the survivors are the physicals alone, all
    // unchanged; every mixed vid maps to NONE.
    const size_t num_physical = 4;
    const size_t t2_total     = 7; // 4 physical + 3 mixed (v5,v6,v7)
    const std::vector<unsigned int> deleted = {5, 6, 7};

    const auto remap = MixedFilamentManager::build_mixed_deletion_painting_remap(num_physical, t2_total, deleted);

    REQUIRE(remap.size() == t2_total + 1);
    CHECK(remap[1] == 1u);
    CHECK(remap[2] == 2u);
    CHECK(remap[3] == 3u);
    CHECK(remap[4] == 4u);
    CHECK(remap[5] == 0u);
    CHECK(remap[6] == 0u);
    CHECK(remap[7] == 0u);
}

TEST_CASE("build_mixed_deletion_painting_remap: empty delete list is identity (short-circuit)", "[MixedFilament][deletion_remap]")
{
    // No deletes -> every vid maps to itself. This is the no-op path cleanup
    // must take without running a remap pass over the whole model.
    const size_t num_physical = 4;
    const size_t t2_total     = 10;
    const std::vector<unsigned int> deleted;

    const auto remap = MixedFilamentManager::build_mixed_deletion_painting_remap(num_physical, t2_total, deleted);

    REQUIRE(remap.size() == t2_total + 1);
    for (size_t i = 1; i <= t2_total; ++i)
        CHECK(remap[i] == static_cast<unsigned int>(i));
}

TEST_CASE("build_mixed_deletion_painting_remap: unsorted delete input is tolerated", "[MixedFilament][deletion_remap]")
{
    // Robustness: caller may supply vids in any order; the function must sort
    // internally so the offset count is correct. Same expectation as the
    // ordered {7,9} case.
    const size_t num_physical = 4;
    const size_t t2_total     = 14;
    const std::vector<unsigned int> deleted = {9, 7}; // descending input

    const auto remap = MixedFilamentManager::build_mixed_deletion_painting_remap(num_physical, t2_total, deleted);

    REQUIRE(remap.size() == t2_total + 1);
    CHECK(remap[7] == 0u);
    CHECK(remap[8] == 7u); // one delete below
    CHECK(remap[9] == 0u);
    CHECK(remap[10] == 8u); // two deletes below
    CHECK(remap[14] == 12u);
}

TEST_CASE("build_mixed_deletion_painting_remap: duplicate ids in delete list dedupe (offset not inflated)", "[MixedFilament][deletion_remap]")
{
    // Robustness: a caller that collects the same vid twice (e.g. from
    // overlapping reference scans) must not double-count it — the function
    // sorts + uniques internally, so {9,9,7} is equivalent to {7,9}.
    // The batch-match cleanup reuses this table to drive both painting and
    // config-level extruder remaps, so an inflated offset would shift every
    // survivor below the duplicate onto the wrong row.
    const size_t num_physical = 4;
    const size_t t2_total     = 14;
    const std::vector<unsigned int> deleted = {9, 9, 7}; // duplicate 9

    const auto remap = MixedFilamentManager::build_mixed_deletion_painting_remap(num_physical, t2_total, deleted);

    REQUIRE(remap.size() == t2_total + 1);
    CHECK(remap[7] == 0u);
    CHECK(remap[8] == 7u); // one delete below (7 only; 9 is above)
    CHECK(remap[9] == 0u);
    CHECK(remap[10] == 8u); // two deletes below (7, 9) — NOT three
    CHECK(remap[14] == 12u);
}
