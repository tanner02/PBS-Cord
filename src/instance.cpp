// SPDX-License-Identifier: GPL-3.0-or-later
#include "instance.hpp"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
namespace pbsd {
namespace {
struct ScopedFd {
  int fd;
  ~ScopedFd() { if (fd >= 0) close(fd); }
};
bool busy_lock() { return errno == EWOULDBLOCK || errno == EAGAIN; }
bool send_restart(const std::string &path) {
  ScopedFd command{open(path.c_str(), O_WRONLY | O_CLOEXEC)};
  if (command.fd < 0) return false;
  ssize_t written;
  do { written = pwrite(command.fd, "R", 1, 0); } while (written < 0 && errno == EINTR);
  return written == 1;
}
} // namespace
PayloadInstance::PayloadInstance(std::string directory, std::function<void()> request_stop)
    : directory_(std::move(directory)), control_path_(directory_ + "/restart.request"),
      request_stop_(std::move(request_stop)) {}
PayloadInstance::~PayloadInstance() {
  closing_ = true;
  if (control_.joinable()) control_.join();
  if (control_fd_ >= 0) close(control_fd_);
  // The next owner cannot reset its request file until this lock releases.
  if (owner_) unlink(control_path_.c_str());
  if (lock_ >= 0) close(lock_);
}
bool PayloadInstance::acquire(std::string &diagnostic, std::chrono::milliseconds timeout) {
  if (lock_ >= 0) { diagnostic = "PBS instance guard already used"; return false; }
  auto fail = [&](const char *operation) {
    diagnostic = std::string(operation) + ": " + std::strerror(errno);
    return false;
  };
  if (mkdir(directory_.c_str(), 0700) < 0 && errno != EEXIST) return fail("PBS data directory");
  lock_ = open((directory_ + "/instance.lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  if (lock_ < 0) return fail("PBS instance lock");
  // Every loader takes the gate, including one that arrives during the tiny
  // gap between the old owner exiting and its waiting replacement starting.
  ScopedFd reload_gate{open((directory_ + "/reload.lock").c_str(),
                           O_RDWR | O_CREAT | O_CLOEXEC, 0600)};
  if (reload_gate.fd < 0) return fail("PBS reload lock");
  if (flock(reload_gate.fd, LOCK_EX | LOCK_NB) < 0) {
    if (!busy_lock()) return fail("PBS reload lock");
    diagnostic = "PBS restart already in progress";
    return false;
  }
  if (flock(lock_, LOCK_EX | LOCK_NB) < 0) {
    if (!busy_lock()) return fail("PBS instance lock");
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    bool requested = false, acquired = false;
    do {
      if (!requested) requested = send_restart(control_path_);
      if (flock(lock_, LOCK_EX | LOCK_NB) == 0) { acquired = true; break; }
      if (!busy_lock()) {
        return fail("PBS instance lock");
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    } while (std::chrono::steady_clock::now() < deadline);
    if (!acquired) {
      diagnostic = requested ? "Previous PBS is still shutting down; another instance was not started"
                             : "Previous PBS restart control is unavailable; another instance was not started";
      return false;
    }
  }
  owner_ = true;
  // The exclusive lock proves any old request is stale. Truncate it before
  // accepting reloads; a request from a crashed owner must not reboot us.
  control_fd_ = open(control_path_.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (control_fd_ < 0) return fail("PBS local restart control");
  try { control_ = std::thread([this] { serve(); }); }
  catch (...) {
    diagnostic = "PBS local restart thread could not start";
    return false;
  }
  diagnostic = "PBS instance acquired";
  return true;
}
void PayloadInstance::serve() {
  while (!closing_) {
    char byte = 0;
    if (pread(control_fd_, &byte, 1, 0) == 1 && byte == 'R') {
      if (!restart_.exchange(true)) std::fputs("PBS reload requested: waiting for capture cleanup\n", stderr);
    }
    // Reloads during initialization remain pending until the HTTP server
    // starts. The callback can then stop it through the normal lifecycle.
    if (restart_ && !closing_) request_stop_();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}
} // namespace pbsd
