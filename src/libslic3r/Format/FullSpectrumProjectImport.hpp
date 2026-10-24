#ifndef slic3r_Format_FullSpectrumProjectImport_hpp_
#define slic3r_Format_FullSpectrumProjectImport_hpp_

// Conversion of Full Spectrum (OrcaSlicer-FullSpectrum / Snapmaker Orca) mixed filaments into
// Bambu Studio's native mixed-filament slots.
//
// Full Spectrum numbers its virtual filaments N+1, N+2, ... (N = physical filaments) in the
// order of its non-deleted mixed rows. Native slots are appended after the physical filaments
// in the same order, so object assignments and painted facets that reference a virtual id keep
// pointing at the same mix without remapping.

#include <cstddef>
#include <string>
#include <vector>

namespace Slic3r {

// Project settings written by Full Spectrum that affect how its mixed rows are resolved.
// Defaults are Full Spectrum's own defaults.
struct FullSpectrumProjectSettings
{
    std::string mixed_filament_definitions;   // compact rows, "mixed_filament_definitions"
    bool        local_z_mode        = false;  // "dithering_local_z_mode" (subdivide all mix layers)
    bool        gradient_height_mode = false; // "mixed_filament_gradient_mode"
    bool        advanced_dithering  = false;  // "mixed_filament_advanced_dithering"
    double      layer_height        = 0.2;    // "layer_height"
    double      min_sublayer_height = 0.06;   // "mixed_filament_height_lower_bound"
};

// One native mixed slot, encoded exactly as the per-slot project keys store it.
struct FullSpectrumNativeSlot
{
    std::string components;      // filament_mixed_components, e.g. "1,3"
    std::string ratios;          // filament_mixed_sublayer_ratios, e.g. "0.7500,0.2500"
    std::string pattern;         // filament_mixed_pattern (cycle mode), may be empty
    bool        gradient = false;// filament_mixed_gradient
    std::string gradient_range;  // filament_mixed_gradient_range, e.g. "0.80,0.20"
    std::string display_color;   // "#RRGGBB" for filament_colour
    bool        uses_sublayers = false; // needs enable_mixed_color_sublayer
};

struct FullSpectrumImportResult
{
    std::vector<FullSpectrumNativeSlot> slots;
    // Rows beyond the native slot limit; virtual ids above num_physical + slots.size() have no slot.
    size_t                              dropped_rows = 0;
    // True when any slot relies on sub-layers (Full Spectrum Local-Z or gradients).
    bool                                needs_sublayers = false;
    // Full Spectrum's minimum sub-layer height, for mixed_color_sublayer_min_height.
    double                              min_sublayer_height = 0.;
    // Human-readable notes about approximations, one per affected row.
    std::vector<std::string>            warnings;
};

// Read the Full Spectrum keys and the physical filament colours from a 3MF project's
// Metadata/project_settings.config. Returns false when the archive cannot be read or has no
// Full Spectrum mixed rows.
bool read_full_spectrum_project_settings(const std::string           &path_3mf,
                                         FullSpectrumProjectSettings &settings,
                                         std::vector<std::string>    &physical_colours);

// Convert the Full Spectrum rows of a project with `physical_colours.size()` physical filaments.
// `max_slots` is the number of mixed slots the target project can still take.
FullSpectrumImportResult convert_full_spectrum_mixed_filaments(const FullSpectrumProjectSettings &settings,
                                                               const std::vector<std::string>    &physical_colours,
                                                               size_t                             max_slots);

} // namespace Slic3r

#endif // slic3r_Format_FullSpectrumProjectImport_hpp_
