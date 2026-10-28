#include "Moonraker.hpp"

#include <chrono>
#include <sstream>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/format.hpp>
#include <boost/log/trivial.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>

#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "Http.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <nlohmann/json.hpp>

namespace pt = boost::property_tree;

namespace Slic3r {

Moonraker::Moonraker(DynamicPrintConfig *config) :
    m_host(config->opt_string("print_host")),
    m_apikey(config->opt_string("printhost_apikey")),
    m_cafile(config->opt_string("printhost_cafile")),
    m_ssl_revoke_best_effort(config->opt_bool("printhost_ssl_ignore_revoke"))
{}

const char* Moonraker::get_name() const { return "Moonraker"; }

void Moonraker::set_auth(Http &http) const
{
    if (!m_apikey.empty())
        http.header("X-Api-Key", m_apikey);
    if (!m_cafile.empty())
        http.ca_file(m_cafile);
}

std::vector<std::string> Moonraker::base_urls() const
{
    std::string host = m_host;
    while (!host.empty() && host.back() == '/')
        host.pop_back();
    std::string scheme = "http://";
    for (const char *prefix : {"http://", "https://"})
        if (host.rfind(prefix, 0) == 0) {
            scheme = prefix;
            host   = host.substr(scheme.size());
        }
    std::vector<std::string> urls{scheme + host + "/"};
    // A port is present when the host part (before any path) contains ':' outside an IPv6 literal.
    const std::string authority = host.substr(0, host.find('/'));
    const size_t      colon     = authority.rfind(':');
    const bool        has_port  = colon != std::string::npos && authority.find(']', colon) == std::string::npos;
    if (!has_port && !authority.empty())
        urls.push_back(scheme + authority + ":8080" + host.substr(authority.size()) + "/");
    return urls;
}

std::string Moonraker::find_base_url(wxString &msg) const
{
    const char *name = get_name();
    for (const std::string &base : base_urls()) {
        bool ok  = false;
        auto url = base + "printer/info";
        BOOST_LOG_TRIVIAL(info) << boost::format("%1%: Get printer info at: %2%") % name % url;
        auto http = Http::get(std::move(url));
        set_auth(http);
        http.on_error([&](std::string body, std::string error, unsigned status) {
                BOOST_LOG_TRIVIAL(error) << boost::format("%1%: Error getting printer info: %2%, HTTP %3%, body: `%4%`") % name % error % status % body;
                msg = format_error(body, error, status);
            })
            .on_complete([&](std::string body, unsigned) {
                BOOST_LOG_TRIVIAL(debug) << boost::format("%1%: Got printer info: %2%") % name % body;
                try {
                    std::stringstream ss(body);
                    pt::ptree         ptree;
                    pt::read_json(ss, ptree);
                    // Moonraker wraps every reply in {"result": {...}}; printer/info reports Klippy's state.
                    if (ptree.get_child_optional("result")) {
                        ok = true;
                    } else {
                        msg = _L("The host answered, but not like Moonraker.");
                    }
                } catch (const std::exception &) {
                    msg = _L("Could not parse server response.");
                }
            })
#ifdef WIN32
            .ssl_revoke_best_effort(m_ssl_revoke_best_effort)
#endif
            .perform_sync();
        if (ok)
            return base;
    }
    return {};
}

bool Moonraker::test(wxString &msg) const
{
    return !find_base_url(msg).empty();
}

std::string Moonraker::query_objects(const std::vector<std::string> &objects, wxString &msg) const
{
    const std::string base = find_base_url(msg);
    if (base.empty())
        return {};
    std::string url = base + "printer/objects/query";
    for (size_t i = 0; i < objects.size(); ++i)
        url += (i == 0 ? "?" : "&") + Http::url_encode(objects[i]);

    std::string status;
    auto        http = Http::get(std::move(url));
    set_auth(http);
    http.on_error([&](std::string body, std::string error, unsigned status_code) {
            msg = format_error(body, error, status_code);
        })
        .on_complete([&](std::string body, unsigned) { status = std::move(body); })
#ifdef WIN32
        .ssl_revoke_best_effort(m_ssl_revoke_best_effort)
#endif
        .perform_sync();
    return status;
}

bool Moonraker::get_file(const std::string &path, std::string &body, wxString &msg) const
{
    const std::string base = find_base_url(msg);
    if (base.empty())
        return false;
    bool ok   = false;
    auto http = Http::get(base + "server/files/" + path);
    set_auth(http);
    http.on_error([&](std::string reply, std::string error, unsigned status) { msg = format_error(reply, error, status); })
        .on_complete([&](std::string reply, unsigned) {
            body = std::move(reply);
            ok   = true;
        })
#ifdef WIN32
        .ssl_revoke_best_effort(m_ssl_revoke_best_effort)
#endif
        .perform_sync();
    return ok;
}

bool Moonraker::websocket_call(const std::string &method, const std::string &params, int timeout_s, wxString &msg) const
{
    namespace beast     = boost::beast;
    namespace websocket = boost::beast::websocket;
    using tcp           = boost::asio::ip::tcp;
    using json          = nlohmann::json;

    const std::string base = find_base_url(msg);
    if (base.empty())
        return false;
    if (base.rfind("http://", 0) != 0) {
        msg = _L("This needs an http:// printer address.");
        return false;
    }
    // base is http://authority/prefix/ ; Moonraker's websocket is prefix + "websocket".
    const std::string rest      = base.substr(7);
    const size_t      slash     = rest.find('/');
    const std::string authority = rest.substr(0, slash);
    const std::string target    = (slash == std::string::npos ? std::string("/") : rest.substr(slash)) + "websocket";
    std::string       host = authority, port = "80";
    const size_t      colon = authority.rfind(':');
    if (colon != std::string::npos && authority.find(']', colon) == std::string::npos) {
        host = authority.substr(0, colon);
        port = authority.substr(colon + 1);
    }
    if (host.size() > 1 && host.front() == '[' && host.back() == ']')
        host = host.substr(1, host.size() - 2);

    const int request_id = 3170;
    json      request;
    try {
        request = {{"jsonrpc", "2.0"}, {"method", method}, {"params", json::parse(params)}, {"id", request_id}};
    } catch (const std::exception &) {
        msg = _L("The request could not be built.");
        return false;
    }
    BOOST_LOG_TRIVIAL(info) << "Moonraker: " << method << " " << request.dump();

    try {
        boost::asio::io_context ioc;
        tcp::resolver           resolver(ioc);
        websocket::stream<beast::tcp_stream> ws(ioc);
        beast::get_lowest_layer(ws).connect(resolver.resolve(host, port));

        // Blocking reads on this socket give up after the timeout instead of hanging the caller.
#ifdef _WIN32
        DWORD timeout_ms = timeout_s * 1000;
        ::setsockopt(beast::get_lowest_layer(ws).socket().native_handle(), SOL_SOCKET, SO_RCVTIMEO, (const char *) &timeout_ms, sizeof(timeout_ms));
#else
        struct timeval timeout{timeout_s, 0};
        ::setsockopt(beast::get_lowest_layer(ws).socket().native_handle(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#endif
        const std::string apikey = m_apikey;
        ws.set_option(websocket::stream_base::decorator([apikey](websocket::request_type &req) {
            if (!apikey.empty())
                req.set("X-Api-Key", apikey);
        }));
        ws.handshake(authority, target);
        ws.text(true);
        ws.write(boost::asio::buffer(request.dump()));

        // Moonraker also pushes status notifications on this socket; wait for the reply to our id.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_s);
        while (std::chrono::steady_clock::now() < deadline) {
            beast::flat_buffer buffer;
            ws.read(buffer);
            json reply = json::parse(beast::buffers_to_string(buffer.data()), nullptr, false);
            if (!reply.is_object() || !reply.contains("id") || reply["id"] != request_id)
                continue;
            BOOST_LOG_TRIVIAL(info) << "Moonraker: " << method << " reply " << reply.dump();
            boost::system::error_code ignored;
            ws.close(websocket::close_code::normal, ignored);
            if (reply.contains("error")) {
                const json &error = reply["error"];
                msg = wxString::FromUTF8(error.is_object() ? error.value("message", error.dump()) : error.dump());
                return false;
            }
            return true;
        }
        msg = _L("The printer did not answer in time.");
    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(error) << "Moonraker: " << method << " failed: " << e.what();
        msg = wxString::FromUTF8(e.what());
    }
    return false;
}

bool Moonraker::start_local_print(const std::string &path, const std::string &options, wxString &msg) const
{
    using json = nlohmann::json;
    std::string params;
    try {
        params = json{{"type", "gcode"}, {"path", path}, {"print_plate", 1}, {"options", json::parse(options)}, {"extra_info", {{"print_task_id", ""}}}}.dump();
    } catch (const std::exception &) {
        msg = _L("The print options could not be read.");
        return false;
    }
    return websocket_call("server.files.start_local_print", params, 30, msg);
}

bool Moonraker::camera_monitor(bool on, wxString &msg) const
{
    // As Snapmaker Orca asks it: with the monitor on, the U1 rewrites camera/monitor.jpg about twice
    // a second (interval 0); without it the picture only changes while printing. The U1 acts on these
    // without answering on Moonraker's websocket, so the wait for a reply is kept short.
    return on ? websocket_call("camera.start_monitor", R"({"domain":"lan","interval":0,"expect_pw":true})", 2, msg)
              : websocket_call("camera.stop_monitor", R"({"domain":"lan"})", 2, msg);
}

wxString Moonraker::get_test_ok_msg() const
{
    return _L("Connection to Moonraker works correctly.");
}

wxString Moonraker::get_test_failed_msg(wxString &msg) const
{
    return GUI::from_u8((boost::format("%s: %s") % _utf8(L("Could not connect to Moonraker")) % std::string(msg.ToUTF8())).str());
}

bool Moonraker::upload(PrintHostUpload upload_data, ProgressFn prorgess_fn, ErrorFn error_fn) const
{
    const char *name = get_name();

    wxString msg;
    const std::string base = find_base_url(msg);
    if (base.empty()) {
        error_fn(std::move(msg));
        return false;
    }

    const auto upload_filename    = upload_data.upload_path.filename();
    const auto upload_parent_path = upload_data.upload_path.parent_path();
    // With Snapmaker start options the file is only uploaded here and started below with them.
    const bool start_with_options = upload_data.post_action == PrintHostPostUploadAction::StartPrint && !upload_data.start_options.empty();
    const bool start_print        = upload_data.post_action == PrintHostPostUploadAction::StartPrint && !start_with_options;
    std::string url               = base + "server/files/upload";

    BOOST_LOG_TRIVIAL(info) << boost::format("%1%: Uploading file %2% at %3%, filename: %4%, path: %5%, print: %6%") % name % upload_data.source_path % url %
                                   upload_filename.string() % upload_parent_path.string() % (start_print ? "true" : "false");

    bool res  = true;
    auto http = Http::post(std::move(url));
    set_auth(http);
    http.form_add("root", "gcodes")
        .form_add("path", upload_parent_path.generic_string())
        .form_add("print", start_print ? "true" : "false")
        .form_add_file("file", upload_data.source_path.string(), upload_filename.string())
        .on_complete([&](std::string body, unsigned status) {
            BOOST_LOG_TRIVIAL(debug) << boost::format("%1%: File uploaded: HTTP %2%: %3%") % name % status % body;
        })
        .on_error([&](std::string body, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(error) << boost::format("%1%: Error uploading file: %2%, HTTP %3%, body: `%4%`") % name % error % status % body;
            error_fn(format_error(body, error, status));
            res = false;
        })
        .on_progress([&](Http::Progress progress, bool &cancel) {
            prorgess_fn(std::move(progress), cancel);
            if (cancel) {
                BOOST_LOG_TRIVIAL(info) << "Moonraker: Upload canceled";
                res = false;
            }
        })
#ifdef WIN32
        .ssl_revoke_best_effort(m_ssl_revoke_best_effort)
#endif
        .perform_sync();

    if (res && start_with_options) {
        wxString start_msg;
        if (!start_local_print(upload_data.upload_path.generic_string(), upload_data.start_options, start_msg)) {
            error_fn(_L("The job was uploaded but did not start") + ": " + start_msg);
            res = false;
        }
    }
    return res;
}

} // namespace Slic3r
