// SPDX-License-Identifier: AGPL-3.0-only
#include "PrinterFTPS.h"
#include <curl/curl.h>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <sys/stat.h>

int printer_ftps_upload(const char *file, const char *host, const char *serial,
                        const char *code, const char *remote, const char *ca,
                        char *error, unsigned long capacity) {
    auto fail = [&](const char *message) { if (capacity) std::snprintf(error, capacity, "%s", message); return 1; };
    if (!file || !host || !serial || !code || !remote || !ca) return fail("Missing FTPS configuration.");
    if (std::strspn(serial, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789") != std::strlen(serial) || !*serial)
        return fail("Invalid printer serial number.");
    if (std::strspn(remote, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._") != std::strlen(remote) || !*remote)
        return fail("Invalid remote filename.");
    // A host is a DNS name or IPv4 address, never a URL, credential or curl connection expression.
    if (std::strspn(host, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._") != std::strlen(host) || !*host)
        return fail("Use a printer IPv4 address or VPN DNS name.");
    static std::once_flag initialized;
    static CURLcode init = CURLE_FAILED_INIT;
    std::call_once(initialized, [] { init = curl_global_init(CURL_GLOBAL_DEFAULT); });
    if (init != CURLE_OK) return fail("FTPS initialization failed.");
    struct stat info {};
    if (stat(file, &info) || info.st_size <= 0) return fail("The sliced file is missing or empty.");
    FILE *input = std::fopen(file, "rb");
    if (!input) return fail("The sliced file could not be opened.");
    CURL *curl = curl_easy_init();
    if (!curl) { std::fclose(input); return fail("FTPS initialization failed."); }
    std::string url = std::string("ftps://") + serial + ":990/" + remote;
    std::string connect = std::string(serial) + ":990:" + host + ":990";
    curl_slist *connections = curl_slist_append(nullptr, connect.c_str());
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_CONNECT_TO, connections);
    curl_easy_setopt(curl, CURLOPT_USERNAME, "bblp");
    curl_easy_setopt(curl, CURLOPT_PASSWORD, code);
    curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(curl, CURLOPT_READDATA, input);
    curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, static_cast<curl_off_t>(info.st_size));
    curl_easy_setopt(curl, CURLOPT_CAINFO, ca);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_USE_SSL, CURLUSESSL_ALL);
    curl_easy_setopt(curl, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);
    curl_easy_setopt(curl, CURLOPT_FTP_SKIP_PASV_IP, 1L);
    curl_easy_setopt(curl, CURLOPT_FTP_SSL_CCC, CURLFTPSSL_CCC_NONE);
    curl_easy_setopt(curl, CURLOPT_SSL_SESSIONID_CACHE, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 600L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_PROXY, "");
    // Never enable verbose logging: it may include credentials or private job information.
    CURLcode result = curl_easy_perform(curl);
    curl_slist_free_all(connections);
    curl_easy_cleanup(curl);
    std::fclose(input);
    if (result != CURLE_OK) {
        if (result == CURLE_LOGIN_DENIED) return fail("The printer rejected the LAN access code.");
        if (result == CURLE_PEER_FAILED_VERIFICATION) return fail("The printer certificate failed verification. Check its serial number and device clock.");
        return fail(curl_easy_strerror(result));
    }
    return 0;
}
