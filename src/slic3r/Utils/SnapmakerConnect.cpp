#include "SnapmakerConnect.hpp"

#include <chrono>
#include <cstdlib>
#include <thread>

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <nlohmann/json.hpp>

#include "Http.hpp"
#include "SnapmakerGateway/ConnectionProcessManager.hpp"

namespace fs = boost::filesystem;

namespace Slic3r {

namespace {

// Same limits as Snapmaker Orca's GatewayService.
constexpr auto health_timeout       = std::chrono::milliseconds(5000);
constexpr auto health_poll_interval = std::chrono::milliseconds(100);

std::string join_url(std::string base, const std::string &path)
{
    while (!base.empty() && base.back() == '/')
        base.pop_back();
    if (path.empty())
        return base;
    return path.front() == '/' ? base + path : base + "/" + path;
}

struct HttpResult
{
    bool        ok{false};
    unsigned    status{0};
    std::string body;
    std::string error;
};

HttpResult http_get(const std::string &url)
{
    HttpResult result;
    auto       http = Http::get(url);
    http.timeout_connect(2)
        .timeout_max(5)
        .on_complete([&](std::string body, unsigned status) {
            result.ok     = true;
            result.status = status;
            result.body   = std::move(body);
        })
        .on_error([&](std::string body, std::string error, unsigned status) {
            result.status = status;
            result.body   = std::move(body);
            result.error  = error.empty() ? "HTTP " + std::to_string(status) : error;
        })
        .perform_sync();
    return result;
}

} // namespace

SnapmakerConnect &SnapmakerConnect::instance()
{
    static SnapmakerConnect connect;
    return connect;
}

SnapmakerConnect::~SnapmakerConnect() { stop(); }

std::vector<fs::path> SnapmakerConnect::default_helper_paths()
{
    const std::string name(Gateway::ConnectionProcessManager::cli_executable_name());
    std::vector<fs::path> paths;
#if defined(__APPLE__)
    paths.push_back(fs::path("/Applications/Snapmaker Orca.app/Contents/Resources") / name);
    if (const char *home = std::getenv("HOME"))
        paths.push_back(fs::path(home) / "Applications/Snapmaker Orca.app/Contents/Resources" / name);
#elif defined(_WIN32)
    for (const char *var : {"ProgramW6432", "ProgramFiles"})
        if (const char *dir = std::getenv(var))
            paths.push_back(fs::path(dir) / "Snapmaker Orca" / "resources" / name);
    if (const char *local = std::getenv("LOCALAPPDATA"))
        paths.push_back(fs::path(local) / "Programs" / "Snapmaker Orca" / "resources" / name);
#else
    paths.push_back(fs::path("/opt/Snapmaker Orca/resources") / name);
    if (const char *home = std::getenv("HOME"))
        paths.push_back(fs::path(home) / ".local/share/Snapmaker Orca/resources" / name);
#endif
    return paths;
}

fs::path SnapmakerConnect::find_helper(const std::string &configured_path)
{
    boost::system::error_code ec;
    if (!configured_path.empty()) {
        const fs::path configured(configured_path);
        if (fs::is_regular_file(configured, ec))
            return configured;
        // A folder (such as Snapmaker Orca's resources folder) holding the helper is fine too.
        const fs::path inside = configured / std::string(Gateway::ConnectionProcessManager::cli_executable_name());
        if (fs::is_regular_file(inside, ec))
            return inside;
    }
    for (const fs::path &path : default_helper_paths())
        if (fs::is_regular_file(path, ec))
            return path;
    return {};
}

bool SnapmakerConnect::parse_health(const std::string &body, Health &health, std::string &error)
{
    // Mirrors Snapmaker Orca's Gateway::parse_health.
    const nlohmann::json root = nlohmann::json::parse(body, nullptr, false);
    if (!root.is_object()) {
        error = "health response is not a JSON object";
        return false;
    }
    const auto status  = root.find("status");
    const auto ok      = root.find("ok");
    const bool healthy = (status != root.end() && status->is_string() && status->get<std::string>() == "ok") ||
                         (ok != root.end() && ok->is_boolean() && ok->get<bool>());
    if (!healthy) {
        error = "health endpoint did not report ok";
        return false;
    }
    const auto components = root.find("components");
    if (components == root.end() || !components->is_object()) {
        error = "health components are missing";
        return false;
    }
    for (const char *component_name : {"ipc_server", "web_server"}) {
        const auto component = components->find(component_name);
        if (component == components->end() || !component->is_string() || component->get<std::string>() != "ok") {
            error = std::string("component is not ready: ") + component_name;
            return false;
        }
    }
    const auto server_url = root.find("server_url");
    if (server_url == root.end() || !server_url->is_object()) {
        error = "health server_url is missing";
        return false;
    }
    const auto base_url = server_url->find("base_url");
    if (base_url == server_url->end() || !base_url->is_string() || base_url->get<std::string>().empty()) {
        error = "health base_url is missing";
        return false;
    }
    Health parsed;
    parsed.base_url = base_url->get<std::string>();
    if (const auto version = root.find("cli_version"); version != root.end() && version->is_string())
        parsed.cli_version = version->get<std::string>();
    for (const auto &item : server_url->items())
        if (item.key() != "base_url" && item.value().is_string())
            parsed.pages.emplace(item.key(), item.value().get<std::string>());
    health = std::move(parsed);
    return true;
}

std::string SnapmakerConnect::helper_locale(const std::string &language_code)
{
    // wx canonical names look like "en_US" or "zh_CN"; the helper takes "en", "zh-CN", ...
    std::string code = language_code;
    for (char &c : code)
        if (c == '_')
            c = '-';
    if (code.rfind("zh", 0) == 0)
        return code.size() >= 5 ? code.substr(0, 5) : "zh-CN";
    const size_t dash = code.find('-');
    code = code.substr(0, dash);
    return code.empty() ? "en" : code;
}

bool SnapmakerConnect::start(const fs::path &helper, const std::string &locale, std::string &error)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_process && m_port != 0)
        return true;
    if (helper.empty()) {
        error = "The Snapmaker connection helper was not found. Install Snapmaker Orca, or set the helper's location.";
        return false;
    }

    Gateway::ConnectionProcessManager::Config config;
    config.executable = helper;
    auto process      = std::make_shared<Gateway::ConnectionProcessManager>(config);
    const auto discovery = process->discover_port(locale);
    if (discovery.error != Gateway::ProcessDiscoveryError::None) {
        process->terminate();
        error = "The Snapmaker connection helper did not start: " + discovery.message;
        return false;
    }

    const std::string health_url = "http://127.0.0.1:" + std::to_string(discovery.port) + "/api/health";
    const auto        deadline   = std::chrono::steady_clock::now() + health_timeout;
    std::string       health_error = "no reply";
    while (std::chrono::steady_clock::now() < deadline) {
        const HttpResult reply = http_get(health_url);
        Health           health;
        if (reply.ok && reply.status >= 200 && reply.status < 300 && parse_health(reply.body, health, health_error)) {
            m_process = std::move(process);
            m_port    = discovery.port;
            m_health  = std::move(health);
            BOOST_LOG_TRIVIAL(info) << "Snapmaker helper ready on port " << m_port << ", version " << m_health.cli_version;
            return true;
        }
        if (!reply.ok)
            health_error = reply.error;
        std::this_thread::sleep_for(health_poll_interval);
    }
    process->terminate();
    error = "The Snapmaker connection helper did not become ready: " + health_error;
    return false;
}

