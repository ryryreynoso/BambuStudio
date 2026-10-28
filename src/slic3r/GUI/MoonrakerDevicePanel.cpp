#include "MoonrakerDevicePanel.hpp"

#include <cmath>
#include <map>
#include <thread>

#include <wx/dcbuffer.h>
#include <wx/mstream.h>
#include <wx/sizer.h>
#include <wx/utils.h>

#include <nlohmann/json.hpp>

#include "GUI_App.hpp"
#include "I18N.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/ProgressBar.hpp"
#include "Widgets/StateColor.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"
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

const int POLL_MS = 3000;

// Klipper objects shown on the page. print_task_config (per-toolhead filament) and
// exception_manager (errors) are Snapmaker's; other Klipper printers just leave them out.
std::vector<std::string> queried_objects(size_t tools)
{
    std::vector<std::string> objects{"webhooks",   "print_stats", "display_status",     "virtual_sdcard",   "heater_bed",
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
    , m_timer(this)
{
    SetBackgroundColour(COLOUR_PAGE);
    build();
    Bind(wxEVT_TIMER, [this](wxTimerEvent &) {
        if (IsShownOnScreen())
            poll();
    });
    Bind(wxEVT_SHOW, [this](wxShowEvent &e) {
        e.Skip();
        if (e.IsShown())
            CallAfter([this]() { poll(); });
    });
    m_timer.Start(POLL_MS);
}

MoonrakerDevicePanel::~MoonrakerDevicePanel()
{
    *m_alive = false;
    m_timer.Stop();
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
    m_btn_refresh = new Button(bar_panel, _L("Refresh"));
    m_btn_web     = new Button(bar_panel, _L("Open web page"));
    for (Button *btn : {m_btn_refresh, m_btn_web}) {
        btn->SetBackgroundColor(bg_white);
        btn->SetBorderColor(wxColour(38, 46, 48));
        btn->SetMinSize(wxSize(-1, FromDIP(28)));
        btn->SetCornerRadius(FromDIP(14));
    }
    m_btn_refresh->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { poll(); });
    m_btn_web->SetToolTip(_L("Open the printer's own web page (Fluidd) in your browser."));
    m_btn_web->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        if (!m_web_url.empty())
            wxLaunchDefaultBrowser(wxString::FromUTF8(m_web_url));
    });

    bar->Add(m_name, 0, wxALIGN_CENTER_VERTICAL);
    bar->Add(m_address, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(12));
    bar->Add(m_state, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(18));
    bar->AddStretchSpacer();
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
            const wxString msg = _L("Waiting for the camera picture...");
            const wxSize   ext = dc.GetTextExtent(msg);
            dc.DrawText(msg, (size.x - ext.x) / 2, (size.y - ext.y) / 2);
            return;
        }
        const double scale = std::min(size.x / (double) m_camera_image.GetWidth(), size.y / (double) m_camera_image.GetHeight());
        const int    w = std::max(1, (int) std::lround(m_camera_image.GetWidth() * scale));
        const int    h = std::max(1, (int) std::lround(m_camera_image.GetHeight() * scale));
        dc.DrawBitmap(wxBitmap(m_camera_image.Scale(w, h, wxIMAGE_QUALITY_BILINEAR)), (size.x - w) / 2, (size.y - h) / 2);
    });
    m_camera->Bind(wxEVT_SIZE, [this](wxSizeEvent &e) {
        m_camera->Refresh();
        e.Skip();
    });
    camera_body->Add(m_camera, 1, wxEXPAND);
    m_camera_note = new Label(camera_card, _L("The U1 takes a new picture every few seconds while it prints."));
    m_camera_note->SetFont(Label::Body_12);
    m_camera_note->SetForegroundColour(COLOUR_MUTED);
    camera_body->Add(m_camera_note, 0, wxTOP, FromDIP(8));
    left->Add(camera_card, 1, wxEXPAND | wxBOTTOM, FromDIP(12));

    wxBoxSizer *job_body = nullptr;
    wxPanel    *job_card = make_card(this, _L("Print job"), job_body);
    m_job_file = new Label(job_card, _L("No print"));
    m_job_file->SetFont(Label::Head_13);
    m_job_state = new Label(job_card, wxEmptyString);
    m_job_state->SetFont(Label::Body_13);
    m_job_state->SetForegroundColour(COLOUR_MUTED);
    auto *progress_row = new wxBoxSizer(wxHORIZONTAL);
    m_job_progress = new ProgressBar(job_card, wxID_ANY, 100, wxDefaultPosition, wxSize(-1, FromDIP(8)));
    m_job_progress->SetHeight(FromDIP(8));
    m_job_progress->SetProgressForedColour(COLOUR_OK);
    m_job_percent = new Label(job_card, "0%");
    m_job_percent->SetFont(Label::Head_14);
    progress_row->Add(m_job_progress, 1, wxALIGN_CENTER_VERTICAL);
    progress_row->Add(m_job_percent, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(12));
    auto *stats = new wxBoxSizer(wxHORIZONTAL);
    m_job_layer     = new Label(job_card, wxEmptyString);
    m_job_elapsed   = new Label(job_card, wxEmptyString);
    m_job_remaining = new Label(job_card, wxEmptyString);
    for (Label *l : {m_job_layer, m_job_elapsed, m_job_remaining}) {
        l->SetFont(Label::Body_13);
        stats->Add(l, 0, wxRIGHT, FromDIP(24));
    }
    job_body->Add(m_job_file, 0, wxBOTTOM, FromDIP(4));
    job_body->Add(m_job_state, 0, wxBOTTOM, FromDIP(10));
    job_body->Add(progress_row, 0, wxEXPAND | wxBOTTOM, FromDIP(10));
    job_body->Add(stats, 0);
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

