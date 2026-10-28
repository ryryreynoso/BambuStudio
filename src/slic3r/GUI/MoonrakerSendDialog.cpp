#include "MoonrakerSendDialog.hpp"

#include <algorithm>
#include <thread>

#include <wx/sizer.h>
#include <wx/statbmp.h>
#include <wx/textctrl.h>

#include <nlohmann/json.hpp>

#include "GUI_App.hpp"
#include "I18N.hpp"
#include "PartPlate.hpp"
#include "Plater.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/StateColor.hpp"
#include "wxExtensions.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/Utils/Moonraker.hpp"

namespace Slic3r {
namespace GUI {

using json = nlohmann::json;

namespace {

const wxColour COLOUR_OK("#00AE42");     // Bambu's brand green
const wxColour COLOUR_WARN("#FF6F00");   // Bambu's warning orange
const wxColour COLOUR_ERROR("#E14747");
const wxColour COLOUR_MUTED("#909090");

// Klipper objects read for the dialog. print_task_config is Snapmaker's: the filament the printer
// detected or was told is in each tool.
const std::vector<std::string> QUERIED_OBJECTS{"webhooks", "print_stats", "print_task_config"};

wxWindow *make_swatch(wxWindow *parent, const wxColour &colour, int size)
{
    auto *swatch = new wxWindow(parent, wxID_ANY, wxDefaultPosition, wxSize(size, size), wxBORDER_SIMPLE);
    swatch->SetBackgroundColour(colour.IsOk() ? colour : *wxLIGHT_GREY);
    return swatch;
}

wxString upper(const std::string &s) { return wxString::FromUTF8(s).Upper(); }

std::string get_string(const json &status, const char *object, const char *field, const std::string &fallback)
{
    if (status.is_object() && status.contains(object) && status.at(object).is_object()) {
        const json &obj = status.at(object);
        if (obj.contains(field) && obj.at(field).is_string())
            return obj.at(field).get<std::string>();
    }
    return fallback;
}

} // namespace

MoonrakerSendDialog::MoonrakerSendDialog(wxWindow *parent, DynamicPrintConfig *printer_config, const std::string &printer_name, const wxString &default_job_name)
    : DPIDialog(parent, wxID_ANY, _L("Send print job"), wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX)
    , m_host(PrintHost::get_print_host(printer_config))
    , m_alive(std::make_shared<bool>(true))
{
    m_moonraker = dynamic_cast<Moonraker *>(m_host.get());
    SetBackgroundColour(*wxWHITE);

    if (auto *nozzles = printer_config->option<ConfigOptionFloats>("nozzle_diameter"))
        m_tool_count = nozzles->values.size();

    auto *sizer = new wxBoxSizer(wxVERTICAL);
    build_header(sizer);
    build_printer(sizer, printer_name);
    build_tools(sizer);
    build_buttons(sizer);
    SetSizerAndFit(sizer);

    m_job_name->SetValue(default_job_name);
    wxGetApp().UpdateDlgDarkUI(this);
    update_warnings();
    refresh_printer();
    CentreOnParent();
}

MoonrakerSendDialog::~MoonrakerSendDialog() { *m_alive = false; }

void MoonrakerSendDialog::build_header(wxBoxSizer *sizer)
{
    Plater    *plater = wxGetApp().plater();
    PartPlate *plate  = plater->get_partplate_list().get_curr_plate();

    auto *row = new wxBoxSizer(wxHORIZONTAL);

    // Plate preview, flipped and scaled as in SelectMachineDialog::set_default_normal.
    wxBitmap bitmap(FromDIP(150), FromDIP(150));
    if (plate && plate->thumbnail_data.is_valid()) {
        const ThumbnailData &data = plate->thumbnail_data;
        wxImage              image(data.width, data.height);
        image.InitAlpha();
        for (unsigned int r = 0; r < data.height; ++r) {
            const unsigned int rr = (data.height - 1 - r) * data.width;
            for (unsigned int c = 0; c < data.width; ++c) {
                const unsigned char *px = data.pixels.data() + 4 * (rr + c);
                image.SetRGB((int) c, (int) r, px[0], px[1], px[2]);
                image.SetAlpha((int) c, (int) r, px[3]);
            }
        }
        bitmap = wxBitmap(image.Rescale(FromDIP(150), FromDIP(150), wxIMAGE_QUALITY_HIGH));
    }
    m_thumbnail = new wxStaticBitmap(this, wxID_ANY, bitmap);
    row->Add(m_thumbnail, 0, wxALL, FromDIP(16));

    auto *info = new wxBoxSizer(wxVERTICAL);
    auto *name_label = new Label(this, _L("Job name"));
    name_label->SetFont(Label::Body_12);
    name_label->SetForegroundColour(COLOUR_MUTED);
    m_job_name = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(300), -1));
#ifdef __APPLE__
    m_job_name->OSXDisableAllSmartSubstitutions();
#endif
    info->Add(name_label, 0, wxBOTTOM, FromDIP(4));
    info->Add(m_job_name, 0, wxEXPAND | wxBOTTOM, FromDIP(12));

