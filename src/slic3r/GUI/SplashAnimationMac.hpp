#ifndef slic3r_SplashAnimationMac_hpp_
#define slic3r_SplashAnimationMac_hpp_

#include <string>

namespace Slic3r {
namespace GUI {

// macOS start-up splash in the app icon's design: a nozzle laying the rainbow layers one by one,
// back and forth, on the icon's dark tile. It is drawn with Core Animation, which keeps animating
// in the window server while the app's main thread is busy loading presets.
class SplashAnimationMac
{
public:
    SplashAnimationMac(const std::string &title, const std::string &version);
    ~SplashAnimationMac(); // fades the window out once the stack is finished
    SplashAnimationMac(const SplashAnimationMac &) = delete;
    SplashAnimationMac &operator=(const SplashAnimationMac &) = delete;

    // The loading step shown under the title.
    void set_text(const std::string &text);

private:
    void *m_window{nullptr}; // NSWindow
    void *m_status{nullptr}; // CATextLayer
    double m_started{0};     // CACurrentMediaTime() when shown
};

} // namespace GUI
} // namespace Slic3r

#endif
