#include "MoonrakerSendDialog.hpp"

#include <algorithm>
#include <thread>

#include <wx/dcmemory.h>
#include <wx/sizer.h>
#include <wx/statbmp.h>
#include <wx/textctrl.h>

#include <nlohmann/json.hpp>

#include "GUI_App.hpp"
#include "I18N.hpp"
#include "PartPlate.hpp"
#include "Plater.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/CheckBox.hpp"
#include "Widgets/ComboBox.hpp"
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

// Square colour chip for the toolhead list.
wxBitmap swatch_bitmap(const wxColour &colour, int size)
{
    wxBitmap   bitmap(size, size);
    wxMemoryDC dc(bitmap);
    dc.SetBackground(*wxWHITE_BRUSH);
    dc.Clear();
    dc.SetPen(wxPen(wxColour(172, 172, 172)));
    dc.SetBrush(wxBrush(colour.IsOk() ? colour : wxColour(238, 238, 238)));
    dc.DrawRectangle(0, 0, size, size);
    dc.SelectObject(wxNullBitmap);
    return bitmap;
}

int colour_distance(const wxColour &a, const wxColour &b)
{
    if (!a.IsOk() || !b.IsOk())
        return 3 * 255 * 255;
    const int dr = a.Red() - b.Red(), dg = a.Green() - b.Green(), db = a.Blue() - b.Blue();
    return dr * dr + dg * dg + db * db;
}

// Print options remembered between jobs (app config key, default).
const std::pair<const char *, const char *> OPT_BED_LEVEL{"moonraker_bed_level", "1"};
const std::pair<const char *, const char *> OPT_FLOW_CALIBRATE{"moonraker_flow_calibrate", "0"};
const std::pair<const char *, const char *> OPT_TIMELAPSE{"moonraker_timelapse", "0"};

bool get_option(const std::pair<const char *, const char *> &opt)
{
    const std::string value = wxGetApp().app_config->get(opt.first);
    return (value.empty() ? std::string(opt.second) : value) == "1";
}

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
    build_options(sizer);
    build_buttons(sizer);
    SetSizerAndFit(sizer);

    m_job_name->SetValue(default_job_name);
    wxGetApp().UpdateDlgDarkUI(this);
    fill_tool_choices();
    auto_map();
    update_rows();
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

    auto *note = new Label(this, _L("Pick the toolhead that prints each filament. The list shows what the printer has loaded in each toolhead; the closest match is picked for you."));
    note->SetFont(Label::Body_12);
    note->SetForegroundColour(COLOUR_MUTED);
    note->Wrap(FromDIP(520));
    sizer->Add(note, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(16));

    PresetBundle *bundle = wxGetApp().preset_bundle;
    PartPlate    *plate  = wxGetApp().plater()->get_partplate_list().get_curr_plate();
    std::vector<int> used = plate ? plate->get_extruders(true) : std::vector<int>();
    std::sort(used.begin(), used.end());
    used.erase(std::unique(used.begin(), used.end()), used.end());

    m_tools.assign(m_tool_count, ToolInfo());

    auto *grid = new wxFlexGridSizer(0, 5, FromDIP(10), FromDIP(10));
    grid->AddGrowableCol(4);
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

        row.tool_choice = new ComboBox(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(230), -1), 0, nullptr, wxCB_READONLY);
        row.tool_choice->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent &) {
            m_user_mapped = true;
            update_rows();
            update_warnings();
        });
        grid->Add(row.tool_choice, 0, wxALIGN_CENTER_VERTICAL);

        row.state_label = new Label(this, wxEmptyString);
        row.state_label->SetFont(Label::Body_12);
        grid->Add(row.state_label, 0, wxALIGN_CENTER_VERTICAL);
        m_rows.push_back(row);
    }
    sizer->Add(grid, 0, wxEXPAND | wxALL, FromDIP(16));

    m_warning = new Label(this, wxEmptyString);
    m_warning->SetFont(Label::Body_13);
    sizer->Add(m_warning, 0, wxLEFT | wxRIGHT, FromDIP(16));
}

