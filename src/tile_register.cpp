// SPDX-License-Identifier: GPL-3.0-or-later
// One-shot XMB tile installer. Run this payload once whenever the tile
// needs (re)installation; it stages the tile, registers it with
// AppInstUtil, prints the outcome and exits. On firmware 12 the module
// load can spend minutes in uninterruptible kernel time - that is
// expected: the payload exits when the call returns. It does not start
// the capture or any server, so nothing else depends on it.
#include "platform.hpp"
#include "xmb.hpp"
#include <csignal>
#include <cstdio>
#include <dave/logger.h>
#ifdef PBS_PS5
#include <unistd.h>
extern "C" int sceKernelSendNotificationRequest(int, void *, size_t, int);
namespace {
struct notify_request_t {
  char useless1[45];
  char message[3075];
};
void notify(const char *text) {
  notify_request_t req;
  __builtin_memset(&req, 0, sizeof req);
  __builtin_strncpy(req.message, text, sizeof req.message - 1);
  sceKernelSendNotificationRequest(0, &req, sizeof req, 0);
}
} // namespace
#endif
int main() {
  pbsd::initialize_platform_random();
  discord::dave::SetLogSink([](discord::dave::LoggingSeverity, const char *,
                               int, const std::string &) {});
  std::printf("PBS tile installer: staging and registering tile...\n");
  std::fflush(stdout);
  std::string diagnostic;
  if (pbsd::xmb_install_tile("http://127.0.0.1:9999/", diagnostic)) {
    std::printf("XMB tile: %s\n", diagnostic.c_str());
    std::fflush(stdout);
#ifdef PBS_PS5
    char message[160];
    std::snprintf(message, sizeof message, "PBS tile installer: %s",
                  diagnostic.c_str());
    notify(message);
#endif
    return 0;
  }
  std::printf("XMB tile unavailable: %s\n", diagnostic.c_str());
  std::fflush(stdout);
#ifdef PBS_PS5
  char message[160];
  std::snprintf(message, sizeof message, "PBS tile installer failed:\n%.120s",
                diagnostic.c_str());
  notify(message);
#endif
  return 1;
}
