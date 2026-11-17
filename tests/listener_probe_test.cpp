// SPDX-License-Identifier: GPL-3.0-or-later
#include "listener_probe.hpp"
#include <arpa/inet.h>
#include <cassert>
#include <cerrno>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static int tcp_socket() {
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  assert(fd >= 0);
  const timeval timeout{1, 0};
  assert(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout) == 0);
  return fd;
}

int main() {
  alarm(10);
  const int listener = tcp_socket();
  const int reuse = 1;
  assert(setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse) == 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  assert(bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof address) == 0);
  socklen_t size = sizeof address;
  assert(getsockname(listener, reinterpret_cast<sockaddr *>(&address), &size) == 0);
  const auto port = ntohs(address.sin_port);
  assert(listen(listener, 1) == 0);

  errno = 0;
  assert(!pbsd::dashboard_port_available(port));
  assert(errno == EADDRINUSE);

  const int client = tcp_socket();
  assert(connect(client, reinterpret_cast<sockaddr *>(&address), sizeof address) == 0);
  const int accepted = accept(listener, nullptr, nullptr);
  assert(accepted >= 0);
  const timeval timeout{1, 0};
  assert(setsockopt(accepted, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout) == 0);

  // The server sends FIN first, so its local port retains the TIME_WAIT tuple.
  assert(shutdown(accepted, SHUT_WR) == 0);
  char byte;
  assert(recv(client, &byte, 1, 0) == 0);
  assert(close(client) == 0);
  assert(recv(accepted, &byte, 1, 0) == 0);
  assert(close(accepted) == 0);
  assert(close(listener) == 0);

  // Establish the actual kernel condition that broke the previous probe.
  const int raw_probe = tcp_socket();
  sockaddr_in any_address{};
  any_address.sin_family = AF_INET;
  any_address.sin_addr.s_addr = htonl(INADDR_ANY);
  any_address.sin_port = htons(port);
  assert(bind(raw_probe, reinterpret_cast<sockaddr *>(&any_address), sizeof any_address) == -1);
  assert(errno == EADDRINUSE);
  assert(close(raw_probe) == 0);
  assert(pbsd::dashboard_port_available(port));

  // A successful probe must release its socket immediately for the new server.
  const int replacement = tcp_socket();
  assert(setsockopt(replacement, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse) == 0);
  assert(bind(replacement, reinterpret_cast<sockaddr *>(&any_address), sizeof any_address) == 0);
  assert(listen(replacement, 1) == 0);
  assert(close(replacement) == 0);
  alarm(0);
}
