#!/usr/bin/env bash
# Run in Linux/WSL. Produces target archives only; never executes target programs.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
: "${PS5_PAYLOAD_SDK:?Set PS5_PAYLOAD_SDK to a Linux/WSL SDK path}"
export PS5_PAYLOAD_SDK="$(realpath "$PS5_PAYLOAD_SDK")"
prefix="${PS5_DEPS_PREFIX:-$root/build/ps5-deps}"
work="${PBSD_DEPS_WORK:-/tmp/pbs-discord-deps}"
mkdir -p "$prefix" "$work/deps-src" "$work/deps-build"
prefix="$(realpath "$prefix")"
sources="$work/deps-src"
builds="$work/deps-build"
jobs="${JOBS:-$(nproc)}"
export CC="$root/scripts/ps5-cc" CXX="$root/scripts/ps5-cxx"
export AR="$(command -v llvm-ar || command -v llvm-ar-21 || command -v llvm-ar-20 || command -v llvm-ar-19)"
export RANLIB="$(command -v llvm-ranlib || command -v llvm-ranlib-21 || command -v llvm-ranlib-20 || command -v llvm-ranlib-19)"
export CFLAGS='-O2 -fPIC' CXXFLAGS='-O2 -fPIC'
export PKG_CONFIG_LIBDIR="$prefix/lib/pkgconfig"
export PKG_CONFIG_PATH=
chmod +x "$CC" "$CXX"
checkout() {
    local name="$1" url="$2" tag="$3" commit="$4"
    if [[ ! -d "$sources/$name/.git" ]]; then git clone --depth 1 --branch "$tag" "$url" "$sources/$name"; fi
    [[ "$(git -C "$sources/$name" rev-parse HEAD)" == "$commit" ]] || {
        echo "Unexpected $name source revision; remove $sources/$name and retry" >&2; exit 1;
    }
    [[ -z "$(git -C "$sources/$name" status --porcelain --untracked-files=no)" ]] || {
        echo "Tracked source changes in $name; refusing a non-reproducible build" >&2; exit 1;
    }
}
checkout openssl https://github.com/openssl/openssl.git openssl-3.5.9 45e844fa2a14ec92d146bd8f5778ac130b6625fb
checkout opus https://github.com/xiph/opus.git v1.5.2 ddbe48383984d56acd9e1ab6a090c54ca6b735a6
checkout json https://github.com/nlohmann/json.git v3.12.0 55f93686c01528224f448c19128836e7df245f72
cat > "$prefix/SOURCE-MANIFEST.txt" <<'MANIFEST'
OpenSSL 3.5.9 https://github.com/openssl/openssl.git 45e844fa2a14ec92d146bd8f5778ac130b6625fb
Opus 1.5.2 https://github.com/xiph/opus.git ddbe48383984d56acd9e1ab6a090c54ca6b735a6
nlohmann JSON 3.12.0 https://github.com/nlohmann/json.git 55f93686c01528224f448c19128836e7df245f72
libsodium 1.0.20 https://download.libsodium.org/libsodium/releases/libsodium-1.0.20.tar.gz SHA256 ebb65ef6ca439333c2bb41a0c1990587288da07f6c7fd07cb3a18cc18d30ce19
SDK compatibility: ps5-cc defines __NO_TLS to use SDK libc's __getCurrentRuneLocale function; no upstream source patches.
MANIFEST
if [[ ! -f "$sources/libsodium-1.0.20.tar.gz" ]]; then
    curl --fail --location --retry 3 https://download.libsodium.org/libsodium/releases/libsodium-1.0.20.tar.gz -o "$sources/libsodium-1.0.20.tar.gz"
fi
echo "ebb65ef6ca439333c2bb41a0c1990587288da07f6c7fd07cb3a18cc18d30ce19  $sources/libsodium-1.0.20.tar.gz" | sha256sum --check
if [[ ! -d "$sources/libsodium-1.0.20" ]]; then tar -xzf "$sources/libsodium-1.0.20.tar.gz" -C "$sources"; fi
mkdir -p "$builds/openssl" "$builds/sodium"
(
    cd "$builds/openssl"
    # No loadable providers, assembler, or target-side helper executables required.
    "$sources/openssl/Configure" BSD-x86_64 no-shared no-tests no-apps no-module no-dso no-asm no-afalgeng \
        --prefix="$prefix" --libdir=lib
    make -j"$jobs"
    make install_sw
)
(
    cd "$builds/sodium"
    "$sources/libsodium-1.0.20/configure" --host=x86_64-unknown-freebsd --build="$("$sources/libsodium-1.0.20/build-aux/config.guess")" \
        --prefix="$prefix" --disable-shared --enable-static --disable-pie --disable-asm
    make -j"$jobs"
    make install
)
cmake -S "$sources/opus" -B "$builds/opus" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$root/cmake/ps5.cmake" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$prefix" -DPS5_DEPS_PREFIX="$prefix" \
    -DBUILD_SHARED_LIBS=OFF -DOPUS_BUILD_TESTING=OFF -DOPUS_BUILD_PROGRAMS=OFF \
    -DOPUS_INSTALL_PKG_CONFIG_MODULE=ON -DOPUS_INSTALL_CMAKE_CONFIG_MODULE=ON
cmake --build "$builds/opus" --parallel "$jobs"
cmake --install "$builds/opus"
# JSON is header-only, but install its package config so subprojects find it.
cmake -S "$sources/json" -B "$builds/json" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$root/cmake/ps5.cmake" -DCMAKE_INSTALL_PREFIX="$prefix" \
    -DJSON_BuildTests=OFF -DJSON_Install=ON
cmake --install "$builds/json"
printf 'Static PS5 dependencies installed to %s\n' "$prefix"
