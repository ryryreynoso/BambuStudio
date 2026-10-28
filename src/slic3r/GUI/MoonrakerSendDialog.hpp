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
class Label;

namespace Slic3r {

class DynamicPrintConfig;
class Moonraker;

namespace GUI {

// "Send print job" for Moonraker printers (Snapmaker U1 and other Klipper toolchangers), laid out
// like Bambu's SelectMachineDialog: plate preview, job name, time and weight, the printer and its
// state, and which tool prints each filament together with the filament the printer reports in
// that tool (Snapmaker's print_task_config). Filament k always prints on tool k on these printers.
// The printer is only read here; the job is sent by the caller with the chosen post-upload action.
class MoonrakerSendDialog : public DPIDialog
{
public:
    MoonrakerSendDialog(wxWindow *parent, DynamicPrintConfig *printer_config, const std::string &printer_name, const wxString &default_job_name);
    ~MoonrakerSendDialog() override;

    // Job name with the .gcode suffix, as it will be stored on the printer.
    wxString                  filename() const;
    PrintHostPostUploadAction post_action() const { return m_post_action; }

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override;

private:
    struct FilamentRow
    {
        int            filament; // 0-based project filament index; also the tool that prints it
        wxString       type;
        wxColour       colour;
        wxWindow      *printer_swatch{nullptr};
        Label         *printer_label{nullptr};
        Label         *state_label{nullptr};
    };

    void build_header(wxBoxSizer *sizer);
    void build_printer(wxBoxSizer *sizer, const std::string &printer_name);
    void build_tools(wxBoxSizer *sizer);
    void build_buttons(wxBoxSizer *sizer);

    void refresh_printer();
    void apply_printer_status(const std::string &reply, const wxString &error);
    void update_warnings();
    void finish(PrintHostPostUploadAction action);

    std::shared_ptr<PrintHost> m_host;
    Moonraker                 *m_moonraker{nullptr};
    std::shared_ptr<bool>      m_alive;
    std::vector<FilamentRow>   m_rows;
    size_t                     m_tool_count{0};
    bool                       m_printer_busy{false};
    bool                       m_printer_reachable{false};
    bool                       m_mismatch{false};

    wxStaticBitmap  *m_thumbnail{nullptr};
    wxTextCtrl      *m_job_name{nullptr};
    Label           *m_printer_state{nullptr};
    Label           *m_warning{nullptr};
    Button          *m_btn_refresh{nullptr};
    Button          *m_btn_upload{nullptr};
    Button          *m_btn_print{nullptr};

    PrintHostPostUploadAction m_post_action{PrintHostPostUploadAction::None};
};

} // namespace GUI
} // namespace Slic3r

#endif