void MoonrakerSendDialog::build_options(wxBoxSizer *sizer)
{
    m_options = new wxPanel(this);
    m_options->SetBackgroundColour(GetBackgroundColour());
    auto *box = new wxBoxSizer(wxVERTICAL);

    auto *title = new Label(m_options, _L("Print options"));
    title->SetFont(Label::Head_14);
    box->Add(title, 0, wxBOTTOM, FromDIP(8));

    auto *row = new wxBoxSizer(wxHORIZONTAL);
    auto add = [this, row](CheckBox *&check, const std::pair<const char *, const char *> &opt, const wxString &text, const wxString &tip) {
        check = new CheckBox(m_options);
        check->SetValue(get_option(opt));
        check->SetToolTip(tip);
        auto *label = new Label(m_options, text);
        label->SetFont(Label::Body_13);
        label->SetToolTip(tip);
        label->Bind(wxEVT_LEFT_DOWN, [check](wxMouseEvent &) { check->SetValue(!check->GetValue()); });
        row->Add(check, 0, wxALIGN_CENTER_VERTICAL);
        row->Add(label, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, FromDIP(6));
        row->AddSpacer(FromDIP(18));
    };
    add(m_opt_bed_level, OPT_BED_LEVEL, _L("Bed leveling"), _L("Probe the bed before printing."));
    add(m_opt_flow_calibrate, OPT_FLOW_CALIBRATE, _L("Flow calibration"), _L("Calibrate extrusion flow on the toolheads in use before printing."));
    add(m_opt_timelapse, OPT_TIMELAPSE, _L("Timelapse"), _L("Record a timelapse with the printer's camera."));
    box->Add(row, 0);

    m_options->SetSizer(box);
    sizer->Add(m_options, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(16));
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
    m_is_snapmaker = !tasks.empty();
    for (size_t i = 0; i < m_tools.size(); ++i) {
        ToolInfo info;
        if (m_is_snapmaker) {
            const json exists = at("filament_exist", i);
            const json type   = at("filament_type", i);
            const json sub    = at("filament_sub_type", i);
            const json rgba   = at("filament_color_rgba", i);
            info.reported = true;
            info.loaded   = !(exists.is_boolean() && !exists.get<bool>());
            info.type     = wxString::FromUTF8(type.is_string() ? type.get<std::string>() : std::string());
            const wxString sub_s = wxString::FromUTF8(sub.is_string() ? sub.get<std::string>() : std::string());
            info.label = sub_s.empty() ? info.type : info.type + " " + sub_s;
            if (rgba.is_string() && rgba.get<std::string>().size() >= 6)
                info.colour = wxColour("#" + wxString::FromUTF8(rgba.get<std::string>().substr(0, 6)));
        }
        m_tools[i] = info;
    }
    if (m_options)
        m_options->Show(m_is_snapmaker || !m_printer_reachable);
    fill_tool_choices();
    if (!m_user_mapped)
        auto_map();
    update_rows();
    update_warnings();
}

void MoonrakerSendDialog::fill_tool_choices()
{
    for (FilamentRow &row : m_rows) {
        const int selection = row.tool_choice->GetSelection();
        row.tool_choice->Clear();
        for (size_t i = 0; i < m_tools.size(); ++i) {
            const ToolInfo &tool = m_tools[i];
            wxString        text = wxString::Format(_L("Toolhead %d"), (int) i + 1);
            if (tool.reported)
                text += wxString::FromUTF8("  ·  ") + (tool.loaded ? tool.label : _L("Empty"));
            row.tool_choice->Append(text, swatch_bitmap(tool.reported && tool.loaded ? tool.colour : wxNullColour, FromDIP(16)));
        }
        if (selection >= 0 && selection < (int) m_tools.size())
            row.tool_choice->SetSelection(selection);
        // Only Snapmaker printers take a toolhead map; elsewhere filament k prints on tool k.
        row.tool_choice->Enable(m_is_snapmaker);
    }
}

// Bambu-style automatic mapping: each filament goes to a loaded toolhead with the same filament
// type and the closest colour, preferring toolheads no other filament took; without a match it
// keeps its own number.
void MoonrakerSendDialog::auto_map()
{
    std::vector<bool> taken(m_tools.size(), false);
    for (FilamentRow &row : m_rows) {
        int best = -1, best_score = 0;
        if (m_is_snapmaker)
            for (size_t i = 0; i < m_tools.size(); ++i) {
                const ToolInfo &tool = m_tools[i];
                if (!tool.loaded || tool.type.empty() || tool.type.Upper() != row.type.Upper())
                    continue;
                const int score = colour_distance(row.colour, tool.colour) + (taken[i] ? 4 * 255 * 255 : 0);
                if (best < 0 || score < best_score) {
                    best       = (int) i;
                    best_score = score;
                }
            }
        if (best < 0)
            best = row.filament < (int) m_tools.size() ? row.filament : 0;
        if (best < (int) m_tools.size()) {
            taken[best] = true;
            row.tool_choice->SetSelection(best);
        }
    }
}

