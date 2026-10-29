#include "MoonrakerDevicePanel.hpp"

#include <cmath>
#include <map>
#include <thread>

#include <wx/dcbuffer.h>
#include <wx/msgdlg.h>
#include <wx/mstream.h>
#include <wx/notifmsg.h>
#include <wx/statbmp.h>
#include <wx/sizer.h>
#include <wx/textctrl.h>
#include <wx/utils.h>

#include <boost/log/trivial.hpp>
#include <nlohmann/json.hpp>

#include "DeviceSwitchBar.hpp"
#include "GUI_App.hpp"
#include "libslic3r/AppConfig.hpp"
#include "I18N.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/ProgressBar.hpp"
#include "Widgets/StateColor.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/Utils/Http.hpp"
#include "slic3r/Utils/Moonraker.hpp"

namespace Slic3r {
namespace GUI {

using json = nlohmann::json;

namespace {

const wxColour COLOUR_OK("#00AE42"); // Bambu's brand green
const wxColour COLOUR_WARN("#FF6F00");
const wxColour COLOUR_ERROR("#E14747");
const wxColour COLOUR_MUTED("#909090");
const wxColour COLOUR_CARD_BORDER("#EEEEEE");
const wxColour COLOUR_PAGE("#F7F7F7");

const int POLL_MS   = 3000;
// The U1 rewrites its camera picture about twice a second while the camera monitor is on.
const int CAMERA_MS = 500;
// The U1 lets the monitor go about five minutes after it was last asked for, so it is asked again
// this often while the page is open. Once the page is left it lapses even if the stop is ignored.
const int MONITOR_RENEW_S = 60;
// With the monitor on the picture changes every half second; this long without a change means the
// monitor lapsed or the camera stalled, and it is asked for again (at most every ASK_AGAIN_S).
const int CAMERA_STALE_S = 6;
const int ASK_AGAIN_S    = 10;
// While the page is hidden the status is still read, this many status ticks apart, for the
// printer switch bar and so a finished print can be announced.
const int HIDDEN_POLL_TICKS = 5;
const int THUMB_SIZE        = 96;

// Moonraker paths are URL paths: each part is encoded, the slashes are kept.
std::string encode_path(const std::string &path)
{
    std::string out;
    size_t      start = 0;
    while (true) {
        const size_t slash = path.find('/', start);
        out += Slic3r::Http::url_encode(path.substr(start, slash == std::string::npos ? std::string::npos : slash - start));
        if (slash == std::string::npos)
            break;
        out += '/';
        start = slash + 1;
    }
    return out;
}

// Klipper objects shown on the page. print_task_config (per-toolhead filament) and
// exception_manager (errors) are Snapmaker's; other Klipper printers just leave them out.
std::vector<std::string> queried_objects(size_t tools)
{
    std::vector<std::string> objects{"webhooks",   "print_stats", "display_status",     "virtual_sdcard",   "heater_bed", "gcode_move",
                                     "toolhead",   "print_task_config", "exception_manager", "temperature_sensor cavity"};
    for (size_t i = 0; i < tools; ++i)
        objects.push_back(i == 0 ? std::string("extruder") : "extruder" + std::to_string(i));
    return objects;
}

const json &field(const json &status, const std::string &object, const char *name)
{
    static const json none;
    if (!status.is_object() || !status.contains(object) || !status.at(object).is_object())
        return none;
    const json &obj = status.at(object);
    return obj.contains(name) ? obj.at(name) : none;
}

double number(const json &value, double fallback = 0.) { return value.is_number() ? value.get<double>() : fallback; }
std::string text(const json &value, const std::string &fallback = {}) { return value.is_string() ? value.get<std::string>() : fallback; }

wxString temperature(const json &status, const std::string &object)
{
    const json &now = field(status, object, "temperature");
    if (!now.is_number())
        return wxString::FromUTF8("—");
    const double target = number(field(status, object, "target"));
    if (target > 0)
        return wxString::Format(wxString::FromUTF8("%.0f / %.0f °C"), now.get<double>(), target);
    return wxString::Format(wxString::FromUTF8("%.0f °C"), now.get<double>());
}

wxString duration(double seconds) { return wxString::FromUTF8(short_time(get_time_dhms((float) std::max(0., seconds)))); }

// Snapmaker does not document its exception entries; show whatever text an entry has, with its
// code and level (same as the iPhone app).
wxString describe_exception(const json &entry)
{
    if (!entry.is_object())
        return wxString::FromUTF8(entry.dump());
    std::string message;
    for (const char *key : {"message", "msg", "description", "name"})
        if (entry.contains(key) && entry.at(key).is_string() && !entry.at(key).get<std::string>().empty()) {
            message = entry.at(key).get<std::string>();
            break;
        }
    std::string details;
    for (const char *key : {"code", "id", "level", "module", "index"})
        if (entry.contains(key) && !entry.at(key).is_null()) {
            const json &v = entry.at(key);
            details += (details.empty() ? "" : ", ") + std::string(key) + " " + (v.is_string() ? v.get<std::string>() : v.dump());
        }
    if (message.empty())
        return wxString::FromUTF8(details.empty() ? entry.dump() : details);
    return wxString::FromUTF8(details.empty() ? message : message + " (" + details + ")");
}

} // namespace

MoonrakerDevicePanel::MoonrakerDevicePanel(wxWindow *parent)
    : wxPanel(parent, wxID_ANY)
    , m_alive(std::make_shared<bool>(true))
    , m_status_busy(std::make_shared<bool>(false))
    , m_camera_busy(std::make_shared<bool>(false))
    , m_timer(this, wxWindow::NewControlId())
    , m_camera_timer(this, wxWindow::NewControlId())
{
    SetBackgroundColour(COLOUR_PAGE);
    build();
    Bind(wxEVT_TIMER, [this](wxTimerEvent &) {
        static int hidden_ticks = 0;
        if (IsShownOnScreen())
            poll();
        else {
            // Slowly, for the switch bar and the print-finished notice.
            if (++hidden_ticks >= HIDDEN_POLL_TICKS) {
                hidden_ticks = 0;
                poll();
            }
        }
    }, m_timer.GetId());
    Bind(wxEVT_TIMER, [this](wxTimerEvent &) {
        if (!IsShownOnScreen()) {
            if (m_monitor_on)
                set_monitor(false);
            return;
        }
        fetch_camera();
        // A picture that stopped changing: the monitor lapsed (the U1 drops it a few minutes after
        // the last request, or after a restart) or the camera stalled. Ask again, and say so.
        const wxDateTime now = wxDateTime::Now();
        if (m_online && m_camera_changed.IsValid() && (now - m_camera_changed).GetSeconds() >= CAMERA_STALE_S) {
            if (!m_monitor_sent.IsValid() || (now - m_monitor_sent).GetSeconds() >= ASK_AGAIN_S)
                set_monitor(true);
            if (m_camera_problem.empty())
                show_camera_problem(wxString::Format(_L("The picture stopped changing at %s. Asking the camera again..."), m_camera_changed.FormatTime()));
        }
    }, m_camera_timer.GetId());
    Bind(wxEVT_SHOW, [this](wxShowEvent &e) {
        e.Skip();
        if (e.IsShown())
            CallAfter([this]() { poll(); });
    });
    m_timer.Start(POLL_MS);
    m_camera_timer.Start(CAMERA_MS);
}

MoonrakerDevicePanel::~MoonrakerDevicePanel()
{
    *m_alive = false;
    m_timer.Stop();
    m_camera_timer.Stop();
    if (m_monitor_on)
        set_monitor(false);
}

wxPanel *MoonrakerDevicePanel::make_card(wxWindow *parent, const wxString &title, wxBoxSizer *&body)
{
    auto *card = new wxPanel(parent);
    card->SetBackgroundColour(*wxWHITE);
    card->Bind(wxEVT_PAINT, [card](wxPaintEvent &) {
        wxPaintDC dc(card);
        dc.SetPen(wxPen(COLOUR_CARD_BORDER));
        dc.SetBrush(*wxTRANSPARENT_BRUSH);
        dc.DrawRectangle(card->GetClientRect());
    });
    auto *outer = new wxBoxSizer(wxVERTICAL);
    auto *heading = new Label(card, title);
    heading->SetFont(Label::Head_14);
    outer->Add(heading, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(14));
    body = new wxBoxSizer(wxVERTICAL);
    outer->Add(body, 1, wxEXPAND | wxALL, FromDIP(14));
    card->SetSizer(outer);
    return card;
}

void MoonrakerDevicePanel::build()
{
    auto *page = new wxBoxSizer(wxVERTICAL);
    page->Add(new DeviceSwitchBar(this, true), 0, wxEXPAND | wxBOTTOM, FromDIP(1));

    // Printer bar.
    auto *bar_panel = new wxPanel(this);
    bar_panel->SetBackgroundColour(*wxWHITE);
    auto *bar = new wxBoxSizer(wxHORIZONTAL);
    m_name = new Label(bar_panel, _L("No printer"));
    m_name->SetFont(Label::Head_16);
    m_address = new Label(bar_panel, wxEmptyString);
    m_address->SetFont(Label::Body_13);
    m_address->SetForegroundColour(COLOUR_MUTED);
    m_state = new Label(bar_panel, wxEmptyString);
    m_state->SetFont(Label::Head_14);

    StateColor bg_white(std::pair<wxColour, int>(wxColour(206, 206, 206), StateColor::Pressed), std::pair<wxColour, int>(wxColour(238, 238, 238), StateColor::Hovered),
                        std::pair<wxColour, int>(*wxWHITE, StateColor::Normal));
    m_address_edit = new wxTextCtrl(bar_panel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(170), -1), wxTE_PROCESS_ENTER);
    m_address_edit->SetHint(_L("Printer address, e.g. 192.168.1.20"));
    m_address_edit->SetToolTip(_L("The printer's IP address or host name. 3D Print remembers it."));
    m_btn_connect = new Button(bar_panel, _L("Connect"));
    m_btn_refresh = new Button(bar_panel, _L("Refresh"));
    m_btn_web     = new Button(bar_panel, _L("Open web page"));
    for (Button *btn : {m_btn_connect, m_btn_refresh, m_btn_web}) {
        btn->SetBackgroundColor(bg_white);
        btn->SetBorderColor(wxColour(38, 46, 48));
        btn->SetMinSize(wxSize(-1, FromDIP(28)));
        btn->SetCornerRadius(FromDIP(14));
    }
    m_btn_refresh->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { poll(); });
    m_btn_connect->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { connect_to(m_address_edit->GetValue()); });
    m_address_edit->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent &) { connect_to(m_address_edit->GetValue()); });
    m_btn_web->SetToolTip(_L("Open the printer's own web page (Fluidd) in your browser."));
    m_btn_web->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        if (!m_web_url.empty())
            wxLaunchDefaultBrowser(wxString::FromUTF8(m_web_url));
    });

    bar->Add(m_name, 0, wxALIGN_CENTER_VERTICAL);
    // The address is shown (and edited) in the field on the right.
    m_address->Hide();
    bar->Add(m_address, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(12));
    bar->Add(m_state, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(18));
    bar->AddStretchSpacer();
    bar->Add(m_address_edit, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    bar->Add(m_btn_connect, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(18));
    bar->Add(m_btn_refresh, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(10));
    bar->Add(m_btn_web, 0, wxALIGN_CENTER_VERTICAL);
    auto *bar_outer = new wxBoxSizer(wxVERTICAL);
    bar_outer->Add(bar, 0, wxEXPAND | wxALL, FromDIP(16));
    bar_panel->SetSizer(bar_outer);
    page->Add(bar_panel, 0, wxEXPAND);

    auto *columns = new wxBoxSizer(wxHORIZONTAL);

    // Left: camera and print job.
    auto *left = new wxBoxSizer(wxVERTICAL);
    wxBoxSizer *camera_body = nullptr;
    wxPanel    *camera_card = make_card(this, _L("Camera"), camera_body);
    m_camera = new wxPanel(camera_card, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(480), FromDIP(270)));
    m_camera->SetBackgroundStyle(wxBG_STYLE_PAINT);
    m_camera->Bind(wxEVT_PAINT, [this](wxPaintEvent &) {
        wxAutoBufferedPaintDC dc(m_camera);
        const wxSize size = m_camera->GetClientSize();
        dc.SetBackground(wxBrush(wxColour(24, 24, 24)));
        dc.Clear();
        if (!m_camera_image.IsOk()) {
            dc.SetTextForeground(wxColour(200, 200, 200));
            dc.SetFont(Label::Body_14);
            const wxString msg = m_camera_problem.empty() ? _L("Waiting for the camera picture...") : m_camera_problem;
            const wxSize   ext = dc.GetTextExtent(msg);
            dc.DrawText(msg, std::max(0, (size.x - ext.x) / 2), (size.y - ext.y) / 2);
            return;
        }
        const double scale = std::min(size.x / (double) m_camera_image.GetWidth(), size.y / (double) m_camera_image.GetHeight());
        const int    w = std::max(1, (int) std::lround(m_camera_image.GetWidth() * scale));
        const int    h = std::max(1, (int) std::lround(m_camera_image.GetHeight() * scale));
        dc.DrawBitmap(wxBitmap(m_camera_image.Scale(w, h, wxIMAGE_QUALITY_BILINEAR)), (size.x - w) / 2, (size.y - h) / 2);
        if (!m_camera_problem.empty()) {
            // An old picture stays visible, marked as old.
            dc.SetFont(Label::Body_13);
            const wxSize ext = dc.GetTextExtent(m_camera_problem);
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.SetBrush(wxBrush(wxColour(0, 0, 0)));
            dc.DrawRectangle(0, size.y - ext.y - FromDIP(12), size.x, ext.y + FromDIP(12));
            dc.SetTextForeground(wxColour(255, 190, 120));
            dc.DrawText(m_camera_problem, FromDIP(10), size.y - ext.y - FromDIP(6));
        }
    });
    m_camera->Bind(wxEVT_SIZE, [this](wxSizeEvent &e) {
        m_camera->Refresh();
        e.Skip();
    });
    camera_body->Add(m_camera, 1, wxEXPAND);
    m_camera_note = new Label(camera_card, _L("Live from the U1's camera, about two pictures a second."));
    m_camera_note->SetFont(Label::Body_12);
    m_camera_note->SetForegroundColour(COLOUR_MUTED);
    camera_body->Add(m_camera_note, 0, wxTOP, FromDIP(8));
    left->Add(camera_card, 1, wxEXPAND | wxBOTTOM, FromDIP(12));

    wxBoxSizer *job_body = nullptr;
    wxPanel    *job_card = make_card(this, _L("Print job"), job_body);
    wxImage     blank(FromDIP(THUMB_SIZE), FromDIP(THUMB_SIZE));
    blank.SetRGB(wxRect(0, 0, blank.GetWidth(), blank.GetHeight()), 238, 238, 238);
    m_job_thumb = new wxStaticBitmap(job_card, wxID_ANY, wxBitmap(blank));
    m_job_thumb->SetToolTip(_L("The print file's preview, as the slicer saved it."));
    m_job_file = new Label(job_card, _L("No print"));
    m_job_file->SetFont(Label::Head_13);
    m_job_state = new Label(job_card, wxEmptyString);
    m_job_state->SetFont(Label::Body_13);
    m_job_state->SetForegroundColour(COLOUR_MUTED);
    auto *progress_row = new wxBoxSizer(wxHORIZONTAL);
    m_job_progress = new ProgressBar(job_card, wxID_ANY, 100, wxDefaultPosition, wxSize(-1, FromDIP(8)));
    m_job_progress->SetHeight(FromDIP(8));
    m_job_progress->SetValue(0);
    m_job_percent = new Label(job_card, "0%");
    m_job_percent->SetFont(Label::Head_14);
    progress_row->Add(m_job_progress, 1, wxALIGN_CENTER_VERTICAL);
    progress_row->Add(m_job_percent, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(12));
    auto *stats = new wxBoxSizer(wxHORIZONTAL);
    m_job_layer     = new Label(job_card, wxEmptyString);
    m_job_elapsed   = new Label(job_card, wxEmptyString);
    m_job_remaining = new Label(job_card, wxEmptyString);
    m_job_eta       = new Label(job_card, wxEmptyString);
    m_job_speed     = new Label(job_card, wxEmptyString);
    for (Label *l : {m_job_layer, m_job_elapsed, m_job_remaining, m_job_eta, m_job_speed}) {
        l->SetFont(Label::Body_13);
        stats->Add(l, 0, wxRIGHT, FromDIP(20));
    }
    m_job_speed->SetToolTip(_L("Klipper's speed factor (M220) the printer reports."));

    StateColor job_btn_bg(std::pair<wxColour, int>(wxColour(206, 206, 206), StateColor::Pressed), std::pair<wxColour, int>(wxColour(238, 238, 238), StateColor::Hovered),
                          std::pair<wxColour, int>(*wxWHITE, StateColor::Normal));
    auto *controls = new wxBoxSizer(wxHORIZONTAL);
    m_btn_pause  = new Button(job_card, _L("Pause"));
    m_btn_resume = new Button(job_card, _L("Resume"));
    m_btn_cancel = new Button(job_card, _L("Stop"));
    for (Button *btn : {m_btn_pause, m_btn_resume, m_btn_cancel}) {
        btn->SetBackgroundColor(job_btn_bg);
        btn->SetBorderColor(wxColour(38, 46, 48));
        btn->SetMinSize(wxSize(FromDIP(80), FromDIP(28)));
        btn->SetCornerRadius(FromDIP(14));
        btn->Enable(false);
        controls->Add(btn, 0, wxRIGHT, FromDIP(10));
    }
    m_btn_cancel->SetTextColor(StateColor(std::pair<wxColour, int>(COLOUR_ERROR, StateColor::Normal)));
    m_btn_pause->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { print_command("printer/print/pause", _L("Pause the print on the U1?")); });
    m_btn_resume->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { print_command("printer/print/resume", _L("Resume the print on the U1?")); });
    m_btn_cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        print_command("printer/print/cancel", _L("Stop the print on the U1? It cannot be resumed afterwards."));
    });

    auto *job_text = new wxBoxSizer(wxVERTICAL);
    job_text->Add(m_job_file, 0, wxBOTTOM, FromDIP(4));
    job_text->Add(m_job_state, 0, wxBOTTOM, FromDIP(10));
    job_text->Add(progress_row, 0, wxEXPAND | wxBOTTOM, FromDIP(10));
    job_text->Add(stats, 0);
    auto *job_row = new wxBoxSizer(wxHORIZONTAL);
    job_row->Add(m_job_thumb, 0, wxRIGHT, FromDIP(14));
    job_row->Add(job_text, 1, wxEXPAND);
    job_body->Add(job_row, 0, wxEXPAND | wxBOTTOM, FromDIP(12));
    job_body->Add(controls, 0);
    left->Add(job_card, 0, wxEXPAND);
    columns->Add(left, 1, wxEXPAND | wxRIGHT, FromDIP(12));

    // Right: temperatures, toolheads, errors.
    auto *right = new wxBoxSizer(wxVERTICAL);
    wxBoxSizer *temp_body = nullptr;
    wxPanel    *temp_card = make_card(this, _L("Temperatures"), temp_body);
    auto       *temp_grid = new wxFlexGridSizer(0, 2, FromDIP(8), FromDIP(24));
    temp_grid->AddGrowableCol(1);
    auto add_temp = [&](const wxString &name, Label *&value) {
        auto *l = new Label(temp_card, name);
        l->SetFont(Label::Body_13);
        value = new Label(temp_card, wxString::FromUTF8("—"));
        value->SetFont(Label::Head_13);
        temp_grid->Add(l, 0);
        temp_grid->Add(value, 0, wxALIGN_RIGHT);
    };
    add_temp(_L("Bed"), m_bed_temp);
    add_temp(_L("Chamber"), m_chamber_temp);
    temp_body->Add(temp_grid, 0, wxEXPAND);
    right->Add(temp_card, 0, wxEXPAND | wxBOTTOM, FromDIP(12));

    wxBoxSizer *tools_body = nullptr;
    wxPanel    *tools_card = make_card(this, _L("Toolheads"), tools_body);
    auto       *tools_grid = new wxFlexGridSizer(0, 4, FromDIP(10), FromDIP(10));
    tools_grid->AddGrowableCol(1);
    for (int i = 0; i < 4; ++i) {
        ToolRow row;
        row.swatch = new wxWindow(tools_card, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(22), FromDIP(22)), wxBORDER_SIMPLE);
        row.swatch->SetBackgroundColour(wxColour(238, 238, 238));
        auto *names = new wxBoxSizer(wxVERTICAL);
        row.name = new Label(tools_card, wxString::Format(_L("Toolhead %d"), i + 1));
        row.name->SetFont(Label::Head_13);
        row.filament = new Label(tools_card, wxString::FromUTF8("—"));
        row.filament->SetFont(Label::Body_12);
        row.filament->SetForegroundColour(COLOUR_MUTED);
        names->Add(row.name, 0);
        names->Add(row.filament, 0);
        row.temperature = new Label(tools_card, wxString::FromUTF8("—"));
        row.temperature->SetFont(Label::Body_13);
        row.active = new Label(tools_card, wxEmptyString);
        row.active->SetFont(Label::Head_12);
        row.active->SetForegroundColour(COLOUR_OK);
        tools_grid->Add(row.swatch, 0, wxALIGN_CENTER_VERTICAL);
        tools_grid->Add(names, 0, wxALIGN_CENTER_VERTICAL);
        tools_grid->Add(row.temperature, 0, wxALIGN_CENTER_VERTICAL);
        tools_grid->Add(row.active, 0, wxALIGN_CENTER_VERTICAL);
        m_tools.push_back(row);
    }
    tools_body->Add(tools_grid, 0, wxEXPAND);
    right->Add(tools_card, 0, wxEXPAND | wxBOTTOM, FromDIP(12));

    m_errors_card = make_card(this, _L("Errors"), m_errors_body);
    right->Add(m_errors_card, 0, wxEXPAND);
    right->SetMinSize(wxSize(FromDIP(340), -1));
    columns->Add(right, 0, wxEXPAND);

    page->Add(columns, 1, wxEXPAND | wxALL, FromDIP(16));
    SetSizer(page);
    show_offline(wxEmptyString);
}

