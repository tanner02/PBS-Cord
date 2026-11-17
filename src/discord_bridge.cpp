// SPDX-License-Identifier: GPL-3.0-or-later
#include "discord_bridge.h"
#include "controller.hpp"
#include <cstdlib>
#include <stdlib.h>
#include <cstring>
#include <exception>
#include <mutex>
#include <new>
#include <string>
#include <thread>
namespace {
// The console uses /data/pbs-discord; tests override via PBSD_ENGINE_DIR.
std::string engine_directory() {
  const char *override_dir = std::getenv("PBSD_ENGINE_DIR");
  return override_dir && *override_dir ? std::string(override_dir)
                                       : std::string("/data/pbs-discord");
}
std::mutex engine_mutex;
std::mutex shutdown_mutex;
std::thread engine_initializer;
pbsd::Controller *engine = nullptr;
std::string engine_error;
int engine_state = 0; // 0 starting, 1 ready, -1 failed
bool engine_start_requested = false;
bool engine_shutdown_requested = false;
const char shutdown_error[] = "Discord engine has shut down";
// Callers normally stop explicitly after HTTP workers have joined. This guard
// also owns initialization at normal process exit, so std::thread's destructor
// can never encounter a joinable worker if a caller omitted that stop.
struct EngineExitGuard {
  ~EngineExitGuard() { pbsd_discord_shutdown(); }
} engine_exit_guard;
void start_engine() {
  std::lock_guard lock(engine_mutex);
  if (engine_start_requested || engine_shutdown_requested)
    return;
  engine_start_requested = true;
  try {
    engine_initializer = std::thread([] {
      try {
        auto controller = new pbsd::Controller(engine_directory());
        std::lock_guard lock(engine_mutex);
        engine = controller;
        if (!engine_shutdown_requested)
          engine_state = 1;
      } catch (const std::exception &e) {
        std::lock_guard lock(engine_mutex);
        if (!engine_shutdown_requested) {
          engine_error = e.what();
          engine_state = -1;
        }
      } catch (...) {
        std::lock_guard lock(engine_mutex);
        if (!engine_shutdown_requested) {
          engine_error = "Unknown engine initialization failure";
          engine_state = -1;
        }
      }
    });
  } catch (const std::exception &e) {
    engine_error = e.what();
    engine_state = -1;
  }
}
std::string call_engine(const char *method, const char *route,
                        const char *body) {
  start_engine();
  {
    std::lock_guard lock(engine_mutex);
    if (engine_shutdown_requested)
      return "{\"error\":\"Discord engine has shut down\"}";
    if (engine_state == 0)
      return "{\"error\":\"Discord engine is starting\"}";
    if (engine_state < 0)
      return "{\"error\":\"Discord engine failed: " + engine_error + "\"}";
  }
  // The viewer adds a cache-busting query to every request. Dispatch by
  // pathname, leaving query handling on the other PBS routes unchanged.
  std::string path = "/api/" + std::string(route, std::strcspn(route, "?"));
  pbsd::Json parsed = pbsd::Json::object();
  if (body && *body) {
    try {
      parsed = pbsd::Json::parse(body);
    } catch (...) {
      return "{\"error\":\"Invalid JSON request body\"}";
    }
    if (!parsed.is_object())
      return "{\"error\":\"Invalid JSON request body\"}";
  }
  try {
    std::lock_guard lock(engine_mutex);
    // Parsing happens without the lock; shutdown may have completed since
    // the initial readiness check. Never dispatch through a detached engine.
    if (engine_shutdown_requested)
      return "{\"error\":\"Discord engine has shut down\"}";
    return engine->api(path, parsed).dump();
  } catch (const std::exception &e) {
    std::string escaped;
    for (char c : std::string(e.what())) {
      if (c == '"' || c == '\\')
        escaped += '\\';
      if (static_cast<unsigned char>(c) < 0x20)
        escaped += ' ';
      else
        escaped += c;
    }
    return "{\"error\":\"" + escaped + "\"}";
  } catch (...) {
    return "{\"error\":\"Unknown Discord engine failure\"}";
  }
}
} // namespace
extern "C" {
void pbsd_discord_shutdown(void) {
  // Repeated and simultaneous callers wait for the same complete teardown.
  std::lock_guard shutdown_lock(shutdown_mutex);
  {
    std::lock_guard lock(engine_mutex);
    engine_shutdown_requested = true;
    engine_state = -1;
    engine_error = shutdown_error;
  }
  // Initialization publishes its result under engine_mutex, so joining with
  // that mutex held would deadlock when shutdown races initialization.
  if (engine_initializer.joinable())
    engine_initializer.join();
  pbsd::Controller *controller;
  {
    std::lock_guard lock(engine_mutex);
    controller = engine;
    engine = nullptr;
  }
  // Controller teardown joins its workers. No API can enter it after the
  // terminal flag was set, and any previous API held engine_mutex to finish.
  delete controller;
}
int pbsd_discord_engine_state(void) {
  start_engine();
  std::lock_guard lock(engine_mutex);
  return engine_state;
}
const char *pbsd_discord_engine_error(void) {
  std::lock_guard lock(engine_mutex);
  return engine_error.c_str();
}
char *pbsd_discord_api(const char *method, const char *route,
                       const char *body) {
  // Every result is malloc'd: the caller frees unconditionally.
  const char *fallback = !method || !route
                             ? "{\"error\":\"Bad Discord route\"}"
                             : "{\"error\":\"Out of memory\"}";
  auto result = method && route
                    ? call_engine(method, route, body)
                    : std::string(fallback);
  auto *out = static_cast<char *>(std::malloc(result.size() + 1));
  if (!out)
    return strdup(fallback);
  std::memcpy(out, result.c_str(), result.size() + 1);
  return out;
}
void pbsd_discord_free(char *ptr) { std::free(ptr); }
}
