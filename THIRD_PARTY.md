# Third-party sources and notices

PBS-Cord retains the PBS Payload source, copyright headers and GPL-3.0-or-later license. PBS base: [seregonwar/pbs-payload](https://github.com/seregonwar/pbs-payload), revision `130fd05a700d5978e9dc3c1258f9d0f7ed116844`. The Discord integration source baseline is `c5ed4840d96f9679eecdb560d7d57ebec6e233a9`, followed by local integration changes. See [SOURCE-MANIFEST.txt](SOURCE-MANIFEST.txt) for component revisions. Component licenses and copyright notices remain applicable; preserve them when redistributing source or binaries.

| Component | Origin and revision | License / notices |
| --- | --- | --- |
| Acheron SPS parser | [ouwou/acheron](https://github.com/ouwou/acheron), `36618c43bee62392dccf03bb67a27b5b67052818` | GPLv3; [LICENSES/Acheron-GPL-3.0.txt](LICENSES/Acheron-GPL-3.0.txt). `src/sps.cpp` adapts `H264Sps.cpp`, `H264Sps.hpp`, `BitReader.hpp` and `NalUnits.hpp`, with local bounds and atomic failure handling. |
| libdave | [discord/libdave](https://github.com/discord/libdave), `52cd56dc550f447fb354b3a06c9e2d2e2a4309c6` | MIT; retained in `vendor/libdave`, including external-sender test code. |
| MLS++ | [cisco/mlspp](https://github.com/cisco/mlspp), `92aaa4134fa45ec39957a7c81a342401fba7feb2` | BSD-2-Clause and retained component notices in `vendor/mlspp`. |
| QR Code generator | [Project Nayuki](https://www.nayuki.io/page/qr-code-generator-library) | MIT, Copyright Project Nayuki; [payload/web/qrcodegen.js](payload/web/qrcodegen.js) and [payload/web/LICENSE-qrcodegen.txt](payload/web/LICENSE-qrcodegen.txt). |
| OpenSSL 3.5.9 | [openssl/openssl](https://github.com/openssl/openssl) | Apache-2.0; [LICENSES/OpenSSL.txt](LICENSES/OpenSSL.txt). |
| libsodium 1.0.20 | [libsodium](https://download.libsodium.org/libsodium/releases/) | ISC; [LICENSES/libsodium.txt](LICENSES/libsodium.txt). |
| Opus 1.5.2 | [xiph/opus](https://github.com/xiph/opus) | BSD-style component licenses; [LICENSES/Opus-COPYING.txt](LICENSES/Opus-COPYING.txt) and [LICENSES/Opus-README.txt](LICENSES/Opus-README.txt). |
| nlohmann/json 3.12.0 | [nlohmann/json](https://github.com/nlohmann/json) | MIT; [LICENSES/nlohmann-json.txt](LICENSES/nlohmann-json.txt). |

The inherited browser player and its dependencies retain their bundled notices in [payload/web/vendor/THIRD-PARTY-LICENSES](payload/web/vendor/THIRD-PARTY-LICENSES). Other inherited PBS source and assets retain their original notices.

Static PS5 dependency revisions are pinned by `scripts/build-ps5-deps.sh`. Host builds instead use the installed system libraries. The native build uses the separately installed SDK's C/C++ runtimes and API stubs; the SDK is not bundled. Upstream LLVM runtime license texts are retained as `LICENSES/LLVM-libcxx.txt`, `LLVM-libcxxabi.txt` and `LLVM-libunwind.txt` (Apache-2.0 with LLVM exceptions and notices).

libdave and MLS++ are pinned Git submodules. Initialize them as described in
[BUILDING.md](BUILDING.md); their original source, copyright and license notices
remain in their respective repositories at the revisions listed above.

`vendor/ca-bundle.pem` contains public trust certificates from Ubuntu's ca-certificates package, primarily Mozilla data. Copyright and MPL-2.0 text are in [LICENSES/ca-certificates.txt](LICENSES/ca-certificates.txt). No private key is distributed. Media tests generate FFmpeg testsrc2 video and local sine tones rather than shipping captured account, game or microphone data.
