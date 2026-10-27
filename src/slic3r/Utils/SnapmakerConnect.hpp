#ifndef slic3r_SnapmakerConnect_hpp_
#define slic3r_SnapmakerConnect_hpp_

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <boost/filesystem/path.hpp>

#include "SnapmakerGateway/GatewayAccount.hpp"

namespace Slic3r {

namespace Gateway { class ConnectionProcessManager; }

// Snapmaker account and cloud access through Snapmaker's own connection helper
// (snapmaker_control_<platform>), the program Snapmaker Orca starts for sign-in, cloud devices and
// remote printing. Bambu Studio does not ship the helper: it starts the copy installed with
// Snapmaker Orca, or one the user points to. The helper owns the Snapmaker login and its token;
// Bambu Studio only shows the helper's own pages and reads the account state it reports, so no
// Snapmaker credentials are handled or stored here. Bambu Lab sign-in is unaffected.
class SnapmakerConnect
{
public:
    struct Health
    {
        std::string                        cli_version;
        std::string                        base_url;
        std::map<std::string, std::string> pages; // page key -> path, e.g. "home_page", "device_control"
    };

    static SnapmakerConnect &instance();

    // Where Snapmaker Orca installs the helper on this platform, most likely first.
    static std::vector<boost::filesystem::path> default_helper_paths();
    // The configured path when it exists, otherwise the first default path that exists; empty if none.
    static boost::filesystem::path find_helper(const std::string &configured_path);
    // Parses the helper's /api/health reply; returns false with error set when it is not ready.
    static bool parse_health(const std::string &body, Health &health, std::string &error);
    // The locale argument the helper accepts ("en", "zh-CN", ...).
    static std::string helper_locale(const std::string &language_code);

    // Starts the helper and waits until its web server reports healthy. Blocking (up to a few
    // seconds): call from a worker thread. Does nothing when the helper is already running.
    bool start(const boost::filesystem::path &helper, const std::string &locale, std::string &error);
    // Stops the helper started by start().
    void stop();
    bool running() const;

    // URL of one of the helper's pages, or empty when unknown or the helper is not running.
    std::string page_url(const std::string &page_key) const;
    // Reads the signed-in Snapmaker account from the helper.
    bool fetch_account(Gateway::AccountSnapshot &account, std::string &error) const;

private:
    SnapmakerConnect() = default;
    ~SnapmakerConnect();

    std::string api_url(const std::string &path) const;

    mutable std::mutex                                 m_mutex;
    std::shared_ptr<Gateway::ConnectionProcessManager> m_process;
    std::uint16_t                                      m_port{0};
    Health                                             m_health;
};

} // namespace Slic3r

#endif
