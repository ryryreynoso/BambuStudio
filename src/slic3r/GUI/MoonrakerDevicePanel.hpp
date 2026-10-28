#ifndef slic3r_MoonrakerDevicePanel_hpp_
#define slic3r_MoonrakerDevicePanel_hpp_

#include <memory>
#include <string>
#include <vector>

#include <wx/datetime.h>
#include <wx/image.h>
#include <wx/panel.h>
#include <wx/timer.h>

class wxBoxSizer;
class Button;
class Label;
class ProgressBar;

namespace Slic3r {

class DynamicPrintConfig;
class PrintHost;

namespace GUI {

// Device tab for Moonraker printers (Snapmaker U1), in the place of the printer's own web page: the
// camera picture, the print job, temperatures, what each toolhead holds and the printer's errors,
// laid out like Snapmaker Orca's device page in this app's colours. It only reads the printer:
// printer/objects/query and the camera picture Moonraker serves, polled while the tab is visible.
class MoonrakerDevicePanel : public wxPanel
{
public:
    explicit MoonrakerDevicePanel(wxWindow *parent);
    ~MoonrakerDevicePanel() override;

    // Points the page at a printer preset; does nothing when name and host are unchanged.
    void set_printer(const std::string &name, const DynamicPrintConfig &config);
    void msw_rescale();

private:
    struct ToolRow
    {
        wxWindow *swatch{nullptr};
        Label    *name{nullptr};
        Label    *filament{nullptr};
        Label    *temperature{nullptr};
        Label    *active{nullptr};
    };

    wxPanel *make_card(wxWindow *parent, const wxString &title, wxBoxSizer *&body);
    void     build();
    void     poll();
    void     apply_status(const std::string &reply, const wxString &error);
    void     apply_camera(const std::string &jpeg);
    void     show_offline(const wxString &error);

    std::shared_ptr<PrintHost> m_host;
    std::string                m_host_address;
    std::shared_ptr<bool>      m_alive;
    std::shared_ptr<bool>      m_status_busy;
    std::shared_ptr<bool>      m_camera_busy;
    wxTimer                    m_timer;
    std::string                m_web_url;

    Label       *m_name{nullptr};
    Label       *m_address{nullptr};
    Label       *m_state{nullptr};
    Button      *m_btn_refresh{nullptr};
    Button      *m_btn_web{nullptr};

    wxPanel     *m_camera{nullptr};
    Label       *m_camera_note{nullptr};
    wxImage      m_camera_image;
    wxDateTime   m_camera_time;

    Label       *m_job_file{nullptr};
    Label       *m_job_state{nullptr};
    ProgressBar *m_job_progress{nullptr};
    Label       *m_job_percent{nullptr};
    Label       *m_job_layer{nullptr};
    Label       *m_job_elapsed{nullptr};
    Label       *m_job_remaining{nullptr};

    Label       *m_bed_temp{nullptr};
    Label       *m_chamber_temp{nullptr};

    std::vector<ToolRow> m_tools;
    wxBoxSizer  *m_errors_body{nullptr};
    wxPanel     *m_errors_card{nullptr};
    std::vector<wxWindow *> m_error_labels;
};

} // namespace GUI
} // namespace Slic3r

#endif
