#include <catch2/catch.hpp>

#include <cstdlib>
#include <fstream>
#include <boost/filesystem.hpp>

#include "libslic3r/PrintConfig.hpp"
#include "slic3r/Utils/PrintHost.hpp"

using namespace Slic3r;

// Runs against scripts/snapmaker/mock_moonraker.py; set MOONRAKER_TEST_HOST (e.g. 127.0.0.1:7125)
// to enable. Without it the test only checks that the host type resolves to the Moonraker host.
TEST_CASE("Moonraker print host", "[Moonraker]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_key_value("host_type", new ConfigOptionEnum<PrintHostType>(htMoonraker));
    const char *host = std::getenv("MOONRAKER_TEST_HOST");
    config.set_key_value("print_host", new ConfigOptionString(host ? host : "127.0.0.1:1"));

    std::unique_ptr<PrintHost> print_host(PrintHost::get_print_host(&config));
    REQUIRE(print_host);
    CHECK(std::string(print_host->get_name()) == "Moonraker");
    CHECK(print_host->get_post_upload_actions() == PrintHostPostUploadActions(PrintHostPostUploadAction::StartPrint));

    if (host == nullptr)
        return;

    wxString msg;
    REQUIRE(print_host->test(msg));

    const boost::filesystem::path gcode = boost::filesystem::temp_directory_path() / "moonraker_host_test.gcode";
    {
        std::ofstream out(gcode.string());
        out << "; moonraker host test\nG28\n";
    }
    for (PrintHostPostUploadAction action : {PrintHostPostUploadAction::None, PrintHostPostUploadAction::StartPrint}) {
        PrintHostUpload upload;
        upload.source_path = gcode;
        upload.upload_path = boost::filesystem::path("bambu") / "test.gcode";
        upload.post_action = action;
        std::string error;
        bool ok = print_host->upload(upload, [](Http::Progress, bool &) {}, [&](wxString e) { error = e.ToStdString(); });
        INFO(error);
        CHECK(ok);
    }
    boost::filesystem::remove(gcode);
}
