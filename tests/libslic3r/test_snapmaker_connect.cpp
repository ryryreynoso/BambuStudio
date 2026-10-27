#include <catch2/catch.hpp>

#include <cstdlib>

#include "slic3r/Utils/SnapmakerConnect.hpp"

using namespace Slic3r;

TEST_CASE("Snapmaker helper locale", "[SnapmakerConnect]")
{
    CHECK(SnapmakerConnect::helper_locale("en_US") == "en");
    CHECK(SnapmakerConnect::helper_locale("de_DE") == "de");
    CHECK(SnapmakerConnect::helper_locale("zh_CN") == "zh-CN");
    CHECK(SnapmakerConnect::helper_locale("") == "en");
}

TEST_CASE("Snapmaker helper health parsing", "[SnapmakerConnect]")
{
    SnapmakerConnect::Health health;
    std::string              error;
    CHECK(SnapmakerConnect::parse_health(R"({"status":"ok","components":{"ipc_server":"ok","web_server":"ok"},)"
                                         R"("server_url":{"base_url":"http://127.0.0.1:5000","home_page":"/web/home"}})",
                                         health, error));
    CHECK(health.base_url == "http://127.0.0.1:5000");
    CHECK(health.pages.at("home_page") == "/web/home");
    CHECK_FALSE(SnapmakerConnect::parse_health(R"({"status":"ok","components":{"ipc_server":"starting","web_server":"ok"},)"
                                               R"("server_url":{"base_url":"x"}})",
                                               health, error));
    CHECK_FALSE(SnapmakerConnect::parse_health("not json", health, error));
}

// Runs scripts/snapmaker/fake_snapmaker_helper.py; set FAKE_SNAPMAKER_HELPER to its path to enable.
TEST_CASE("Snapmaker helper lifecycle", "[SnapmakerConnect]")
{
    const char *helper_path = std::getenv("FAKE_SNAPMAKER_HELPER");
    if (helper_path == nullptr)
        return;
    SnapmakerConnect &connect = SnapmakerConnect::instance();
    const auto        helper  = SnapmakerConnect::find_helper(helper_path);
    REQUIRE(!helper.empty());

    std::string error;
    REQUIRE(connect.start(helper, "en", error));
    CHECK(connect.running());
    CHECK(connect.page_url("home_page").find("/web/home?locale=en") != std::string::npos);
    CHECK(connect.page_url("device_control").find("/web/devices") != std::string::npos);
    CHECK(connect.page_url("missing").empty());

    Gateway::AccountSnapshot account;
    REQUIRE(connect.fetch_account(account, error));
    CHECK(account.is_login == (std::getenv("FAKE_SNAPMAKER_SIGNED_IN") != nullptr));
    CHECK(account.token.empty()); // the helper keeps the token

    connect.stop();
    CHECK_FALSE(connect.running());
    CHECK(connect.page_url("home_page").empty());
    CHECK_FALSE(connect.fetch_account(account, error));
}
