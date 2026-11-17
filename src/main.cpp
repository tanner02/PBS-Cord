// SPDX-License-Identifier: GPL-3.0-or-later
// Fork entry: PBS's own capture + web UI is the process main. The Discord
// engine boots lazily behind the /discord/* routes of PBS's server, and a
// detached thread registers the XMB tile pointing at the PBS dashboard.
#include "platform.hpp"
#include "xmb.hpp"
#include "instance.hpp"
#include "listener_probe.hpp"
#include "runtime_diagnostics.hpp"
#include "payload_arguments.hpp"
#include "discord_bridge.h"
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <dave/logger.h>
#include <string>
#include <thread>
#include <vector>
#ifdef PBS_PS5
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
extern "C" int pbs_capture_main(int argc, char **argv);
extern "C" void pbsd_notify(const char *message);
extern "C" int pbs_h264_server_running(void);
extern "C" void pbs_h264_server_stop(void);
extern "C" void pbs_h264_server_wait(void);
extern "C" int pbs_h264_dashboard_start_with_error(int port, const char *reason);
#else
#include "fixture.hpp"
#endif
#ifdef PBS_PS5
int main(int argc, char **argv) {
  pbsd::runtime_event("payload_begin");
  std::signal(SIGPIPE, SIG_IGN);
  const auto options = pbsd::parse_payload_arguments(argc, argv);
  const auto &capture_profile = options.capture_profile;
  const auto capture_bitrate = options.capture_bitrate;
  const auto tile_port = options.tile_port;
  const auto &passthrough = options.passthrough;
  if (capture_profile != "720p" && capture_profile != "1080p" &&
      capture_profile != "1440p" && capture_profile != "2160p") {
    std::fputs("PBS Discord: capture profile must be 720p, 1080p, 1440p or "
               "2160p\n",
               stderr);
    return 1;
  }
  if (capture_bitrate < 1000000 || capture_bitrate > 100000000) {
    std::fputs("PBS Discord: bitrate must be between 1000000 and 100000000\n",
               stderr);
    return 1;
  }
  pbsd_notify("PBS Discord: loading; checking previous instance");
  bool restart_notified = false; // Used only by the instance control thread.
  pbsd::PayloadInstance instance("/data/pbs-discord", [&] {
    if (!restart_notified) {
      pbsd::runtime_event("restart_requested");
      restart_notified = true;
      pbsd_notify("PBS Discord: restarting; waiting for capture cleanup");
    }
    if (pbs_h264_server_running()) pbs_h264_server_stop();
  });
  std::string instance_diagnostic;
  if (!instance.acquire(instance_diagnostic)) {
    std::fprintf(stderr, "%s\n", instance_diagnostic.c_str());
    pbsd_notify(instance_diagnostic.c_str());
    return 1;
  }
  pbsd::initialize_platform_random();
  discord::dave::SetLogSink([](discord::dave::LoggingSeverity, const char *,
                               int, const std::string &) {});
  // Compatibility guard for legacy builds without the ownership lock.
  // Reuse TIME_WAIT connections left by a clean restart, as the server does.
  if (!pbsd::dashboard_port_available(static_cast<std::uint16_t>(tile_port))) {
    if (errno == EADDRINUSE) {
      pbsd_notify("PBS Discord: another instance is still running.\n"
                  "Stop it from pldmgr or reboot, then load again.");
    } else pbsd_notify("PBS Discord: dashboard port probe failed");
    std::perror("PBS dashboard port probe");
    return 1;
  }
#if !defined(PBSD_NO_TILE)
  if (tile_port) {
    // Tile registration on firmware 12 spends minutes inside an
    // uninterruptible kernel call before completing. Detached, so PBS
    // capture and its dashboard start immediately regardless.
    std::thread([tile_port]() {
      std::string diagnostic;
      if (pbsd::xmb_install_tile(
              "http://127.0.0.1:" + std::to_string(tile_port) + "/",
              diagnostic))
        std::printf("XMB tile: %s\n", diagnostic.c_str());
      else
        std::printf("XMB tile unavailable: %s\n", diagnostic.c_str());
      std::fflush(stdout);
    }).detach();
  }
#endif // !PBSD_NO_TILE
  // PBS owns the process from here: avserve capture, its HTTP dashboard
  // (now with the Livestream tab) and its own notifications.
  const std::string rate = "bitrate=" + std::to_string(capture_bitrate);
  std::vector<std::string> owned = {"pbs-discord", "avserve", capture_profile,
                                    rate, "nocontroller"};
  for (const auto &arg : passthrough)
    owned.push_back(arg);
  std::vector<char *> args;
  for (const auto &s : owned)
    args.push_back(const_cast<char *>(s.c_str()));
  args.push_back(nullptr);
  pbsd::runtime_event("capture_begin");
  int rc = instance.restart_requested() ? 0 :
      pbs_capture_main(static_cast<int>(args.size() - 1), args.data());
  pbsd::runtime_event("capture_return", rc);
  pbsd::runtime_event("capture_restart_flag", instance.restart_requested());
  // An initial encoder failure may occur before any HTTP listener exists.
  // Keep the dashboard available without reopening the encoder.
  if (rc != 0 && !instance.restart_requested() && !pbs_h264_server_running() &&
      pbs_h264_dashboard_start_with_error(tile_port,
          "Capture startup failed. Reload PBS to retry") == 0) {
    pbsd::runtime_event("capture_failed_dashboard_ready", rc);
    pbs_h264_server_wait();
  }
  // Keep ownership through capture, HTTP worker and Discord cleanup.
  pbsd_discord_shutdown();
  pbsd::runtime_event("payload_end", rc);
  return rc;
}
#else
int main(int argc, char **argv) {
  pbsd::initialize_platform_random();
  discord::dave::SetLogSink([](discord::dave::LoggingSeverity, const char *,
                               int, const std::string &) {});
  std::string fixture;
  int width = 1280, height = 720, fps = 60;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&]() {
      if (i + 1 >= argc)
        throw std::runtime_error("Missing command-line value");
      return std::string(argv[++i]);
    };
    if (a == "--fixture")
      fixture = next();
    else if (a == "--width")
      width = std::stoi(next());
    else if (a == "--height")
      height = std::stoi(next());
    else if (a == "--fps")
      fps = std::stoi(next());
    else
      throw std::runtime_error("Unknown launch option");
  }
  if (fixture.empty())
    throw std::runtime_error(
        "Host build requires --fixture with an AUD-delimited H264 file");
  std::atomic<bool> quit{};
  try {
    pbsd::run_fixture(fixture, width, height, fps, quit);
  } catch (const std::exception &e) {
    std::fprintf(stderr, "Capture fixture: %s\n", e.what());
    return 1;
  }
  return 0;
}
#endif