void SnapmakerConnect::stop()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_process)
        m_process->terminate();
    m_process.reset();
    m_port   = 0;
    m_health = Health();
}

bool SnapmakerConnect::running() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_process && m_port != 0;
}

std::string SnapmakerConnect::page_url(const std::string &page_key) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_process || m_port == 0)
        return {};
    const auto page = m_health.pages.find(page_key);
    if (page == m_health.pages.end() || page->second.empty())
        return {};
    return join_url(m_health.base_url, page->second);
}

std::string SnapmakerConnect::api_url(const std::string &path) const
{
    return "http://127.0.0.1:" + std::to_string(m_port) + path;
}

bool SnapmakerConnect::fetch_account(Gateway::AccountSnapshot &account, std::string &error) const
{
    std::string url;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_process || m_port == 0) {
            error = "The Snapmaker connection helper is not running.";
            return false;
        }
        url = api_url("/api/account");
    }
    const HttpResult reply = http_get(url);
    if (!reply.ok || reply.status < 200 || reply.status >= 300) {
        error = reply.error.empty() ? "HTTP " + std::to_string(reply.status) : reply.error;
        return false;
    }
    const nlohmann::json frame = nlohmann::json::parse(reply.body, nullptr, false);
    if (!Gateway::GatewayAccount::parse(frame, account)) {
        error = "invalid account response";
        return false;
    }
    // The token stays with the helper; nothing here needs it.
    account.token.clear();
    return true;
}

} // namespace Slic3r
