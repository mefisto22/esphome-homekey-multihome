#!/usr/bin/env bash
# Host tests for the multi-Home HomeKey store.
#
#   tests/homekey_host/run_tests.sh            # both suites
#   tests/homekey_host/run_tests.sh store      # store unit tests only (no crypto deps)
#
# Suite 1 (test_homekey_store.cpp): store logic with fakes for the library.
# Suite 2 (test_real_library.cpp): the real (vendored) HK-HomeKit-Lib
#   provisioning and FAST/STANDARD NFC flows driven through the store against a
#   protocol model of a phone.
# Suite 3 (test_hostile_nfc.cpp): malformed / malicious NFC input must never
#   crash or hang the library (each case runs in a forked child).
#
# Needs g++/gcc (C++20), cmake and git. The HomeKey library is the vendored
# copy in components/homekit/HK-HomeKit-Lib; other dependencies are fetched
# into tests/homekey_host/.cache unless provided:
#   MBEDTLS_SRC   Mbed TLS 3.6 source tree (e.g. ESP-IDF components/mbedtls/mbedtls)
#   TINYCBOR_SRC  tinycbor source tree
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
CACHE="$HERE/.cache"
mkdir -p "$CACHE"
SUITES="${1:-all}"

HK_LIB_DIR="$ROOT/components/homekit/HK-HomeKit-Lib"

# -fno-exceptions: same as the ESP-IDF firmware build; a JSON type error in the
# store's decoders would abort() here just like it would on the ESP32.
CXXFLAGS=(-std=gnu++20 -O1 -g -fno-exceptions -fsanitize=address,undefined -fno-sanitize-recover=undefined)

echo "=== Suite 1: HomeKey store (fake library) ==="
g++ "${CXXFLAGS[@]}" -Wall -Wextra -Wno-unused-parameter \
  -I"$HERE/stubs" -I"$HERE" -I"$ROOT/components/homekit" -I"$HK_LIB_DIR/include" \
  "$HERE/test_homekey_store.cpp" "$HERE/fake_nvs.cpp" "$ROOT/components/homekit/homekey_store.cpp" \
  -o "$CACHE/test_homekey_store"
"$CACHE/test_homekey_store"

[ "$SUITES" = "store" ] && exit 0

echo
echo "=== Suite 2: real (vendored) HK-HomeKit-Lib through the store ==="
if [ -z "${MBEDTLS_SRC:-}" ]; then
  MBEDTLS_SRC="$CACHE/mbedtls"
  [ -d "$MBEDTLS_SRC" ] || git clone -q --depth 1 -b v3.6.5 --recurse-submodules --shallow-submodules \
    https://github.com/Mbed-TLS/mbedtls.git "$MBEDTLS_SRC"
fi
if [ ! -f "$CACHE/mbedtls-build/library/libmbedcrypto.a" ]; then
  cmake -S "$MBEDTLS_SRC" -B "$CACHE/mbedtls-build" -DENABLE_TESTING=OFF -DENABLE_PROGRAMS=OFF \
    -DCMAKE_BUILD_TYPE=Release >/dev/null
  cmake --build "$CACHE/mbedtls-build" -j"$(nproc)" >/dev/null
fi
if [ -z "${TINYCBOR_SRC:-}" ]; then
  TINYCBOR_SRC="$CACHE/tinycbor"
  [ -d "$TINYCBOR_SRC" ] || git clone -q https://github.com/intel/tinycbor.git "$TINYCBOR_SRC"
  git -C "$TINYCBOR_SRC" checkout -q c0aad2fb2137a31b9845fbaae3653540c410f215  # espressif/cbor 0.6.1~4
fi
mkdir -p "$CACHE/cbor-obj"
for f in cborencoder cborencoder_close_container_checked cborerrorstrings cborparser cborparser_dup_string; do
  gcc -c -O1 -I"$TINYCBOR_SRC/src" "$TINYCBOR_SRC/src/$f.c" -o "$CACHE/cbor-obj/$f.o"
done
# -w: warnings of the third-party library are not ours to fix here.
g++ "${CXXFLAGS[@]}" -w \
  -I"$HERE/stubs" -I"$HERE" -I"$ROOT/components/homekit" -I"$HK_LIB_DIR/include" -I"$HK_LIB_DIR/priv" \
  -I"$MBEDTLS_SRC/include" -I"$TINYCBOR_SRC/src" \
  "$HERE/test_real_library.cpp" "$HERE/fake_nvs.cpp" \
  "$ROOT/components/homekit/homekey_store.cpp" "$ROOT/components/homekit/homekey_library.cpp" \
  "$HK_LIB_DIR/src/HK_HomeKit.cpp" "$HK_LIB_DIR"/src/auth/*.cpp "$HK_LIB_DIR"/src/crypto/*.cpp \
  "$HK_LIB_DIR"/src/utils/*.cpp "$CACHE"/cbor-obj/*.o \
  "$CACHE/mbedtls-build/library/libmbedtls.a" "$CACHE/mbedtls-build/library/libmbedx509.a" \
  "$CACHE/mbedtls-build/library/libmbedcrypto.a" \
  -o "$CACHE/test_real_library"
"$CACHE/test_real_library"

echo
echo "=== Suite 3: hostile NFC input against the (vendored) HomeKey library ==="
g++ "${CXXFLAGS[@]}" -w \
  -I"$HERE/stubs" -I"$HERE" -I"$ROOT/components/homekit" -I"$HK_LIB_DIR/include" -I"$HK_LIB_DIR/priv" \
  -I"$MBEDTLS_SRC/include" -I"$TINYCBOR_SRC/src" \
  "$HERE/test_hostile_nfc.cpp" "$HERE/fake_nvs.cpp" \
  "$ROOT/components/homekit/homekey_store.cpp" "$ROOT/components/homekit/homekey_library.cpp" \
  "$HK_LIB_DIR/src/HK_HomeKit.cpp" "$HK_LIB_DIR"/src/auth/*.cpp "$HK_LIB_DIR"/src/crypto/*.cpp \
  "$HK_LIB_DIR"/src/utils/*.cpp "$CACHE"/cbor-obj/*.o \
  "$CACHE/mbedtls-build/library/libmbedtls.a" "$CACHE/mbedtls-build/library/libmbedx509.a" \
  "$CACHE/mbedtls-build/library/libmbedcrypto.a" \
  -o "$CACHE/test_hostile_nfc"
"$CACHE/test_hostile_nfc"