void MoonrakerDevicePanel::set_printer(const std::string &name, const DynamicPrintConfig &config)
{
    const std::string host = config.has("print_host") ? config.opt_string("print_host") : std::string();
    if (m_host && host == m_host_address && m_name->GetLabel() == wxString::FromUTF8(name))
        return;
    m_host_address = host;
    m_name->SetLabel(wxString::FromUTF8(name));
    m_address->SetLabel(wxString::FromUTF8(host));

    std::string web = config.has("print_host_webui") ? config.opt_string("print_host_webui") : std::string();
    if (web.empty())
        web = host;
    if (!web.empty() && web.rfind("http", 0) != 0)
        web = "http://" + web;
    m_web_url = web;
    m_btn_web->Enable(!web.empty());

    // A copy, so the poll threads never see the preset change under them.
    DynamicPrintConfig copy(config);
    m_host.reset(host.empty() ? nullptr : PrintHost::get_print_host(&copy));
    m_camera_image = wxImage();
    m_camera->Refresh();
    show_offline(host.empty() ? _L("Add the printer's address with the Wi-Fi button next to the printer list.") : wxString());
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
    if (!*m_camera_busy) {
        *m_camera_busy = true;
        auto busy      = m_camera_busy;
        std::thread([this, host, alive, busy]() {
            wxString    error;
            std::string jpeg;
            if (!static_cast<Moonraker *>(host.get())->get_file("camera/monitor.jpg", jpeg, error))
                jpeg.clear();
            wxGetApp().CallAfter([this, host, alive, busy, jpeg]() {
                *busy = false;
                if (*alive && host == m_host && !jpeg.empty())
                    apply_camera(jpeg);
            });
        }).detach();
    }
}

void MoonrakerDevicePanel::apply_camera(const std::string &jpeg)
{
    wxMemoryInputStream stream(jpeg.data(), jpeg.size());
    wxImage             image;
    if (!image.LoadFile(stream, wxBITMAP_TYPE_JPEG))
        return;
    m_camera_image = image;
    m_camera_time  = wxDateTime::Now();
    m_camera_note->SetLabel(wxString::Format(_L("Picture loaded at %s. The U1 takes a new one every few seconds while it prints."), m_camera_time.FormatTime()));
    m_camera->Refresh();
}

void MoonrakerDevicePanel::show_offline(const wxString &error)
{
    m_state->SetLabel(error.empty() ? (m_host ? _L("Connecting...") : wxString()) : _L("Offline"));
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
    // Klipper reports no estimate; extrapolate from the file progress as Fluidd does.
    m_job_remaining->SetLabel(active && progress > 0.01 && elapsed > 0 ? _L("Remaining") + " ~" + duration(elapsed / progress - elapsed) : wxString());

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
    m_btn_refresh->Rescale();
    m_btn_web->Rescale();
    m_job_progress->Rescale();
    Layout();
}

} // namespace GUI
} // namespace Slic3r
