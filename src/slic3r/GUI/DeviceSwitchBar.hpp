#ifndef slic3r_DeviceSwitchBar_hpp_
#define slic3r_DeviceSwitchBar_hpp_

#include <wx/panel.h>
#include <wx/timer.h>

class Button;

namespace Slic3r {
namespace GUI {

// Strip at the top of the Device tab with one button per printer kind: the selected Bambu printer
// and the Snapmaker U1. Each shows the printer's state (e.g. "Printing 45%"), and a click opens
// that printer's own Device page whatever printer preset is selected for slicing. Hidden until a
// U1 address has been used, so Bambu-only setups look as before.
class DeviceSwitchBar : public wxPanel
{
public:
    DeviceSwitchBar(wxWindow *parent, bool u1_page);
    void update();

private:
    bool    m_u1_page;
    Button *m_bambu{nullptr};
    Button *m_u1{nullptr};
    wxTimer m_timer;
};

} // namespace GUI
} // namespace Slic3r

#endif
