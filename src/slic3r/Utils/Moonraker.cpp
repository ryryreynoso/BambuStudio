#include "Moonraker.hpp"

#include <sstream>
#include <boost/format.hpp>
#include <boost/log/trivial.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>

#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "Http.hpp"
#include "libslic3r/PrintConfig.hpp"

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
    const bool start_print        = upload_data.post_action == PrintHostPostUploadAction::StartPrint;
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

    return res;
}

} // namespace Slic3r