std::string MoonrakerDevicePanel::remembered_host()
{
    return wxGetApp().app_config ? wxGetApp().app_config->get("moonraker_host") : std::string();
}

void MoonrakerDevicePanel::set_printer(const std::string &name, const DynamicPrintConfig &config)
{
    // The address set on the preset wins and is remembered; a preset without one (the Snapmaker
    // system preset) uses the last address that worked, so the page survives a preset switch.
    std::string host = config.has("print_host") ? config.opt_string("print_host") : std::string();
    if (!host.empty() && wxGetApp().app_config)
        wxGetApp().app_config->set("moonraker_host", host);
    if (host.empty())
        host = remembered_host();
    if (m_host && host == m_host_address && m_name->GetLabel() == wxString::FromUTF8(name))
        return;
    m_config.reset(new DynamicPrintConfig(config));
    m_name->SetLabel(wxString::FromUTF8(name));
    apply_host(host);
}

void MoonrakerDevicePanel::connect_to(const wxString &address)
{
    std::string host = into_u8(address.Strip(wxString::both));
    if (host.empty() || !m_config)
        return;
    if (wxGetApp().app_config)
        wxGetApp().app_config->set("moonraker_host", host);
    apply_host(host);
}

void MoonrakerDevicePanel::apply_host(const std::string &host)
{
    if (m_monitor_on)
        set_monitor(false); // on the printer the page is leaving
    m_host_address = host;
    m_address->SetLabel(wxString::FromUTF8(host));
    m_address_edit->ChangeValue(wxString::FromUTF8(host));

    std::string web = m_config->has("print_host_webui") ? m_config->opt_string("print_host_webui") : std::string();
    if (web.empty())
        web = host;
    if (!web.empty() && web.rfind("http", 0) != 0)
        web = "http://" + web;
    m_web_url = web;
    m_btn_web->Enable(!web.empty());

    // A copy, so the poll threads never see the preset change under them.
    DynamicPrintConfig copy(*m_config);
    copy.set_key_value("print_host", new ConfigOptionString(host));
    m_host.reset(host.empty() ? nullptr : PrintHost::get_print_host(&copy));
    m_camera_image   = wxImage();
    m_camera_hash    = 0;
    m_camera_changed = wxDateTime();
    m_camera_problem.clear();
    m_online         = false;
    m_last_job_state.clear();
    m_thumb_file.clear();
    m_camera->Refresh();
    show_offline(host.empty() ? _L("Type the printer's address at the top right and press Connect.") : wxString());
    Layout();
    if (IsShownOnScreen())
        poll();
}