    wxString time = "-", weight = "-";
    if (plate && plate->get_slice_result() && !plate->get_slice_result()->print_statistics.modes.empty())
        time = wxString::FromUTF8(short_time(get_time_dhms(plate->get_slice_result()->print_statistics.modes[0].time)));
    const double grams = plater->get_partplate_list().get_current_fff_print().print_statistics().total_weight;
    if (grams > 0)
        weight = wxGetApp().app_config->get("use_inches") == "1" ? wxString::Format("%.2f oz", grams * 0.035274) : wxString::Format("%.2f g", grams);

    auto *stats = new wxBoxSizer(wxHORIZONTAL);
    for (auto [icon, text] : {std::pair<const char *, wxString>{"print-time", time}, {"print-weight", weight}}) {
        stats->Add(new wxStaticBitmap(this, wxID_ANY, create_scaled_bitmap(icon, this, 18)), 0, wxALIGN_CENTER_VERTICAL);
        auto *label = new Label(this, text);
        label->SetFont(Label::Body_13);
        stats->Add(label, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, FromDIP(6));
        stats->AddSpacer(FromDIP(18));
    }
    info->Add(stats, 0);

    row->Add(info, 1, wxTOP | wxRIGHT | wxBOTTOM | wxALIGN_CENTER_VERTICAL, FromDIP(16));
    sizer->Add(row, 0, wxEXPAND);
}

void MoonrakerSendDialog::build_printer(wxBoxSizer *sizer, const std::string &printer_name)
{
    auto *title = new Label(this, _L("Printer"));
    title->SetFont(Label::Head_14);
    sizer->Add(title, 0, wxLEFT | wxRIGHT, FromDIP(16));

    auto *row  = new wxBoxSizer(wxHORIZONTAL);
    auto *name = new Label(this, wxString::FromUTF8(printer_name) + "  ·  " + wxString::FromUTF8(m_host ? m_host->get_host() : std::string()));
    name->SetFont(Label::Body_13);
    m_printer_state = new Label(this, _L("Connecting..."));
    m_printer_state->SetFont(Label::Head_13);
    m_printer_state->SetForegroundColour(COLOUR_MUTED);

    m_btn_refresh = new Button(this, _L("Refresh"));
    m_btn_refresh->SetMinSize(wxSize(-1, FromDIP(24)));
    m_btn_refresh->SetCornerRadius(FromDIP(12));
    m_btn_refresh->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { refresh_printer(); });

    row->Add(name, 0, wxALIGN_CENTER_VERTICAL);
    row->AddSpacer(FromDIP(12));
    row->Add(m_printer_state, 1, wxALIGN_CENTER_VERTICAL);
    row->Add(m_btn_refresh, 0, wxALIGN_CENTER_VERTICAL);
    sizer->Add(row, 0, wxEXPAND | wxALL, FromDIP(16));
}

