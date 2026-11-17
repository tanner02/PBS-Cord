// SPDX-License-Identifier: GPL-3.0-or-later
// Host test for the PBS <-> Discord engine bridge: engine boot, status
// shape, unknown-route error, and JSON body handling.
#include "discord_bridge.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif
namespace fs = std::filesystem;
void assert_shutdown() {
  assert(pbsd_discord_engine_state() == -1);
  assert(std::string(pbsd_discord_engine_error()) == "Discord engine has shut down");
  for (const char *route : {"status", "logout"}) {
    char *reply = pbsd_discord_api("POST", route, "{}");
    auto parsed = nlohmann::json::parse(reply);
    pbsd_discord_free(reply);
    assert(parsed.at("error") == "Discord engine has shut down");
  }
}
#ifndef _WIN32
void check_shutdown_lifecycle(bool initialize, bool explicit_shutdown) {
  // Each fresh child starts with untouched bridge globals. No controller
  // credentials are loaded and these paths never request a Discord login.
  const auto child = fork();
  assert(child >= 0);
  if (child == 0) {
    alarm(5);
    if (initialize)
      (void)pbsd_discord_engine_state();
    if (explicit_shutdown) {
      pbsd_discord_shutdown();
      pbsd_discord_shutdown();
      assert_shutdown();
    }
    // Normal exit also tests ownership of an initializer that might still
    // be running. An unjoined std::thread would terminate the process here.
    std::exit(0);
  }
  int result = 0;
  assert(waitpid(child, &result, 0) == child);
  assert(WIFEXITED(result) && WEXITSTATUS(result) == 0);
}
#endif
int main() {
  auto dir = (fs::temp_directory_path() / "pbsd-bridge-test").string();
  fs::remove_all(dir);
  fs::create_directories(dir);
#ifdef _WIN32
  _putenv_s("PBSD_ENGINE_DIR", dir.c_str());
#else
  setenv("PBSD_ENGINE_DIR", dir.c_str(), 1);
#endif
#ifndef _WIN32
  check_shutdown_lifecycle(false, true); // Shutdown before the first request.
  check_shutdown_lifecycle(true, true); // Immediate shutdown races initialization.
  check_shutdown_lifecycle(true, false); // Exit guard owns any remaining thread.
#endif
  // The engine boots lazily on the first call; give the owned thread a
  // moment and accept either a ready status or a clear failure.
  char *first = pbsd_discord_api("GET", "status", nullptr);
  assert(first && *first == '{');
  std::string status = first;
  pbsd_discord_free(first);
  for (int attempt = 0; attempt < 100; ++attempt) {
    if (pbsd_discord_engine_state() != 0)
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  int state = pbsd_discord_engine_state();
  assert(state == 1 || state == -1);
  if (state == 1) {
    char *fresh = pbsd_discord_api("GET", "status", nullptr);
    assert(fresh && std::string(fresh).find("\"state\"") != std::string::npos);
    assert(std::string(fresh).find("\"capture\"") != std::string::npos);
    pbsd_discord_free(fresh);
  } else {
    assert(pbsd_discord_engine_error() && *pbsd_discord_engine_error());
  }
  // Exercise the exact cache-busted paths used by the browser, for both
  // polling and actions. These operations use only this test's empty vault.
  char *queried = pbsd_discord_api("GET", "status?ts=123&extra=1", nullptr);
  auto parsed = nlohmann::json::parse(queried);
  pbsd_discord_free(queried);
  assert(parsed.at("state") == "disconnected");
  assert(parsed.at("error") == "");
  assert(parsed.at("verification").is_null());
  char *action = pbsd_discord_api("POST", "logout?ts=456", "{\"forget\":false}");
  parsed = nlohmann::json::parse(action);
  pbsd_discord_free(action);
  assert(parsed.at("ok") == true);
  // Stale verification submissions and cancellation cannot create a login.
  char *stale = pbsd_discord_api("POST", "verify?ts=789",
      "{\"challenge_id\":\"expired\",\"solution\":\"fixture\"}");
  parsed = nlohmann::json::parse(stale);
  pbsd_discord_free(stale);
  assert(parsed.contains("error"));
  char *cancel = pbsd_discord_api("POST", "cancel-link", "{}");
  parsed = nlohmann::json::parse(cancel);
  pbsd_discord_free(cancel);
  assert(parsed.contains("error"));
  // Unknown route and invalid bodies produce JSON errors, never crashes.
  char *unknown = pbsd_discord_api("POST", "nonsense", "{}");
  assert(std::string(unknown).find("error") != std::string::npos);
  pbsd_discord_free(unknown);
  char *badbody = pbsd_discord_api("POST", "start", "{not json");
  assert(std::string(badbody).find("error") != std::string::npos);
  pbsd_discord_free(badbody);
  char *nullbody = pbsd_discord_api("POST", "start", nullptr);
  assert(std::string(nullbody).find("error") != std::string::npos);
  pbsd_discord_free(nullbody);
  // Preferences are available before linking, without account authentication.
  char *activity = pbsd_discord_api("GET", "activity?ts=100", nullptr);
  parsed = nlohmann::json::parse(activity);
  pbsd_discord_free(activity);
  assert(parsed.at("enabled") == false && parsed.at("game").is_null());
  activity = pbsd_discord_api("POST", "activity", "{\"enabled\":true}");
  parsed = nlohmann::json::parse(activity);
  pbsd_discord_free(activity);
  assert(parsed.at("enabled") == true);
  activity = pbsd_discord_api("POST", "activity", "{\"enabled\":\"false\"}");
  parsed = nlohmann::json::parse(activity);
  pbsd_discord_free(activity);
  assert(parsed.contains("error"));
  activity = pbsd_discord_api("GET", "status", nullptr);
  parsed = nlohmann::json::parse(activity);
  pbsd_discord_free(activity);
  assert(parsed.at("activity").at("enabled") == true);
  assert(parsed.at("state") == "disconnected");
  activity = pbsd_discord_api("POST", "activity", "{\"enabled\":false}");
  parsed = nlohmann::json::parse(activity);
  pbsd_discord_free(activity);
  assert(parsed.at("enabled") == false);
  // Race status dispatch against simultaneous shutdown callers. Calls that
  // already entered may finish; later calls must return the terminal error.
  std::atomic<bool> finish{false};
  std::thread reader([&] {
    while (!finish.load()) {
      char *reply = pbsd_discord_api("GET", "status", nullptr);
      auto value = nlohmann::json::parse(reply);
      pbsd_discord_free(reply);
      assert(value.contains("state") ||
             value.value("error", "") == "Discord engine has shut down");
    }
  });
  std::thread first_shutdown(pbsd_discord_shutdown);
  std::thread second_shutdown(pbsd_discord_shutdown);
  first_shutdown.join();
  second_shutdown.join();
  finish = true;
  reader.join();
  assert_shutdown();
  pbsd_discord_shutdown();
  assert_shutdown();
  fs::remove_all(dir);
  std::cout << "bridge tests passed\n";
}