void MoonrakerDevicePanel::poll()
{
    auto *moonraker = dynamic_cast<Moonraker *>(m_host.get());
    if (!moonraker)
        return;
    std::shared_ptr<PrintHost> host  = m_host;
    std::shared_ptr<bool>      alive = m_alive;

    if (!*m_status_busy) {
        *m_status_busy  = true;
        auto busy       = m_status_busy;
        auto objects    = queried_objects(m_tools.size());
        std::thread([this, host, alive, busy, objects]() {
            wxString    error;
            std::string reply = static_cast<Moonraker *>(host.get())->query_objects(objects, error);
            wxGetApp().CallAfter([this, host, alive, busy, reply, error]() {
                *busy = false;
                if (*alive && host == m_host)
                    apply_status(reply, error);
            });
        }).detach();
    }
    if (m_online && IsShownOnScreen() &&
        (!m_monitor_on || !m_monitor_sent.IsValid() || (wxDateTime::Now() - m_monitor_sent).GetSeconds() >= MONITOR_RENEW_S))
        set_monitor(true);
}

void MoonrakerDevicePanel::fetch_camera()
{
    auto *moonraker = dynamic_cast<Moonraker *>(m_host.get());
    if (!moonraker || *m_camera_busy)
        return;
    std::shared_ptr<PrintHost> host  = m_host;
    std::shared_ptr<bool>      alive = m_alive;
    *m_camera_busy = true;
    auto busy      = m_camera_busy;
    // The query string only defeats caches on the way; Moonraker ignores it.
    const std::string path = "camera/monitor.jpg?t=" + std::to_string(wxGetUTCTimeMillis().GetValue());
    std::thread([this, host, alive, busy, path]() {
        wxString    error;
        std::string jpeg;
        if (!static_cast<Moonraker *>(host.get())->get_file(path, jpeg, error))
            jpeg.clear();
        wxGetApp().CallAfter([this, host, alive, busy, jpeg, error]() {
            *busy = false;
            if (!*alive || host != m_host)
                return;
            if (!jpeg.empty()) {
                apply_camera(jpeg);
                return;
            }
            const std::string text = into_u8(error);
            if (text.find("401") != std::string::npos || text.find("403") != std::string::npos)
                show_camera_problem(_L("The printer refused the camera picture. If its Moonraker needs an API key, enter it in the printer's connection settings."));
            else if (text.find("404") != std::string::npos)
                show_camera_problem(_L("The printer has no camera picture yet. Asking the camera to start..."));
            else if (!error.empty())
                show_camera_problem(_L("No camera picture") + ": " + error);
        });
    }).detach();
}

