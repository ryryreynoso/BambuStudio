// Tests for the Full Spectrum "Cycle" mode port (FilamentMixer cycle patterns).
// Expected sequences follow OrcaSlicer-FullSpectrum MixedFilamentManager::resolve():
// a single-group manual pattern resolves layer N to sequence[N % size].
#include <catch2/catch.hpp>

#include "libslic3r/FilamentMixer.hpp"

using namespace Slic3r;

using Groups = std::vector<std::vector<unsigned int>>;

TEST_CASE("Cycle pattern parsing", "[FilamentMixer][FullSpectrum]")
{
    const std::vector<unsigned int> comps{2, 4};
    Groups groups;
    std::string err;

    SECTION("empty and blank patterns mean ratio mode") {
        REQUIRE(parse_mixed_pattern("", comps, groups, &err));
        REQUIRE(groups.empty());
        REQUIRE(parse_mixed_pattern("   ", comps, groups, &err));
        REQUIRE(groups.empty());
    }
    SECTION("separated decimal indices") {
        REQUIRE(parse_mixed_pattern("2/2/4", comps, groups, &err));
        REQUIRE(groups == Groups{{2, 2, 4}});
        REQUIRE(parse_mixed_pattern("2 4-2_4", comps, groups, &err));
        REQUIRE(groups == Groups{{2, 4, 2, 4}});
    }
    SECTION("multi-digit indices need separators") {
        REQUIRE(parse_mixed_pattern("12/10", {10, 12}, groups, &err));
        REQUIRE(groups == Groups{{12, 10}});
    }
    SECTION("compact Full Spectrum form is one step per character") {
        REQUIRE(parse_mixed_pattern("2242", comps, groups, &err));
        REQUIRE(groups == Groups{{2, 2, 4, 2}});
    }
    SECTION("letters name components by position") {
        REQUIRE(parse_mixed_pattern("AAB", comps, groups, &err));
        REQUIRE(groups == Groups{{2, 2, 4}});
        REQUIRE(parse_mixed_pattern("a/b", comps, groups, &err));
        REQUIRE(groups == Groups{{2, 4}});
    }
    SECTION("perimeter groups") {
        REQUIRE(parse_mixed_pattern("AB,BA", comps, groups, &err));
        REQUIRE(groups == Groups{{2, 4}, {4, 2}});
    }
    SECTION("invalid input") {
        REQUIRE_FALSE(parse_mixed_pattern("AC", comps, groups, &err)); // C: no third component
        REQUIRE_FALSE(parse_mixed_pattern("2/x", comps, groups, &err));
        REQUIRE_FALSE(parse_mixed_pattern("0/2", comps, groups, &err));
        REQUIRE_FALSE(parse_mixed_pattern("2,,4", comps, groups, &err));
        REQUIRE(groups.empty());
        REQUIRE_FALSE(err.empty());
    }
}

TEST_CASE("Cycle pattern serialization round-trips", "[FilamentMixer][FullSpectrum]")
{
    Groups groups;
    REQUIRE(parse_mixed_pattern("AAB,BA", {1, 3}, groups));
    const std::string text = serialize_mixed_pattern(groups);
    REQUIRE(text == "1/1/3,3/1");
    Groups again;
    REQUIRE(parse_mixed_pattern(text, {1, 3}, again));
    REQUIRE(again == groups);
}

TEST_CASE("Cycle pattern resolves per layer like Full Spectrum", "[FilamentMixer][FullSpectrum]")
{
    const std::vector<unsigned int> seq = mixed_pattern_layer_sequence({{1, 1, 2}});
    const std::vector<unsigned int> expected{1, 1, 2, 1, 1, 2, 1, 1, 2};
    for (size_t layer = 0; layer < expected.size(); ++layer)
        REQUIRE(resolve_mixed_pattern(seq, layer) == expected[layer]);
    REQUIRE(resolve_mixed_pattern({}, 5) == 0);
    // Groups are concatenated for per-layer resolution.
    REQUIRE(mixed_pattern_layer_sequence({{1, 2}, {3}}) == std::vector<unsigned int>{1, 2, 3});
}

