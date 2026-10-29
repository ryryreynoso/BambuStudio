#ifndef slic3r_Moonraker_hpp_
#define slic3r_Moonraker_hpp_

#include <mutex>
#include <string>
#include <vector>
#include <wx/string.h>

#include "PrintHost.hpp"

namespace Slic3r {

class DynamicPrintConfig;
class Http;

// Klipper printers through Moonraker's own HTTP API (Snapmaker U1 and others): the connection test
// reads printer/info and the upload posts to server/files/upload, optionally starting the print.
// Adapted from Snapmaker Orca's Moonraker host (AGPL-3.0).
class Moonraker : public PrintHost
{
public:
    explicit Moonraker(DynamicPrintConfig *config);
    ~Moonraker() override = default;

    const char* get_name() const override;

    bool test(wxString &curl_msg) const override;
    wxString get_test_ok_msg() const override;
    wxString get_test_failed_msg(wxString &msg) const override;
    bool upload(PrintHostUpload upload_data, ProgressFn prorgess_fn, ErrorFn error_fn) const override;
    bool has_auto_discovery() const override { return false; }
    bool can_test() const override { return true; }
    PrintHostPostUploadActions get_post_upload_actions() const override { return PrintHostPostUploadAction::StartPrint; }
    std::string get_host() const override { return m_host; }

    // Reads printer/objects/query for the given Klipper objects (GET only). Returns Moonraker's
    // reply body ({"result": {"status": {...}}}), or an empty string with msg set. Blocks; call it
    // off the UI thread.
    std::string query_objects(const std::vector<std::string> &objects, wxString &msg) const;
    // Downloads a file Moonraker serves under server/files (for example "camera/monitor.jpg", the
    // Snapmaker U1's camera picture). Blocks; call it off the UI thread.
    bool get_file(const std::string &path, std::string &body, wxString &msg) const;
    // GET of any Moonraker endpoint (for example "server/files/metadata?filename=..."). Returns the
    // reply body, or an empty string with msg set. Blocks.
    std::string get(const std::string &endpoint, wxString &msg) const;
    // POST without a body (for example "printer/print/pause"). Blocks.
    bool post(const std::string &endpoint, wxString &msg) const;
    // Snapmaker's print start (server.files.start_local_print over Moonraker's websocket), as Snapmaker
    // Orca sends it: the uploaded gcode path relative to the gcodes root and the options JSON
    // (map_table, bed_level, flow_calibrate, time_lapse_camera). Blocks; call it off the UI thread.
    bool start_local_print(const std::string &path, const std::string &options, wxString &msg) const;
    // Turns the Snapmaker U1's camera monitor on or off (camera.start_monitor / stop_monitor over
    // Moonraker's websocket). While it is on, camera/monitor.jpg is close to live. Blocks.
    // Returns false only when the request failed; confirmed is set when the U1 pushed
    // notify_camera_status_change with the new state (it pushes nothing when the state is unchanged).
    bool camera_monitor(bool on, wxString &msg, bool *confirmed = nullptr) const;

private:
    // Waits until print_stats shows path printing, so a start the printer ignored is reported.
    bool wait_for_print(const std::string &path, wxString &msg) const;
    // Sends one JSON-RPC request over Moonraker's websocket and waits for its reply. A notification
    // named ack_method also counts as the reply (its params go to ack_params); when one is given,
    // no reply before the timeout is not an error. Blocks.
    bool websocket_call(const std::string &method, const std::string &params, int timeout_s, wxString &msg,
                        const std::string &ack_method = {}, std::string *ack_params = nullptr) const;

    std::string m_host;
    std::string m_apikey;
    std::string m_cafile;
    bool        m_ssl_revoke_best_effort;

    void set_auth(Http &http) const;
    // Base URLs to try, in order: the host as entered, then port 8080 when no port was entered
    // (Snapmaker printers serve the API there as well).
    std::vector<std::string> base_urls() const;
    // Returns the first base URL that answers printer/info, or an empty string with msg set.
    // The answer is remembered until a request to it fails.
    std::string find_base_url(wxString &msg) const;
    void        forget_base_url() const;
    mutable std::mutex  m_base_mutex;
    mutable std::string m_base_url;
};

} // namespace Slic3r

#endif