void MoonrakerDevicePanel::set_monitor(bool on)
{
    if (!dynamic_cast<Moonraker *>(m_host.get()))
        return;
    m_monitor_on   = on;
    m_monitor_sent = wxDateTime::Now();
    std::shared_ptr<PrintHost> host = m_host;
    // Only the U1's camera is switched on or off; nothing moves or heats.
    std::thread([host, on]() {
        wxString error;
        bool     confirmed = false;
        if (!static_cast<Moonraker *>(host.get())->camera_monitor(on, error, &confirmed))
            BOOST_LOG_TRIVIAL(warning) << "U1 camera monitor " << (on ? "start" : "stop") << " failed: " << into_u8(error);
        else
            BOOST_LOG_TRIVIAL(info) << "U1 camera monitor " << (on ? "start" : "stop") << (confirmed ? " confirmed" : " sent (no change reported)");
    }).detach();
}

void MoonrakerDevicePanel::apply_camera(const std::string &jpeg)
{
    const size_t hash = std::hash<std::string>()(jpeg);
    m_camera_time     = wxDateTime::Now();
    if (hash == m_camera_hash)
        return; // the U1 has not written a new picture since the last request
    wxMemoryInputStream stream(jpeg.data(), jpeg.size());
    wxImage             image;
    if (!image.LoadFile(stream, wxBITMAP_TYPE_JPEG))
        return; // caught half-written; the next request gets the whole picture
    // The first picture after connecting may be old; only a change shows the camera is live.
    const bool first  = m_camera_hash == 0;
    m_camera_hash     = hash;
    m_camera_image    = image;
    if (!first) {
        m_camera_changed = m_camera_time;
        m_camera_problem.clear();
        m_camera_note->SetForegroundColour(COLOUR_MUTED);
        m_camera_note->SetLabel(wxString::Format(_L("Live from the U1's camera, about two pictures a second. Last picture at %s."), m_camera_time.FormatTime()));
    } else {
        m_camera_changed = m_camera_time; // the stale check starts from here
        m_camera_note->SetLabel(_L("Starting the camera..."));
    }
    m_camera->Refresh();
}

