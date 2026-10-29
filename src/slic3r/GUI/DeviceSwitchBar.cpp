#include "DeviceSwitchBar.hpp"

#include <wx/sizer.h>

#include "DeviceManager.hpp"
#include "DeviceCore/DevManager.h"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "MoonrakerDevicePanel.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/StateColor.hpp"

namespace Slic3r {
namespace GUI {

namespace {

wxString bambu_label()
{
    DeviceManager *dm  = wxGetApp().getDeviceManager();
    MachineObject *obj = dm ? dm->get_selected_machine() : nullptr;
    if (!obj)
        return _L("Bambu printer") + wxString::FromUTF8(" · ") + _L("Not connected");
    wxString state;
    if (!obj->is_online())
        state = _L("Offline");
    else if (obj->is_in_printing_pause())
        state = wxString::Format(_L("Paused %d%%"), obj->mc_print_percent);
    else if (obj->is_in_printing())
        state = wxString::Format(_L("Printing %d%%"), obj->mc_print_percent);
    else if (obj->print_status == "FINISH")
        state = _L("Finished");
    else if (obj->print_status == "FAILED")
        state = _L("Stopped");
    else
        state = _L("Idle");
    return wxString::FromUTF8(obj->get_dev_name()) + wxString::FromUTF8(" · ") + state;
}

} // namespace

DeviceSwitchBar::DeviceSwitchBar(wxWindow *parent, bool u1_page)
    : wxPanel(parent, wxID_ANY), m_u1_page(u1_page), m_timer(this)
{
    // Colours StateColor's dark-mode map leaves alone, chosen per theme here, so the text always
    // stands out from its button.
    const bool     dark      = wxGetApp().dark_mode();
    const wxColour bar_bg    = dark ? wxColour("#1F1F23") : wxColour("#FDFDFD");
    const wxColour idle_bg   = dark ? wxColour("#33333A") : wxColour("#F2F2F3");
    const wxColour idle_hov  = dark ? wxColour("#45454D") : wxColour("#E2E2E4");
    const wxColour idle_text = dark ? wxColour("#F2F2F2") : wxColour("#1E1E1E");
    const wxColour idle_line = dark ? wxColour("#6A6A72") : wxColour("#B8B8BC");
    const wxColour sel_bg("#00A040");
    const wxColour sel_text("#FDFDFD");
    SetBackgroundColour(bar_bg);
    auto *sizer = new wxBoxSizer(wxHORIZONTAL);
    auto *title = new Label(this, _L("Printers"));
    title->SetFont(Label::Head_13);
    title->SetForegroundColour(dark ? wxColour("#C8C8CC") : wxColour("#55555A"));
    sizer->Add(title, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, FromDIP(14));

    auto make = [&](bool selected) {
        auto *btn = new Button(this, wxEmptyString);
        if (selected) {
            btn->SetBackgroundColor(StateColor(std::pair<wxColour, int>(sel_bg, StateColor::Normal)));
            btn->SetBorderColor(StateColor(std::pair<wxColour, int>(sel_bg, StateColor::Normal)));
            btn->SetTextColor(StateColor(std::pair<wxColour, int>(sel_text, StateColor::Normal)));
            btn->SetFont(Label::Head_13);
        } else {
            btn->SetBackgroundColor(StateColor(std::pair<wxColour, int>(idle_hov, StateColor::Hovered), std::pair<wxColour, int>(idle_bg, StateColor::Normal)));
            btn->SetBorderColor(StateColor(std::pair<wxColour, int>(idle_line, StateColor::Normal)));
            btn->SetTextColor(StateColor(std::pair<wxColour, int>(idle_text, StateColor::Normal)));
            btn->SetFont(Label::Body_13);
        }
        btn->SetMinSize(wxSize(FromDIP(200), FromDIP(28)));
        btn->SetCornerRadius(FromDIP(14));
        btn->SetCursor(wxCURSOR_HAND);
        sizer->Add(btn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(10));
        return btn;
    };
    m_bambu = make(!u1_page);
    m_u1    = make(u1_page);
    m_bambu->SetToolTip(_L("Show the Bambu printer's Device page (the one selected in its printer list)."));
    m_u1->SetToolTip(_L("Show the Snapmaker U1's Device page."));
    m_bambu->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        if (m_u1_page && wxGetApp().mainframe)
            wxGetApp().mainframe->show_device_page(false);
    });
    m_u1->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        if (!m_u1_page && wxGetApp().mainframe)
            wxGetApp().mainframe->show_device_page(true);
    });

    auto *outer = new wxBoxSizer(wxVERTICAL);
    outer->Add(sizer, 0, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(8));
    SetSizer(outer);

    Bind(wxEVT_TIMER, [this](wxTimerEvent &) { update(); });
    Bind(wxEVT_SHOW, [this](wxShowEvent &e) {
        e.Skip();
        if (e.IsShown())
            update();
    });
    m_timer.Start(2000);
    update();
}

void DeviceSwitchBar::update()
{
    // Only shown once there is a U1 to switch to.
    const bool has_u1 = !MoonrakerDevicePanel::remembered_host().empty();
    if (has_u1 != IsShown()) {
        Show(has_u1);
        if (GetParent())
            GetParent()->Layout();
    }
    if (!has_u1 || !GetParent() || !GetParent()->IsShownOnScreen())
        return;
    const wxString bambu = bambu_label();
    MainFrame     *frame = wxGetApp().mainframe;
    const wxString u1    = _L("Snapmaker U1") + wxString::FromUTF8(" · ") +
                        (frame && frame->m_moonraker_device ? frame->m_moonraker_device->summary() : _L("Not connected"));
    if (m_bambu->GetLabel() != bambu)
        m_bambu->SetLabel(bambu);
    if (m_u1->GetLabel() != u1)
        m_u1->SetLabel(u1);
    Layout();
}

} // namespace GUI
} // namespace Slic3r