void MoonrakerSendDialog::build_tools(wxBoxSizer *sizer)
{
    auto *title = new Label(this, _L("Filament and toolhead"));
    title->SetFont(Label::Head_14);
    sizer->Add(title, 0, wxLEFT | wxRIGHT, FromDIP(16));

    auto *note = new Label(this, _L("Each filament prints on the toolhead with the same number. The right column is what the printer reports in that toolhead."));
    note->SetFont(Label::Body_12);
    note->SetForegroundColour(COLOUR_MUTED);
    note->Wrap(FromDIP(500));
    sizer->Add(note, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(16));

    PresetBundle *bundle = wxGetApp().preset_bundle;
    PartPlate    *plate  = wxGetApp().plater()->get_partplate_list().get_curr_plate();
    std::vector<int> used = plate ? plate->get_extruders(true) : std::vector<int>();
    std::sort(used.begin(), used.end());
    used.erase(std::unique(used.begin(), used.end()), used.end());

    auto *grid = new wxFlexGridSizer(0, 6, FromDIP(10), FromDIP(10));
    grid->AddGrowableCol(5);
    for (int id : used) {
        FilamentRow row;
        row.filament = id - 1;
        row.colour   = wxColour(wxString::FromUTF8(bundle->project_config.opt_string("filament_colour", (unsigned int) row.filament)));
        if (row.filament < (int) bundle->filament_presets.size())
            if (const Preset *preset = bundle->filaments.find_preset(bundle->filament_presets[row.filament]))
                row.type = wxString::FromUTF8(preset->config.opt_string("filament_type", 0u));

        grid->Add(make_swatch(this, row.colour, FromDIP(20)), 0, wxALIGN_CENTER_VERTICAL);
        auto *project = new Label(this, wxString::Format("%d  %s", id, row.type));
        project->SetFont(Label::Body_13);
        grid->Add(project, 0, wxALIGN_CENTER_VERTICAL);
        grid->Add(new Label(this, wxString::FromUTF8("→")), 0, wxALIGN_CENTER_VERTICAL);

        auto *tool = new Label(this, wxString::Format(_L("Toolhead %d"), id));
        tool->SetFont(Label::Head_13);
        grid->Add(tool, 0, wxALIGN_CENTER_VERTICAL);

        auto *printer = new wxBoxSizer(wxHORIZONTAL);
        row.printer_swatch = make_swatch(this, wxNullColour, FromDIP(20));
        row.printer_label  = new Label(this, wxString::FromUTF8("—"));
        row.printer_label->SetFont(Label::Body_13);
        printer->Add(row.printer_swatch, 0, wxALIGN_CENTER_VERTICAL);
        printer->Add(row.printer_label, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(6));
        grid->Add(printer, 0, wxALIGN_CENTER_VERTICAL);

        row.state_label = new Label(this, wxEmptyString);
        row.state_label->SetFont(Label::Body_12);
        grid->Add(row.state_label, 0, wxALIGN_CENTER_VERTICAL);
        if (row.filament >= (int) m_tool_count) {
            row.state_label->SetLabel(_L("No such toolhead"));
            row.state_label->SetForegroundColour(COLOUR_ERROR);
        }
        m_rows.push_back(row);
    }
    sizer->Add(grid, 0, wxEXPAND | wxALL, FromDIP(16));

    m_warning = new Label(this, wxEmptyString);
    m_warning->SetFont(Label::Body_13);
    sizer->Add(m_warning, 0, wxLEFT | wxRIGHT, FromDIP(16));
}