TEST_CASE("Cycle pattern validation", "[FilamentMixer][FullSpectrum]")
{
    // Slots 1-3 physical, slot 4 mixed from 1 and 3.
    const std::vector<unsigned char> is_mixed{0, 0, 0, 1};
    const std::vector<std::string>   comps{"", "", "", "1,3"};
    const std::vector<std::string>   ratios{"", "", "", "0.5,0.5"};
    const std::vector<unsigned char> no_gradient{0, 0, 0, 0};
    const std::vector<std::string>   empty4(4);

    auto errors_for = [&](const std::string &pattern, const std::vector<unsigned char> &gradient = {0, 0, 0, 0}) {
        std::vector<std::string> patterns{"", "", "", pattern};
        std::vector<std::string> ranges = empty4;
        if (gradient[3])
            ranges[3] = "0.2,0.8";
        return validate_mixed_filament_params(is_mixed, comps, ratios, gradient, ranges, empty4, patterns);
    };

    REQUIRE(errors_for("").empty());
    REQUIRE(errors_for("1/1/3").empty());
    REQUIRE(errors_for("AAB").empty());
    REQUIRE(errors_for("1/2").count("filament_mixed_pattern") == 1);    // 2 is not a component
    REQUIRE(errors_for("1/x").count("filament_mixed_pattern") == 1);    // unparsable
    REQUIRE(errors_for("13,31").count("filament_mixed_pattern") == 1);  // per-perimeter groups not ported yet
    REQUIRE(errors_for("1/3", {0, 0, 0, 1}).count("filament_mixed_pattern") == 1); // no gradient + pattern
    // Callers that predate the pattern key still validate.
    REQUIRE(validate_mixed_filament_params(is_mixed, comps, ratios, no_gradient, empty4, empty4).empty());
}

TEST_CASE("Cycle patterns follow physical filament deletion", "[FilamentMixer][FullSpectrum]")
{
    // Physical 1..4, slot 5 mixed from 2 and 4 with a pattern.
    const std::vector<unsigned char> is_mixed{0, 0, 0, 0, 1};
    const std::vector<std::string>   comps{"", "", "", "", "2,4"};

    SECTION("deleting a lower filament shifts the steps down") {
        std::vector<std::string> patterns{"", "", "", "", "AAB"};
        remap_mixed_patterns_on_delete(is_mixed, comps, patterns, 1);
        REQUIRE(patterns[4] == "1/1/3");
    }
    SECTION("deleting a filament between components only shifts the higher one") {
        std::vector<std::string> patterns{"", "", "", "", "2/4"};
        remap_mixed_patterns_on_delete(is_mixed, comps, patterns, 3);
        REQUIRE(patterns[4] == "2/3");
    }
    SECTION("deleting a used filament clears the pattern") {
        std::vector<std::string> patterns{"", "", "", "", "2/2/4"};
        remap_mixed_patterns_on_delete(is_mixed, comps, patterns, 4);
        REQUIRE(patterns[4].empty());
    }
}

TEST_CASE("AMS filament change estimate", "[FilamentMixer][FullSpectrum]")
{
    const std::vector<unsigned int> comps{1, 2};
    // Cycle "AAB": changes A->B and B->A once per three layers.
    REQUIRE(estimate_mixed_changes_per_layer(comps, {0.67, 0.33}, "AAB", false) == Approx(2. / 3.));
    // Sub-layers print every component on every layer.
    REQUIRE(estimate_mixed_changes_per_layer(comps, {0.5, 0.5}, "", true) == Approx(2.));
    REQUIRE(estimate_mixed_changes_per_layer({1, 2, 3}, {0.4, 0.3, 0.3}, "", true) == Approx(3.));
    // 50/50 alternation changes every layer; 90/10 about twice per ten layers.
    REQUIRE(estimate_mixed_changes_per_layer(comps, {0.5, 0.5}, "", false) == Approx(1.).epsilon(0.01));
    REQUIRE(estimate_mixed_changes_per_layer(comps, {0.9, 0.1}, "", false) == Approx(0.2).epsilon(0.02));
    REQUIRE(estimate_mixed_changes_per_layer({1}, {1.}, "", true) == 0.);
}