void MoonrakerDevicePanel::show_camera_problem(const wxString &problem)
{
    if (problem == m_camera_problem)
        return;
    m_camera_problem = problem;
    m_camera_note->SetForegroundColour(COLOUR_WARN);
    m_camera_note->SetLabel(problem);
    m_camera->Refresh();
}

void MoonrakerDevicePanel::fetch_thumbnail(const std::string &file)
{
    m_thumb_file = file;
    wxImage blank(FromDIP(THUMB_SIZE), FromDIP(THUMB_SIZE));
    blank.SetRGB(wxRect(0, 0, blank.GetWidth(), blank.GetHeight()), 238, 238, 238);
    m_job_thumb->SetBitmap(wxBitmap(blank));
    if (file.empty() || !dynamic_cast<Moonraker *>(m_host.get()))
        return;
    std::shared_ptr<PrintHost> host  = m_host;
    std::shared_ptr<bool>      alive = m_alive;
    std::thread([this, host, alive, file]() {
        auto       *moonraker = static_cast<Moonraker *>(host.get());
        wxString    error;
        std::string png;
        double      estimate = 0.;
        // The slicer's thumbnails, as Moonraker lists them in the file's metadata; paths are
        // relative to the file's folder.
        const json meta = json::parse(moonraker->get("server/files/metadata?filename=" + Http::url_encode(file), error), nullptr, false);
        if (meta.is_object() && meta.contains("result")) {
            const json &result = meta["result"];
            estimate           = number(result.value("estimated_time", json()));
            const json &thumbs = result.value("thumbnails", json::array());
            const json *best   = nullptr;
            for (const json &t : thumbs)
                if (t.is_object() && t.contains("relative_path") && (!best || number(t.value("width", json())) > number(best->value("width", json()))))
                    best = &t;
            if (best) {
                const size_t      slash  = file.rfind('/');
                const std::string folder = slash == std::string::npos ? std::string() : file.substr(0, slash + 1);
                moonraker->get_file("gcodes/" + encode_path(folder + best->at("relative_path").get<std::string>()), png, error);
            }
        }
        wxGetApp().CallAfter([this, host, alive, file, png, estimate]() {
            if (!*alive || host != m_host || file != m_thumb_file)
                return;
            m_job_estimate = estimate;
            wxMemoryInputStream stream(png.data(), png.size());
            wxImage             image;
            if (png.empty() || !image.LoadFile(stream, wxBITMAP_TYPE_ANY))
                return;
            const int    side  = FromDIP(THUMB_SIZE);
            const double scale = std::min(side / (double) image.GetWidth(), side / (double) image.GetHeight());
            image.Rescale(std::max(1, (int) (image.GetWidth() * scale)), std::max(1, (int) (image.GetHeight() * scale)), wxIMAGE_QUALITY_HIGH);
            image.Resize(wxSize(side, side), wxPoint((side - image.GetWidth()) / 2, (side - image.GetHeight()) / 2), 238, 238, 238);
            m_job_thumb->SetBitmap(wxBitmap(image));
            Layout();
        });
    }).detach();
}

