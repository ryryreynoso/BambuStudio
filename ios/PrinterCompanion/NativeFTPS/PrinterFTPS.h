#pragma once
#ifdef __cplusplus
extern "C" {
#endif
// Returns zero only after an FTPS transfer completed. Never sends a print command.
int printer_ftps_upload(const char *file, const char *host, const char *serial,
                        const char *access_code, const char *remote, const char *ca_file,
                        char *error, unsigned long error_size);
#ifdef __cplusplus
}
#endif
