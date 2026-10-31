#!/bin/bash
set -euo pipefail
ROOT="${NETWORK_BUILD_ROOT:?Set NETWORK_BUILD_ROOT to the directory containing the verified curl-8.23.0 and openssl-3.5.9 sources}"
for SDK in iphoneos iphonesimulator; do
  PREFIX="$ROOT/install-$SDK"
  SYSPATH="$(xcrun --sdk "$SDK" --show-sdk-path)"
  if [ "$SDK" = iphoneos ]; then TARGET=arm64-apple-ios17.0; else TARGET=arm64-apple-ios17.0-simulator; fi
  if [ ! -f "$PREFIX/lib/libssl.a" ]; then
  mkdir -p "$ROOT/build-ssl-$SDK"
  cd "$ROOT/build-ssl-$SDK"
  CFLAGS="-target $TARGET -isysroot $SYSPATH" CC="$(xcrun --sdk "$SDK" --find clang)" "$ROOT/openssl-3.5.9/Configure" ios64-xcrun no-shared no-tests no-apps no-module no-asm --prefix="$PREFIX" --openssldir="$PREFIX/ssl"
  make -j8 build_libs
  make install_dev
  fi
  cmake -S "$ROOT/curl-8.23.0" -B "$ROOT/build-curl-823-$SDK" -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT="$SYSPATH" -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=17.0 -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DBUILD_CURL_EXE=OFF -DBUILD_TESTING=OFF -DCURL_USE_OPENSSL=ON -DOPENSSL_ROOT_DIR="$PREFIX" -DOPENSSL_INCLUDE_DIR="$PREFIX/include" -DOPENSSL_SSL_LIBRARY="$PREFIX/lib/libssl.a" -DOPENSSL_CRYPTO_LIBRARY="$PREFIX/lib/libcrypto.a" -DOPENSSL_USE_STATIC_LIBS=TRUE -DCURL_USE_LIBPSL=OFF -DCURL_ZLIB=OFF -DCURL_BROTLI=OFF -DCURL_ZSTD=OFF -DUSE_LIBIDN2=OFF -DCURL_USE_LIBSSH2=OFF -DCURL_DISABLE_LDAP=ON -DCURL_DISABLE_LDAPS=ON
  cmake --build "$ROOT/build-curl-823-$SDK" -j8
  cmake --install "$ROOT/build-curl-823-$SDK"
done