void MoonrakerDevicePanel::print_command(const std::string &endpoint, const wxString &question)
{
    if (!dynamic_cast<Moonraker *>(m_host.get()))
        return;
    wxMessageDialog confirm(this, question, _L("Snapmaker U1"), wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION);
    if (confirm.ShowModal() != wxID_YES)
        return;
    std::shared_ptr<PrintHost> host  = m_host;
    std::shared_ptr<bool>      alive = m_alive;
    for (Button *btn : {m_btn_pause, m_btn_resume, m_btn_cancel})
        btn->Enable(false); // until the next status shows what the printer did
    std::thread([this, host, alive, endpoint]() {
        wxString error;
        const bool ok = static_cast<Moonraker *>(host.get())->post(endpoint, error);
        wxGetApp().CallAfter([this, host, alive, ok, error]() {
            if (!*alive || host != m_host)
                return;
            if (!ok)
                wxMessageBox(_L("The printer did not take the command") + ":\n" + error, _L("Snapmaker U1"), wxOK | wxICON_ERROR, this);
            poll();
        });
    }).detach();
}

void MoonrakerDevicePanel::notify_job_end(const std::string &state, const std::string &file)
{
    wxString title;
    if (state == "complete")
        title = _L("Print finished");
    else if (state == "error")
        title = _L("Print stopped with an error");
    else if (state == "cancelled")
        title = _L("Print stopped");
    else
        return;
    wxNotificationMessage notice(title, m_name->GetLabel() + ": " + wxString::FromUTF8(file), this,
                                 state == "complete" ? wxICON_INFORMATION : wxICON_WARNING);
    notice.Show(wxNotificationMessage::Timeout_Auto);
}