void MoonrakerSendDialog::update_rows()
{
    m_mismatch    = false;
    m_shared_tool = false;
    std::vector<int> users(m_tools.size(), 0);
    for (const FilamentRow &row : m_rows) {
        const int tool = row.tool_choice->GetSelection();
        if (tool >= 0 && tool < (int) users.size())
            ++users[tool];
    }
    for (FilamentRow &row : m_rows) {
        const int tool = row.tool_choice->GetSelection();
        if (!m_is_snapmaker && row.filament >= (int) m_tool_count) {
            row.state_label->SetLabel(_L("No such toolhead"));
            row.state_label->SetForegroundColour(COLOUR_ERROR);
            continue;
        }
        if (tool < 0 || tool >= (int) m_tools.size() || !m_tools[tool].reported) {
            row.state_label->SetLabel(wxEmptyString);
            continue;
        }
        const ToolInfo &info = m_tools[tool];
        if (!info.loaded) {
            row.state_label->SetLabel(_L("No filament loaded"));
            row.state_label->SetForegroundColour(COLOUR_ERROR);
            m_mismatch = true;
        } else if (!info.type.empty() && !row.type.empty() && info.type.Upper() != row.type.Upper()) {
            row.state_label->SetLabel(_L("Different filament type"));
            row.state_label->SetForegroundColour(COLOUR_WARN);
            m_mismatch = true;
        } else if (users[tool] > 1) {
            row.state_label->SetLabel(_L("Shares a toolhead"));
            row.state_label->SetForegroundColour(COLOUR_WARN);
            m_shared_tool = true;
        } else {
            row.state_label->SetLabel(wxString::FromUTF8("✓ ") + _L("Matches"));
            row.state_label->SetForegroundColour(COLOUR_OK);
        }
    }
}

void MoonrakerSendDialog::update_warnings()
{
    // Without a toolhead map (non-Snapmaker printers) filament k needs tool k.
    const bool missing_tool = !m_is_snapmaker && m_printer_reachable &&
                              std::any_of(m_rows.begin(), m_rows.end(), [this](const FilamentRow &r) { return r.filament >= (int) m_tool_count; });

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
        text = _L("Some toolheads hold a different filament than the job expects. Pick another toolhead or check them before printing.");
    } else if (m_shared_tool) {
        text = _L("More than one filament prints with the same toolhead, so those parts come out in one colour.");
    } else if (!m_printer_reachable) {
        text = _L("The printer cannot be reached right now. Uploading will be retried in the upload queue.");
    }
    m_warning->SetLabel(text);
    m_warning->SetForegroundColour(colour);
    m_warning->Wrap(FromDIP(500));

    m_btn_upload->Enable(!m_rows.empty());
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
    if (m_opt_bed_level) {
        wxGetApp().app_config->set(OPT_BED_LEVEL.first, m_opt_bed_level->GetValue() ? "1" : "0");
        wxGetApp().app_config->set(OPT_FLOW_CALIBRATE.first, m_opt_flow_calibrate->GetValue() ? "1" : "0");
        wxGetApp().app_config->set(OPT_TIMELAPSE.first, m_opt_timelapse->GetValue() ? "1" : "0");
    }
    EndModal(wxID_OK);
}

std::string MoonrakerSendDialog::start_options() const
{
    if (!m_is_snapmaker)
        return {};
    // Same shape Snapmaker Orca sends: map_table pairs are [G-code tool, toolhead], both 0-based.
    json map_table = json::array();
    for (const FilamentRow &row : m_rows) {
        const int tool = row.tool_choice->GetSelection();
        if (tool >= 0)
            map_table.push_back({row.filament, tool});
    }
    json options = {{"bed_level", m_opt_bed_level->GetValue() ? 1 : 0},
                    {"flow_calibrate", m_opt_flow_calibrate->GetValue() ? 1 : 0},
                    {"map_table", map_table},
                    {"time_lapse_camera", m_opt_timelapse->GetValue() ? 1 : 0}};
    return options.dump();
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
