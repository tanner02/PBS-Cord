// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#ifdef PBS_PS5
#include <cstdio>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#endif
namespace pbsd {
// Only call with constant event names and numeric codes. No account data,
// protocol payloads, captured audio or session keys enter this file.
inline void runtime_event(const char *event, int code = 0) noexcept {
#ifdef PBS_PS5
  timespec time{};
  clock_gettime(CLOCK_MONOTONIC, &time);
  char line[160];
  const int length = std::snprintf(line, sizeof line, "%lld.%03ld pid=%d %s code=%d\n",
      static_cast<long long>(time.tv_sec), time.tv_nsec / 1000000,
      static_cast<int>(getpid()), event, code);
  if (length <= 0 || length >= static_cast<int>(sizeof line)) return;
  const int fd = open("/data/pbs-discord/lifecycle.log",
      O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd < 0) return;
  (void)write(fd, line, static_cast<unsigned>(length));
  close(fd);
#else
  (void)event; (void)code;
#endif
}
} // namespace pbsd