void MoonrakerSendDialog::build_buttons(wxBoxSizer *sizer)
{
    StateColor bg_green(std::pair<wxColour, int>(wxColour(206, 206, 206), StateColor::Disabled), std::pair<wxColour, int>(wxColour(27, 136, 68), StateColor::Pressed),
                        std::pair<wxColour, int>(wxColour(61, 203, 115), StateColor::Hovered), std::pair<wxColour, int>(COLOUR_OK, StateColor::Normal));
    StateColor bg_white(std::pair<wxColour, int>(wxColour(206, 206, 206), StateColor::Pressed), std::pair<wxColour, int>(wxColour(238, 238, 238), StateColor::Hovered),
                        std::pair<wxColour, int>(*wxWHITE, StateColor::Normal));

    m_btn_upload = new Button(this, _L("Upload"));
    m_btn_upload->SetBackgroundColor(bg_white);
    m_btn_upload->SetBorderColor(wxColour(38, 46, 48));
    m_btn_upload->SetToolTip(_L("Store the job on the printer without starting it. Start it from the printer's screen."));
    m_btn_upload->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { finish(PrintHostPostUploadAction::None); });

    m_btn_print = new Button(this, _L("Print"));
    m_btn_print->SetBackgroundColor(bg_green);
    m_btn_print->SetBorderColor(COLOUR_OK);
    m_btn_print->SetTextColor(wxColour("#FFFFFE"));
    m_btn_print->SetToolTip(_L("Upload the job and start printing right away."));
    m_btn_print->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { finish(PrintHostPostUploadAction::StartPrint); });

    for (Button *btn : {m_btn_upload, m_btn_print}) {
        btn->SetMinSize(wxSize(FromDIP(110), FromDIP(32)));
        btn->SetCornerRadius(FromDIP(16));
    }

    auto *row = new wxBoxSizer(wxHORIZONTAL);
    row->AddStretchSpacer();
    row->Add(m_btn_upload, 0, wxRIGHT, FromDIP(10));
    row->Add(m_btn_print, 0);
    sizer->Add(row, 0, wxEXPAND | wxALL, FromDIP(16));
}

void MoonrakerSendDialog::refresh_printer()
{
    if (!m_moonraker) {
        apply_printer_status({}, _L("This printer does not use Moonraker."));
        return;
    }
    m_printer_state->SetLabel(_L("Connecting..."));
    m_printer_state->SetForegroundColour(COLOUR_MUTED);
    m_btn_refresh->Enable(false);

    // The host object outlives the request thread through this shared_ptr; m_alive tells the reply
    // whether the dialog is still open.
    std::shared_ptr<PrintHost> host  = m_host;
    std::shared_ptr<bool>      alive = m_alive;
    std::thread([this, host, alive]() {
        wxString    error;
        std::string reply = static_cast<Moonraker *>(host.get())->query_objects(QUERIED_OBJECTS, error);
        wxGetApp().CallAfter([this, alive, reply, error]() {
            if (*alive)
                apply_printer_status(reply, error);
        });
    }).detach();
}

