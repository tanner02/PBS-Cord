// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cerrno>
#include <cstdint>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
namespace pbsd {
// Match the real listener's reuse policy. TIME_WAIT from a clean shutdown
// must not look like a still-running legacy payload. No SO_REUSEPORT: an
// active listener continues to fail this probe.
inline bool dashboard_port_available(std::uint16_t port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return false;
  int yes = 1;
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_ANY);
  int result = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
  if (result == 0)
    result = bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof address);
  int error = result < 0 ? errno : 0;
  close(fd);
  if (result < 0) errno = error;
  return result == 0;
}
} // namespace pbsd
