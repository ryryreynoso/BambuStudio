#include "SnapmakerConnectDialog.hpp"

#include <thread>

#include <wx/button.h>
#include <wx/filedlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/utils.h>
#include <wx/webview.h>

#include "GUI_App.hpp"
#include "I18N.hpp"
#include "Widgets/WebView.hpp"
#include "libslic3r/AppConfig.hpp"
#include "slic3r/Utils/SnapmakerConnect.hpp"

namespace Slic3r { namespace GUI {

static const char *HELPER_PATH_KEY = "snapmaker_helper_path";

SnapmakerConnectDialog::SnapmakerConnectDialog(wxWindow *parent)
    : DPIDialog(parent, wxID_ANY, _L("Snapmaker account and devices"), wxDefaultPosition, wxDefaultSize,
                wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , m_account_timer(this)
    , m_alive(std::make_shared<std::atomic<bool>>(true))
{
    SetBackgroundColour(*wxWHITE);
    auto *sizer   = new wxBoxSizer(wxVERTICAL);
    auto *toolbar = new wxBoxSizer(wxHORIZONTAL);

    m_btn_home    = new wxButton(this, wxID_ANY, _L("Account"));
    m_btn_devices = new wxButton(this, wxID_ANY, _L("Devices"));
    m_btn_helper  = new wxButton(this, wxID_ANY, _L("Helper location..."));
    m_account     = new wxStaticText(this, wxID_ANY, wxEmptyString);
    toolbar->Add(m_btn_home, 0, wxALL, FromDIP(4));
    toolbar->Add(m_btn_devices, 0, wxALL, FromDIP(4));
    toolbar->AddStretchSpacer(1);
    toolbar->Add(m_account, 0, wxALIGN_CENTER_VERTICAL | wxALL, FromDIP(4));
    toolbar->Add(m_btn_helper, 0, wxALL, FromDIP(4));
    sizer->Add(toolbar, 0, wxEXPAND);

    m_status = new wxStaticText(this, wxID_ANY, wxEmptyString);
    sizer->Add(m_status, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(8));

    m_browser = WebView::CreateWebView(this, wxEmptyString, "snapmaker_connect");
    if (m_browser) {
        m_browser->Hide();
        sizer->Add(m_browser, 1, wxEXPAND | wxTOP, FromDIP(4));
        Bind(wxEVT_WEBVIEW_NEWWINDOW, &SnapmakerConnectDialog::on_new_window, this, m_browser->GetId());
    }

    SetSizer(sizer);
    SetSize(FromDIP(wxSize(1000, 720)));
    CenterOnParent();

    m_btn_home->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { show_page("home_page"); });
    m_btn_devices->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { show_page("device_control"); });
    m_btn_helper->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { choose_helper(); });
    Bind(wxEVT_TIMER, [this](wxTimerEvent &) { refresh_account(); }, m_account_timer.GetId());

    m_btn_home->Disable();
    m_btn_devices->Disable();
    start_helper();
}

SnapmakerConnectDialog::~SnapmakerConnectDialog()
{
    m_alive->store(false);
    m_account_timer.Stop();
}

void SnapmakerConnectDialog::on_dpi_changed(const wxRect &suggested_rect)
{
    Layout();
    Refresh();
}

void SnapmakerConnectDialog::start_helper()
{
    const std::string configured = wxGetApp().app_config->get(HELPER_PATH_KEY);
    const auto        helper     = SnapmakerConnect::find_helper(configured);
    if (helper.empty()) {
        on_helper_ready(false, _L("Snapmaker's connection helper was not found. It is installed with Snapmaker Orca; "
                                  "install Snapmaker Orca or use \"Helper location...\" to point to it."));
        return;
    }
    m_status->SetLabel(_L("Starting Snapmaker's connection helper..."));
    const std::string locale = SnapmakerConnect::helper_locale(wxGetApp().current_language_code_safe().ToStdString());
    std::weak_ptr<std::atomic<bool>> alive = m_alive;
    std::thread([this, alive, helper, locale]() {
        std::string error;
        const bool  ok = SnapmakerConnect::instance().start(helper, locale, error);
        wxGetApp().CallAfter([this, alive, ok, error]() {
            auto flag = alive.lock();
            if (flag && flag->load())
                on_helper_ready(ok, wxString::FromUTF8(error));
        });
    }).detach();
}

void SnapmakerConnectDialog::on_helper_ready(bool ok, const wxString &message)
{
    if (!ok) {
        m_status->SetLabel(message);
        m_status->Wrap(GetClientSize().GetWidth() - FromDIP(16));
        Layout();
        return;
    }
    m_status->SetLabel(wxEmptyString);
    m_btn_home->Enable();
    m_btn_devices->Enable(!SnapmakerConnect::instance().page_url("device_control").empty());
    show_page("home_page");
    refresh_account();
    m_account_timer.Start(3000);
    Layout();
}

void SnapmakerConnectDialog::show_page(const std::string &page_key)
{
    const std::string url = SnapmakerConnect::instance().page_url(page_key);
    if (url.empty() || m_browser == nullptr) {
        m_status->SetLabel(_L("Snapmaker's connection helper does not provide this page."));
        return;
    }
    m_browser->Show();
    WebView::LoadUrl(m_browser, wxString::FromUTF8(url));
    Layout();
}

void SnapmakerConnectDialog::choose_helper()
{
    wxFileDialog dlg(this, _L("Choose Snapmaker's connection helper (snapmaker_control)"), wxEmptyString, wxEmptyString,
                     wxFileSelectorDefaultWildcardStr, wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dlg.ShowModal() != wxID_OK)
        return;
    wxGetApp().app_config->set(HELPER_PATH_KEY, dlg.GetPath().ToUTF8().data());
    SnapmakerConnect::instance().stop();
    m_account_timer.Stop();
    m_btn_home->Disable();
    m_btn_devices->Disable();
    start_helper();
}

void SnapmakerConnectDialog::refresh_account()
{
    std::weak_ptr<std::atomic<bool>> alive = m_alive;
    std::thread([this, alive]() {
        Gateway::AccountSnapshot account;
        std::string              error;
        const bool               ok = SnapmakerConnect::instance().fetch_account(account, error);
        wxGetApp().CallAfter([this, alive, ok, account]() {
            auto flag = alive.lock();
            if (!flag || !flag->load())
                return;
            wxString label;
            if (ok && account.is_login) {
                const std::string &name = !account.nickname.empty() ? account.nickname : !account.email.empty() ? account.email : account.account;
                label = wxString::Format(_L("Signed in to Snapmaker as %s"), wxString::FromUTF8(name));
            } else if (ok) {
                label = _L("Not signed in to Snapmaker");
            }
            m_account->SetLabel(label);
            Layout();
        });
    }).detach();
}

void SnapmakerConnectDialog::on_new_window(wxWebViewEvent &evt)
{
    // Links that open a new window (help pages, the Snapmaker website) go to the system browser.
    wxLaunchDefaultBrowser(evt.GetURL());
}

}} // namespace Slic3r::GUI
