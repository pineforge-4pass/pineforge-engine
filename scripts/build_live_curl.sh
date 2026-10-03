#!/usr/bin/env bash
set -euo pipefail

readonly CURL_VERSION="8.14.1"
readonly CURL_SHA256="f4619a1e2474c4bbfedc88a7c2191209c8334b48fa1f4e53fd584cc12e9120dd"

if [[ "${1:-}" == "--metadata" ]]; then
  printf 'version=%s\nsha256=%s\n' "$CURL_VERSION" "$CURL_SHA256"
  exit 0
fi

dependency_dir="${1:-build-native-deps}"
build_jobs="${2:-4}"
mkdir -p "$dependency_dir"
cd "$dependency_dir"
cmake_file="curl-install/lib/cmake/CURL/CURLConfig.cmake"
if [[ "${CURL_CACHE_HIT:-false}" == "true" && -f "$cmake_file" ]]; then
  echo "Using cached libcurl at ${cmake_file}"
  exit 0
fi

curl --fail --location --proto '=https' --tlsv1.2 \
  "https://curl.se/download/curl-${CURL_VERSION}.tar.xz" -o curl.tar.xz
echo "${CURL_SHA256}  curl.tar.xz" | sha256sum -c -
tar -xf curl.tar.xz
cmake -S "curl-${CURL_VERSION}" -B curl-build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PWD/curl-install" \
  -DBUILD_CURL_EXE=OFF -DBUILD_SHARED_LIBS=OFF -DBUILD_STATIC_LIBS=ON \
  -DBUILD_TESTING=OFF -DCURL_USE_OPENSSL=ON -DENABLE_WEBSOCKETS=ON \
  -DHTTP_ONLY=ON -DCURL_USE_LIBPSL=OFF -DUSE_LIBIDN2=OFF \
  -DCURL_USE_LIBSSH2=OFF -DCURL_BROTLI=OFF -DCURL_ZSTD=OFF \
  2>&1 | tee curl-configure.log
cmake --build curl-build -j "$build_jobs" 2>&1 | tee curl-build.log
cmake --install curl-build 2>&1 | tee curl-install.log
