// SPDX-License-Identifier: GPL-3.0-or-later
// Host-only fixtures: each child execs before constructing a threaded guard.
#include "instance.hpp"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;
using namespace std::chrono_literals;
namespace {
std::vector<pid_t> children;
void check(bool ok, const std::string &message) {
  if (!ok) throw std::runtime_error(message);
}
void mark(const fs::path &path) {
  int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  check(fd >= 0, "create fixture marker");
  close(fd);
}
void event(const fs::path &dir, const std::string &name) {
  int fd = open((dir / "events").c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
  check(fd >= 0, "open event log");
  const auto line = name + "\n";
  check(write(fd, line.data(), line.size()) == static_cast<ssize_t>(line.size()),
        "append fixture event");
  close(fd);
}
std::string events(const fs::path &dir) {
  std::ifstream input(dir / "events");
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
void wait_marker(const fs::path &path, std::chrono::milliseconds timeout = 4s) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!fs::exists(path) && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(5ms);
  check(fs::exists(path), "marker timed out: " + path.filename().string());
}
void own_active(const fs::path &dir) {
  int fd = open((dir / "active").c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
  check(fd >= 0, "two payloads overlapped simulated capture ownership");
  close(fd);
}
int owner(const fs::path &dir, int cleanup_ms, int initialize_ms) {
  std::atomic<bool> initialized{}, stop{}, requested{}, applied{};
  {
    pbsd::PayloadInstance instance(dir.string(), [&] {
      if (!requested.exchange(true)) {
        event(dir, "stop-requested");
        mark(dir / "stop-requested");
      }
      if (initialized && !applied.exchange(true)) {
        event(dir, "stop-applied");
        stop = true;
      }
    });
    std::string diagnostic;
    check(instance.acquire(diagnostic, 2s), "owner acquire: " + diagnostic);
    own_active(dir);
    event(dir, "owner-start");
    mark(dir / "owner-ready");
    std::this_thread::sleep_for(std::chrono::milliseconds(initialize_ms));
    event(dir, "initialized");
    initialized = true;
    const auto deadline = std::chrono::steady_clock::now() + 8s;
    while (!stop && std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(5ms);
    check(stop && instance.restart_requested(), "owner did not receive restart");
    event(dir, "cleanup-start");
    mark(dir / "cleanup-start");
    std::this_thread::sleep_for(std::chrono::milliseconds(cleanup_ms));
    check(fs::remove(dir / "active"), "owner active sentinel missing");
    event(dir, "cleanup-done");
    mark(dir / "cleanup-done");
    // The ownership lock outlives all simulated capture cleanup.
  }
  event(dir, "owner-exit");
  return 0;
}
int replacement(const fs::path &dir, const std::string &label,
                int timeout_ms, bool barrier) {
  mark(dir / (label + "-ready"));
  if (barrier) wait_marker(dir / "release");
  pbsd::PayloadInstance instance(dir.string(), [] {});
  std::string diagnostic;
  if (!instance.acquire(diagnostic, std::chrono::milliseconds(timeout_ms))) {
    event(dir, label + "-refused");
    std::ofstream(dir / (label + "-diagnostic")) << diagnostic;
    return 20;
  }
  check(fs::exists(dir / "cleanup-done"), "replacement preceded old cleanup");
  own_active(dir);
  event(dir, "replacement-start");
  std::this_thread::sleep_for(300ms);
  check(fs::remove(dir / "active"), "replacement sentinel missing");
  return 0;
}
int normal(const fs::path &dir) {
  std::atomic<int> callbacks{};
  {
    pbsd::PayloadInstance instance(dir.string(), [&] { ++callbacks; });
    std::string diagnostic;
    check(instance.acquire(diagnostic, 300ms), "normal acquire: " + diagnostic);
    check(fs::exists(dir / "restart.request"), "missing restart request file");
    check(!instance.restart_requested(), "unexpected restart");
    std::this_thread::sleep_for(150ms);
  }
  check(callbacks == 0, "normal destruction requested restart");
  check(!fs::exists(dir / "restart.request"), "normal destruction left request file");
  return 0;
}
pid_t launch(const std::vector<std::string> &arguments) {
  pid_t pid = fork();
  check(pid >= 0, "fork fixture");
  if (!pid) {
    std::vector<char *> argv{const_cast<char *>("instance_test")};
    for (const auto &value : arguments)
      argv.push_back(const_cast<char *>(value.c_str()));
    argv.push_back(nullptr);
    execv("/proc/self/exe", argv.data());
    _exit(127);
  }
  children.push_back(pid);
  return pid;
}
int finish(pid_t pid, std::chrono::milliseconds timeout = 5s) {
  int status = 0;
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    const auto result = waitpid(pid, &status, WNOHANG);
    if (result == pid) {
      std::erase(children, pid);
      check(WIFEXITED(status), "fixture terminated unexpectedly");
      return WEXITSTATUS(status);
    }
    check(result == 0 || (result < 0 && errno == EINTR), "wait fixture");
    std::this_thread::sleep_for(5ms);
  }
  throw std::runtime_error("fixture exceeded deadline");
}
void crash(pid_t pid) {
  check(kill(pid, SIGKILL) == 0, "kill own fixture process");
  int status = 0;
  check(waitpid(pid, &status, 0) == pid && WIFSIGNALED(status), "reap crashed fixture");
  std::erase(children, pid);
}
void before(const std::string &log, const std::string &first, const std::string &second) {
  const auto a = log.find(first + "\n"), b = log.find(second + "\n");
  check(a != std::string::npos && b != std::string::npos && a < b,
        "event order: " + first + " before " + second);
}
struct Fixtures {
  fs::path root;
  Fixtures() {
    char directory[] = "/tmp/pbsd-instance-test-XXXXXX";
    const auto created = mkdtemp(directory);
    check(created != nullptr, "create fixture directory");
    root = created;
  }
  ~Fixtures() {
    for (auto pid : children) { kill(pid, SIGKILL); waitpid(pid, nullptr, 0); }
    children.clear();
    fs::remove_all(root);
  }
  fs::path make(const std::string &name) {
    auto dir = root / name;
    fs::create_directory(dir);
    return dir;
  }
};
} // namespace

int main(int argc, char **argv) {
  try {
    if (argc > 1) {
      const std::string mode = argv[1];
      if (mode == "owner" && argc == 5)
        return owner(argv[2], std::stoi(argv[3]), std::stoi(argv[4]));
      if (mode == "replacement" && argc == 6)
        return replacement(argv[2], argv[3], std::stoi(argv[4]), std::stoi(argv[5]) != 0);
      if (mode == "normal" && argc == 3) return normal(argv[2]);
      throw std::runtime_error("invalid fixture invocation");
    }
    Fixtures fixtures;
    {
      auto dir = fixtures.make("ordered");
      auto old = launch({"owner", dir, "250", "0"});
      wait_marker(dir / "owner-ready");
      auto next = launch({"replacement", dir, "next", "2000", "0"});
      check(finish(old) == 0 && finish(next) == 0, "ordered replacement failed");
      const auto log = events(dir);
      before(log, "stop-requested", "cleanup-start");
      before(log, "cleanup-done", "replacement-start");
    }
    {
      auto dir = fixtures.make("coalesced");
      auto old = launch({"owner", dir, "600", "0"});
      wait_marker(dir / "owner-ready");
      auto a = launch({"replacement", dir, "a", "2500", "1"});
      auto b = launch({"replacement", dir, "b", "2500", "1"});
      wait_marker(dir / "a-ready"); wait_marker(dir / "b-ready");
      mark(dir / "release");
      const auto ar = finish(a), br = finish(b);
      check(finish(old) == 0, "coalescing owner failed");
      check((ar == 0 && br == 20) || (ar == 20 && br == 0), "reloads did not coalesce");
      const auto log = events(dir);
      const auto first = log.find("replacement-start\n");
      check(first != std::string::npos && log.find("replacement-start\n", first + 1) == std::string::npos,
            "multiple replacement payloads started");
      before(log, "cleanup-done", "replacement-start");
    }
    {
      auto dir = fixtures.make("stalled");
      auto old = launch({"owner", dir, "5000", "0"});
      wait_marker(dir / "owner-ready");
      auto next = launch({"replacement", dir, "next", "250", "0"});
      check(finish(next) == 20, "stalled cleanup allowed replacement");
      wait_marker(dir / "cleanup-start");
      check(fs::exists(dir / "active") && !fs::exists(dir / "cleanup-done"),
            "stalled owner released simulated capture");
      check(events(dir).find("replacement-start") == std::string::npos, "overlap after timeout");
      crash(old);
    }
    {
      auto dir = fixtures.make("unavailable-control");
      auto old = launch({"owner", dir, "0", "0"});
      wait_marker(dir / "owner-ready");
      // An unavailable endpoint must never be mistaken for an abandoned lock.
      check(fs::remove(dir / "restart.request"), "remove own fixture request file");
      auto next = launch({"replacement", dir, "next", "200", "0"});
      check(finish(next) == 20, "missing control file allowed overlap");
      check(fs::exists(dir / "active") && !fs::exists(dir / "stop-requested"),
            "unreachable owner state changed");
      crash(old);
    }
    {
      auto dir = fixtures.make("crashed");
      auto old = launch({"owner", dir, "0", "0"});
      wait_marker(dir / "owner-ready");
      crash(old);
      check(fs::exists(dir / "restart.request"), "crash did not leave stale request file");
      std::ofstream(dir / "restart.request", std::ios::binary | std::ios::trunc) << 'R';
      fs::remove(dir / "active"); // The killed fixture no longer owns capture.
      check(finish(launch({"normal", dir})) == 0, "stale request or lock blocked recovery");
      check(finish(launch({"normal", dir})) == 0, "destructor did not release lock");
    }
    {
      auto dir = fixtures.make("initializing");
      auto old = launch({"owner", dir, "50", "650"});
      wait_marker(dir / "owner-ready");
      auto next = launch({"replacement", dir, "next", "2500", "0"});
      wait_marker(dir / "stop-requested");
      check(finish(old) == 0 && finish(next) == 0, "pending initialization restart was lost");
      const auto log = events(dir);
      before(log, "stop-requested", "initialized");
      before(log, "initialized", "stop-applied");
      before(log, "cleanup-done", "replacement-start");
    }
    std::cout << "Payload instance process ownership and reload fixtures passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "instance_test: " << error.what() << '\n';
    return 1;
  }
}