void MoonrakerSendDialog::apply_printer_status(const std::string &reply, const wxString &error)
{
    m_btn_refresh->Enable(true);
    m_printer_reachable = false;
    m_printer_busy      = false;
    m_mismatch          = false;

    json status;
    try {
        if (!reply.empty())
            status = json::parse(reply).at("result").at("status");
    } catch (const std::exception &) {
        status = json();
    }

    if (status.is_null()) {
        m_printer_state->SetLabel(error.empty() ? _L("Offline") : _L("Offline") + ": " + error);
        m_printer_state->SetForegroundColour(COLOUR_ERROR);
    } else {
        m_printer_reachable  = true;
        const std::string klippy = get_string(status, "webhooks", "state", "ready");
        const std::string job    = get_string(status, "print_stats", "state", "");
        if (klippy != "ready") {
            m_printer_state->SetLabel(_L("Not ready") + ": " + wxString::FromUTF8(get_string(status, "webhooks", "state_message", klippy)));
            m_printer_state->SetForegroundColour(COLOUR_ERROR);
            m_printer_busy = true;
        } else if (job == "printing" || job == "paused") {
            m_printer_state->SetLabel(job == "printing" ? _L("Printing") : _L("Paused"));
            m_printer_state->SetForegroundColour(COLOUR_WARN);
            m_printer_busy = true;
        } else {
            m_printer_state->SetLabel(_L("Ready"));
            m_printer_state->SetForegroundColour(COLOUR_OK);
        }
    }

    // Snapmaker's per-toolhead filament; other Klipper printers do not report it.
    const json tasks = status.is_object() ? status.value("print_task_config", json::object()) : json::object();
    auto at = [&tasks](const char *key, size_t i) -> json {
        const json &values = tasks.contains(key) ? tasks.at(key) : json();
        return values.is_array() && i < values.size() ? values.at(i) : json();
    };
    for (FilamentRow &row : m_rows) {
        if (row.filament >= (int) m_tool_count)
            continue;
        const size_t tool = (size_t) row.filament;
        if (tasks.empty()) {
            row.printer_label->SetLabel(m_printer_reachable ? _L("Not reported") : wxString::FromUTF8("—"));
            row.state_label->SetLabel(wxEmptyString);
            continue;
        }
        const json exists = at("filament_exist", tool);
        const json type   = at("filament_type", tool);
        const json sub    = at("filament_sub_type", tool);
        const json rgba   = at("filament_color_rgba", tool);
        if (exists.is_boolean() && !exists.get<bool>()) {
            row.printer_label->SetLabel(_L("Empty"));
            row.printer_swatch->SetBackgroundColour(*wxLIGHT_GREY);
            row.state_label->SetLabel(_L("No filament loaded"));
            row.state_label->SetForegroundColour(COLOUR_ERROR);
            m_mismatch = true;
        } else {
            const std::string type_s = type.is_string() ? type.get<std::string>() : std::string();
            const std::string sub_s  = sub.is_string() ? sub.get<std::string>() : std::string();
            row.printer_label->SetLabel(wxString::FromUTF8(sub_s.empty() ? type_s : type_s + " " + sub_s));
            if (rgba.is_string() && rgba.get<std::string>().size() >= 6)
                row.printer_swatch->SetBackgroundColour(wxColour("#" + wxString::FromUTF8(rgba.get<std::string>().substr(0, 6))));
            if (!type_s.empty() && !row.type.empty() && upper(type_s) != row.type.Upper()) {
                row.state_label->SetLabel(_L("Different filament type"));
                row.state_label->SetForegroundColour(COLOUR_WARN);
                m_mismatch = true;
            } else {
                row.state_label->SetLabel(wxString::FromUTF8("✓ ") + _L("Matches"));
                row.state_label->SetForegroundColour(COLOUR_OK);
            }
        }
        row.printer_swatch->Refresh();
    }
    update_warnings();
}

void MoonrakerSendDialog::update_warnings()
{
    const bool missing_tool = std::any_of(m_rows.begin(), m_rows.end(), [this](const FilamentRow &r) { return r.filament >= (int) m_tool_count; });

    wxString text;
    wxColour colour = COLOUR_WARN;
    if (missing_tool) {
        text   = wxString::Format(_L("This printer has %d toolheads. Assign every object to filament 1 to %d before sending."), (int) m_tool_count, (int) m_tool_count);
        colour = COLOUR_ERROR;
    } else if (m_rows.empty()) {
        text = _L("Slice the plate before sending it.");
    } else if (m_printer_busy) {
        text = _L("The printer is busy. Upload the job now and start it when the printer is free.");
    } else if (m_mismatch) {
        text = _L("Some toolheads hold a different filament than the job expects. Check them before printing.");
    } else if (!m_printer_reachable) {
        text = _L("The printer cannot be reached right now. Uploading will be retried in the upload queue.");
    }
    m_warning->SetLabel(text);
    m_warning->SetForegroundColour(colour);
    m_warning->Wrap(FromDIP(500));

    m_btn_upload->Enable(!missing_tool && !m_rows.empty());
    m_btn_print->Enable(!missing_tool && !m_rows.empty() && m_printer_reachable && !m_printer_busy);
    Layout();
    Fit();
}

void MoonrakerSendDialog::finish(PrintHostPostUploadAction action)
{
    wxString name = m_job_name->GetValue().Trim().Trim(false);
    if (name.empty()) {
        m_job_name->SetFocus();
        return;
    }
    m_job_name->SetValue(name);
    m_post_action = action;
    EndModal(wxID_OK);
}

wxString MoonrakerSendDialog::filename() const
{
    wxString name = m_job_name->GetValue().Trim().Trim(false);
    if (!name.Lower().EndsWith(".gcode"))
        name += ".gcode";
    return name;
}

void MoonrakerSendDialog::on_dpi_changed(const wxRect &suggested_rect)
{
    Layout();
    Fit();
    Refresh();
}

} // namespace GUI
} // namespace Slic3r
