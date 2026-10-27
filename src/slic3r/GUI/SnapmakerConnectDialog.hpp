#ifndef slic3r_GUI_SnapmakerConnectDialog_hpp_
#define slic3r_GUI_SnapmakerConnectDialog_hpp_

#include <atomic>
#include <memory>

#include <wx/timer.h>

#include "GUI_Utils.hpp"

class wxStaticText;
class wxButton;
class wxWebView;
class wxWebViewEvent;

namespace Slic3r { namespace GUI {

// Snapmaker account, cloud devices and remote printing, shown from Snapmaker's own connection
// helper (see Utils/SnapmakerConnect.hpp). Sign-in happens inside the helper's pages; this dialog
// only displays them and the account the helper reports.
class SnapmakerConnectDialog : public DPIDialog
{
public:
    explicit SnapmakerConnectDialog(wxWindow *parent);
    ~SnapmakerConnectDialog() override;

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override;

private:
    void start_helper();
    void on_helper_ready(bool ok, const wxString &message);
    void show_page(const std::string &page_key);
    void choose_helper();
    void refresh_account();
    void on_new_window(wxWebViewEvent &evt);

    wxStaticText *m_status{nullptr};
    wxStaticText *m_account{nullptr};
    wxButton     *m_btn_home{nullptr};
    wxButton     *m_btn_devices{nullptr};
    wxButton     *m_btn_helper{nullptr};
    wxWebView    *m_browser{nullptr};
    wxTimer       m_account_timer;
    // Shared with worker threads so they can tell whether the dialog still exists.
    std::shared_ptr<std::atomic<bool>> m_alive;
};

}} // namespace Slic3r::GUI

#endif
