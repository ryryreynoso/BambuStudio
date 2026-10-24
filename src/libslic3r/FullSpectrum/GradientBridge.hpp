#ifndef slic3r_FullSpectrum_GradientBridge_hpp_
#define slic3r_FullSpectrum_GradientBridge_hpp_

// Bridge from Bambu Studio's native mixed slots to the vendored Full Spectrum gradient code.

#include <vector>

namespace Slic3r {

// Full Spectrum's default "Middle filament window" (dithering_local_z_gradient_middle_filament_window).
constexpr double kFullSpectrumMiddleFilamentWindow = 0.22;

// Weights (summing to 1, aligned with `components`) of a multi-filament gradient at height
// progress `t` in [0, 1], sampled with Full Spectrum's sample_mixed_gradient(): the gradient walks
// the ordered components, blending one adjacent pair at a time, with a solid band of width
// `middle_window` around every inner component. `ratios` are the slot's component weights; a
// larger weight gives that component more of the gradient height.
std::vector<double> sample_multi_filament_gradient(const std::vector<unsigned int> &components,
                                                   const std::vector<double>       &ratios,
                                                   double                           t,
                                                   double                           middle_window = kFullSpectrumMiddleFilamentWindow);

} // namespace Slic3r

#endif // slic3r_FullSpectrum_GradientBridge_hpp_
