# FTPS source provenance

OpenSSL 3.5.9, source SHA256 603f5602e2eef00d77fbd429d34dcd5822bb301757a1bc9cdb24c670f1eb859a
https://github.com/openssl/openssl/releases/download/openssl-3.5.9/openssl-3.5.9.tar.gz
Checksum independently matched the publisher's .sha256 file.

curl 8.23.0, source SHA256 e3de77cfa6367083c776a26647fc9f633b538c06c3baa73d931a252ac8f72b49
https://curl.se/download/curl-8.23.0.tar.gz

Built from source for iOS arm64 and iOS Simulator arm64, deployment target 17.0.
Use Scripts/build_network.sh with NETWORK_BUILD_ROOT containing the extracted verified sources.
Copy install-<sdk>/lib/{libcurl,libssl,libcrypto}.a to Vendor/FTPS/<sdk>/.
Copy curl headers to NativeFTPS/curl. Preserve bundled curl/OpenSSL licenses.
The static libraries use OpenSSL certificate verification and keep data-channel TLS enabled.

Tests/FTPSFixture/run.py builds the exact wrapper (only test port 19990 changes)
against macOS system curl and tests encrypted upload, rejection, wrong identity and CA.
It contacts localhost only. The iOS static libraries are compiled separately by Xcode.