void MoonrakerDevicePanel::show_offline(const wxString &error)
{
    if (!error.empty()) {
        // Ask for the camera again as soon as the printer is back.
        m_online     = false;
        m_monitor_on = false;
        for (Button *btn : {m_btn_pause, m_btn_resume, m_btn_cancel})
            btn->Enable(false);
        if (m_camera_image.IsOk() || m_camera_hash)
            show_camera_problem(_L("The printer is not answering. The picture is from") + " " + m_camera_time.FormatTime() + ".");
    }
    m_state->SetLabel(error.empty() ? (m_host ? _L("Connecting...") : wxString()) : _L("Offline"));
    m_summary = error.empty() ? (m_host ? _L("Connecting...") : _L("Not connected")) : _L("Offline");
    m_state->SetForegroundColour(error.empty() ? COLOUR_MUTED : COLOUR_ERROR);
    m_state->SetToolTip(error);
    if (!error.empty()) {
        m_job_state->SetLabel(error);
    }
}

void MoonrakerDevicePanel::apply_status(const std::string &reply, const wxString &error)
{
    json status;
    try {
        if (!reply.empty())
            status = json::parse(reply).at("result").at("status");
    } catch (const std::exception &) {
        status = json();
    }
    if (status.is_null()) {
        show_offline(error.empty() ? _L("The printer did not answer.") : error);
        Layout();
        return;
    }

    if (!m_online) {
        m_online = true;
        if (IsShownOnScreen() && !m_monitor_on)
            set_monitor(true);
    }

    // Printer state.
    const std::string klippy = text(field(status, "webhooks", "state"), "ready");
    const std::string job    = text(field(status, "print_stats", "state"));
    wxString          state_text;
    wxColour          state_colour = COLOUR_OK;
    if (klippy != "ready") {
        state_text   = _L("Not ready") + ": " + wxString::FromUTF8(text(field(status, "webhooks", "state_message"), klippy));
        state_colour = COLOUR_ERROR;
    } else if (job == "printing") {
        state_text   = _L("Printing");
        state_colour = COLOUR_WARN;
    } else if (job == "paused") {
        state_text   = _L("Paused");
        state_colour = COLOUR_WARN;
    } else if (job == "error") {
        state_text   = _L("Error");
        state_colour = COLOUR_ERROR;
    } else {
        state_text = _L("Ready");
    }
    m_state->SetLabel(state_text);
    m_state->SetForegroundColour(state_colour);
    m_state->SetToolTip(wxEmptyString);

    // Print job.
    const std::string file   = text(field(status, "print_stats", "filename"));
    const bool        active = job == "printing" || job == "paused";
    double            progress = number(field(status, "display_status", "progress"), number(field(status, "virtual_sdcard", "progress")));
    if (!active && job != "complete")
        progress = 0.;
    m_job_file->SetLabel(file.empty() ? _L("No print") : wxString::FromUTF8(file));
    if (file != m_thumb_file) {
        m_job_estimate = 0.;
        fetch_thumbnail(file);
    }
    if ((m_last_job_state == "printing" || m_last_job_state == "paused") && !active)
        notify_job_end(job, file);
    m_last_job_state = job;
    m_btn_pause->Enable(klippy == "ready" && job == "printing");
    m_btn_resume->Enable(klippy == "ready" && job == "paused");
    m_btn_cancel->Enable(klippy == "ready" && active);
    m_summary = active ? wxString::Format("%s %d%%", job == "paused" ? _L("Paused") : _L("Printing"), (int) std::lround(progress * 100.)) : state_text;
    static const std::map<std::string, wxString> job_names{{"standby", _L("Idle")},       {"printing", _L("Printing")}, {"paused", _L("Paused")},
                                                           {"complete", _L("Finished")}, {"cancelled", _L("Cancelled")}, {"error", _L("Stopped with an error")}};
    auto       named = job_names.find(job);
    wxString   job_text = named != job_names.end() ? named->second : wxString::FromUTF8(job);
    const std::string message = text(field(status, "print_stats", "message"));
    if (!message.empty())
        job_text += "  ·  " + wxString::FromUTF8(message);
    m_job_state->SetLabel(job_text);
    m_job_progress->SetValue((int) std::lround(progress * 100.));
    m_job_percent->SetLabel(wxString::Format("%d%%", (int) std::lround(progress * 100.)));

    const json &info  = field(status, "print_stats", "info");
    const int   layer = info.is_object() ? (int) number(info.value("current_layer", json()), 0) : 0;
    const int   total = info.is_object() ? (int) number(info.value("total_layer", json()), 0) : 0;
    m_job_layer->SetLabel(total > 0 ? wxString::Format(_L("Layer %d / %d"), layer, total) : wxString());
    const double elapsed = number(field(status, "print_stats", "print_duration"));
    m_job_elapsed->SetLabel(active || elapsed > 0 ? _L("Elapsed") + " " + duration(elapsed) : wxString());
    // Klipper reports no estimate of its own. Use the slicer's estimate from the file metadata, as
    // Fluidd does, else extrapolate from the file progress.
    double remaining = -1.;
    if (active && m_job_estimate > 0)
        remaining = std::max(0., m_job_estimate - elapsed);
    else if (active && progress > 0.01 && elapsed > 0)
        remaining = elapsed / progress - elapsed;
    m_job_remaining->SetLabel(remaining >= 0 ? _L("Remaining") + " ~" + duration(remaining) : wxString());
    m_job_eta->SetLabel(remaining >= 0 && job == "printing" ? _L("Done at") + " " + (wxDateTime::Now() + wxTimeSpan::Seconds((wxLongLong) remaining)).Format("%H:%M") : wxString());
    const json &speed = field(status, "gcode_move", "speed_factor");
    m_job_speed->SetLabel(active && speed.is_number() ? wxString::Format(_L("Speed %d%%"), (int) std::lround(speed.get<double>() * 100.)) : wxString());

    // Temperatures.
    m_bed_temp->SetLabel(temperature(status, "heater_bed"));
    m_chamber_temp->SetLabel(temperature(status, "temperature_sensor cavity"));

    // Toolheads.
    const json  tasks       = status.value("print_task_config", json::object());
    const std::string current = text(field(status, "toolhead", "extruder"));
    auto at = [&tasks](const char *key, size_t i) -> json {
        const json &values = tasks.is_object() && tasks.contains(key) ? tasks.at(key) : json();
        return values.is_array() && i < values.size() ? values.at(i) : json();
    };
    for (size_t i = 0; i < m_tools.size(); ++i) {
        ToolRow          &row    = m_tools[i];
        const std::string object = i == 0 ? std::string("extruder") : "extruder" + std::to_string(i);
        const bool        exists = status.contains(object);
        row.temperature->SetLabel(exists ? temperature(status, object) : wxString::FromUTF8("—"));
        row.active->SetLabel(exists && object == current ? _L("In use") : wxString());

        wxColour colour(238, 238, 238);
        wxString filament = wxString::FromUTF8("—");
        if (tasks.is_object() && !tasks.empty()) {
            const json loaded = at("filament_exist", i);
            if (loaded.is_boolean() && !loaded.get<bool>()) {
                filament = _L("Empty");
            } else {
                const std::string type = text(at("filament_type", i));
                const std::string sub  = text(at("filament_sub_type", i));
                const std::string vendor = text(at("filament_vendor", i));
                filament = wxString::FromUTF8((vendor.empty() ? "" : vendor + " ") + type + (sub.empty() ? "" : " " + sub));
                const std::string rgba = text(at("filament_color_rgba", i));
                if (rgba.size() >= 6)
                    colour = wxColour("#" + wxString::FromUTF8(rgba.substr(0, 6)));
            }
        }
        row.filament->SetLabel(filament);
        row.swatch->SetBackgroundColour(colour);
        row.swatch->Refresh();
    }

    // Errors.
    for (wxWindow *w : m_error_labels)
        w->Destroy();
    m_error_labels.clear();
    const json &exceptions = field(status, "exception_manager", "exceptions");
    if (exceptions.is_array() && !exceptions.empty()) {
        for (const json &entry : exceptions) {
            auto *l = new Label(m_errors_card, describe_exception(entry));
            l->SetFont(Label::Body_13);
            l->SetForegroundColour(COLOUR_ERROR);
            l->Wrap(FromDIP(300));
            m_errors_body->Add(l, 0, wxBOTTOM, FromDIP(6));
            m_error_labels.push_back(l);
        }
    } else {
        auto *l = new Label(m_errors_card, status.contains("exception_manager") ? wxString::FromUTF8("✓ ") + _L("No errors") : _L("This printer does not report errors."));
        l->SetFont(Label::Body_13);
        l->SetForegroundColour(status.contains("exception_manager") ? COLOUR_OK : COLOUR_MUTED);
        m_errors_body->Add(l, 0);
        m_error_labels.push_back(l);
    }
    Layout();
}

void MoonrakerDevicePanel::msw_rescale()
{
    m_btn_connect->Rescale();
    m_btn_refresh->Rescale();
    m_btn_web->Rescale();
    for (Button *btn : {m_btn_pause, m_btn_resume, m_btn_cancel})
        btn->Rescale();
    m_job_progress->Rescale();
    Layout();
}

} // namespace GUI
} // namespace Slic3r
