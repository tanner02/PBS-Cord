# Building PBS-Cord

Use Linux or WSL with a C++20 compiler and CMake 3.20 or newer. Run the following commands from the repository root. These CMake builds produce the Discord integration; the inherited standalone PBS build scripts are documented in [docs/UPSTREAM-BUILDING.md](docs/UPSTREAM-BUILDING.md).

Initialize the two pinned source dependencies after cloning:

```sh
git clone https://github.com/tanner02/PBS-Cord.git
cd PBS-Cord
git submodule update --init vendor/libdave vendor/mlspp
```

The unused vcpkg submodules inside those dependencies are not required.

## Host build and offline checks

On a Debian/Ubuntu environment with OpenSSL 3 and nlohmann/json 3.11 or newer available:

```sh
sudo apt-get update
sudo apt-get install build-essential clang lld llvm cmake ninja-build pkg-config   python3 git curl perl libssl-dev libsodium-dev libopus-dev nlohmann-json3-dev   ffmpeg nodejs
cmake -S . -B build-host -G Ninja -DCMAKE_BUILD_TYPE=Release   -DPBSD_BUILD_TESTS=ON
cmake --build build-host --parallel
ctest --test-dir build-host --output-on-failure
```

Node enables the dashboard tests; FFmpeg and ffprobe enable generated media round-trip checks. Host tests use local fixtures rather than a console. A passing host suite does not establish console firmware compatibility.

## PS5 payload

Install a compatible PS5 Payload SDK separately. It must contain `target/lib/main.script`, the target headers, C/C++ runtimes and native API stubs. The SDK is not bundled. Use its Linux/WSL path rather than a Windows drive-letter path.

```sh
export PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk
chmod +x scripts/build-ps5-deps.sh scripts/ps5-cc scripts/ps5-cxx
bash scripts/build-ps5-deps.sh
cmake -S . -B build-ps5 -G Ninja   -DCMAKE_TOOLCHAIN_FILE=cmake/ps5.cmake -DCMAKE_BUILD_TYPE=Release   -DPBSD_BUILD_TESTS=OFF
cmake --build build-ps5 --parallel
```

Output: `build-ps5/pbs-discord.elf`. Browser assets are embedded in this ELF. The dependency script downloads pinned source revisions and builds static target libraries without executing target programs. Defaults are `build/ps5-deps` for installed dependencies and `/tmp/pbs-discord-deps` for temporary sources/builds. If changing `PS5_DEPS_PREFIX`, export it for the script and pass the same absolute value as `-DPS5_DEPS_PREFIX=...` to CMake. `PS5_CLANG` and `PS5_CLANGXX` can select compiler paths; the defaults are `/usr/bin/clang` and `/usr/bin/clang++`.

Load the ELF using your existing PS5 payload loader. Keep the loader's installed filename/path when replacing an existing installation. The executable and runtime directory retain the `pbs-discord` name for compatibility: account vaults, preferences and instance state use `/data/pbs-discord`. Inherited capture reports/settings also use `/data/pbs`. Do not package runtime data directories with a release. The console web tile uses title ID `PBSD09943` under `/user/app` and points to the local dashboard; tile registration is optional and can be omitted with `-DPBSD_NO_TILE=ON`.

## Capture arguments

The default is 720p with a requested 4,000,000 bit/s encoder budget. A verified 1080p60 capture setup uses these payload arguments (supply them through the loader):

```text
pbs-discord.elf --capture 1080p --bitrate 4000000 fps=60
```

The `fps=60` passthrough selects 60 fps; the dashboard also exposes capture settings. The encoder budget is a request, not a guarantee of measured throughput. Capture fallback retains the requested bitrate. Higher resolutions depend on capture/output availability; 4K and HEVC Discord streaming are not verified. `--tile-port` sets the console tile's dashboard URL port; PBS accepts its HTTP port as a positional passthrough argument. Legacy `--data`, `--port` and `--address` options are ignored by this entry point and do not relocate state or secure the dashboard.

Two-way voice chat is not included. For inherited PBS platforms and standalone backend build instructions, consult [upstream PBS](https://github.com/seregonwar/pbs-payload).

## GitHub Actions

The **PBS Cord** workflow runs host checks on pushes and pull requests. Its manual **Build PS5** option defaults to false; enable it to build a `PBS-Cord-PS5` artifact containing `pbs-discord.elf`. The existing Payload SDK action supplies the SDK using the repository variables `PS5_PAYLOAD_SDK_URL`, `PS5_PAYLOAD_SDK_REPO` and `PS5_PAYLOAD_SDK_REF`. Configure a compatible SDK before requesting that job.
