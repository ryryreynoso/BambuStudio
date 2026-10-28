#ifndef slic3r_MoonrakerSendDialog_hpp_
#define slic3r_MoonrakerSendDialog_hpp_

#include <memory>
#include <string>
#include <vector>

#include <wx/string.h>

#include "GUI_Utils.hpp"
#include "slic3r/Utils/PrintHost.hpp"

class wxBoxSizer;
class wxFlexGridSizer;
class wxStaticBitmap;
class wxTextCtrl;
class Button;
class CheckBox;
class ComboBox;
class Label;

namespace Slic3r {

class DynamicPrintConfig;
class Moonraker;

namespace GUI {

// "Send print job" for Moonraker printers (Snapmaker U1 and other Klipper toolchangers), laid out
// like Bambu's SelectMachineDialog: plate preview, job name, time and weight, the printer and its
// state, and which toolhead prints each filament, picked from what the printer reports in each
// toolhead (Snapmaker's print_task_config) and matched by type and colour as Bambu matches AMS
// slots. On Snapmaker printers the choice and the print options (bed leveling, flow calibration,
// timelapse) go to the printer with the print start, as Snapmaker Orca sends them; other Klipper
// printers print filament k on tool k. The printer is only read here; the job is sent by the caller.
class MoonrakerSendDialog : public DPIDialog
{
public:
    MoonrakerSendDialog(wxWindow *parent, DynamicPrintConfig *printer_config, const std::string &printer_name, const wxString &default_job_name);
    ~MoonrakerSendDialog() override;

    // Job name with the .gcode suffix, as it will be stored on the printer.
    wxString                  filename() const;
    PrintHostPostUploadAction post_action() const { return m_post_action; }
    // Options for Snapmaker's start_local_print (map_table and print options) when printing on a
    // Snapmaker printer; empty otherwise.
    std::string               start_options() const;

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override;

private:
    struct FilamentRow
    {
        int       filament; // 0-based project filament index (G-code tool T<filament>)
        wxString  type;
        wxColour  colour;
        ComboBox *tool_choice{nullptr}; // item i is toolhead i
        Label    *state_label{nullptr};
    };

    // What the printer reports in one toolhead.
    struct ToolInfo
    {
        bool     reported{false};
        bool     loaded{true};
        wxString type;
        wxString label; // type and sub type, e.g. "PLA SnapSpeed"
        wxColour colour;
    };

    void build_header(wxBoxSizer *sizer);
    void build_printer(wxBoxSizer *sizer, const std::string &printer_name);
    void build_tools(wxBoxSizer *sizer);
    void build_options(wxBoxSizer *sizer);
    void build_buttons(wxBoxSizer *sizer);

    void refresh_printer();
    void apply_printer_status(const std::string &reply, const wxString &error);
    void fill_tool_choices();
    void auto_map();
    void update_rows();
    void update_warnings();
    void finish(PrintHostPostUploadAction action);

    std::shared_ptr<PrintHost> m_host;
    Moonraker                 *m_moonraker{nullptr};
    std::shared_ptr<bool>      m_alive;
    std::vector<FilamentRow>   m_rows;
    std::vector<ToolInfo>      m_tools;
    size_t                     m_tool_count{0};
    bool                       m_printer_busy{false};
    bool                       m_printer_reachable{false};
    bool                       m_is_snapmaker{false};
    bool                       m_user_mapped{false};
    bool                       m_mismatch{false};
    bool                       m_shared_tool{false};

    wxStaticBitmap  *m_thumbnail{nullptr};
    wxTextCtrl      *m_job_name{nullptr};
    Label           *m_printer_state{nullptr};
    Label           *m_warning{nullptr};
    wxWindow        *m_options{nullptr};
    CheckBox        *m_opt_bed_level{nullptr};
    CheckBox        *m_opt_flow_calibrate{nullptr};
    CheckBox        *m_opt_timelapse{nullptr};
    Button          *m_btn_refresh{nullptr};
    Button          *m_btn_upload{nullptr};
    Button          *m_btn_print{nullptr};

    PrintHostPostUploadAction m_post_action{PrintHostPostUploadAction::None};
};

} // namespace GUI
} // namespace Slic3r

#endif
