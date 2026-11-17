/* Host-only regression: exercise the real HTTP server across socket faults. */
#include "common/http/server.h"
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static atomic_int accept_error, faults_seen, accepts_entered;
static atomic_int recovery_port, wrong_port, bind_failures, binds_seen;

int __real_accept(int fd, struct sockaddr *address, socklen_t *length);
int __real_bind(int fd, const struct sockaddr *address, socklen_t length);

int __wrap_accept(int fd, struct sockaddr *address, socklen_t *length) {
  atomic_fetch_add(&accepts_entered, 1);
  int client = __real_accept(fd, address, length);
  if (client < 0) return client;
  int error = atomic_exchange(&accept_error, 0);
  if (!error) return client;
  /* Wake a real blocked accept, then simulate its network failure. */
  close(client);
  atomic_fetch_add(&faults_seen, 1);
  errno = error;
  return -1;
}

int __wrap_bind(int fd, const struct sockaddr *address, socklen_t length) {
  int port = atomic_load(&recovery_port);
  if (port) {
    assert(address->sa_family == AF_INET);
    if (ntohs(((const struct sockaddr_in *)address)->sin_port) != port)
      atomic_store(&wrong_port, 1);
    atomic_fetch_add(&binds_seen, 1);
    int remaining = atomic_load(&bind_failures);
    if (remaining) {
      if (remaining > 0) atomic_fetch_sub(&bind_failures, 1);
      errno = EADDRINUSE;
      return -1;
    }
  }
  return __real_bind(fd, address, length);
}

static unsigned long long now_ms(void) {
  struct timespec t;
  assert(clock_gettime(CLOCK_MONOTONIC, &t) == 0);
  return (unsigned long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void pause_ms(void) {
  struct timespec t = {0, 10000000};
  nanosleep(&t, NULL);
}

static void await_counter(atomic_int *counter, int minimum) {
  unsigned long long deadline = now_ms() + 3000;
  while (atomic_load(counter) < minimum && now_ms() < deadline) pause_ms();
  assert(atomic_load(counter) >= minimum);
}

static int connect_local(int port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  assert(fd >= 0);
  struct timeval timeout = {0, 150000};
  assert(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout) == 0);
  assert(setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout) == 0);
  struct sockaddr_in address = {.sin_family = AF_INET,
    .sin_port = htons(port), .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
  if (connect(fd, (struct sockaddr *)&address, sizeof address) == 0) return fd;
  close(fd);
  return -1;
}

static int unused_port(void) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  assert(fd >= 0);
  struct sockaddr_in address = {.sin_family = AF_INET,
    .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
  assert(__real_bind(fd, (struct sockaddr *)&address, sizeof address) == 0);
  socklen_t size = sizeof address;
  assert(getsockname(fd, (struct sockaddr *)&address, &size) == 0);
  int port = ntohs(address.sin_port);
  close(fd);
  return port;
}

static int get_status(int port, unsigned frames) {
  int fd = connect_local(port);
  if (fd < 0) return 0;
  const char request[] = "GET /status.json HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
  if (send(fd, request, sizeof request - 1, MSG_NOSIGNAL) != sizeof request - 1) {
    close(fd); return 0;
  }
  char response[4096];
  size_t used = 0;
  ssize_t got;
  while (used < sizeof response - 1 &&
      (got = recv(fd, response + used, sizeof response - 1 - used, 0)) > 0)
    used += (size_t)got;
  close(fd);
  response[used] = 0;
  char expected[64];
  snprintf(expected, sizeof expected, "\"frames\":%u,", frames);
  return strstr(response, "HTTP/1.1 200 OK\r\n") && strstr(response, expected);
}

static void await_status(int port, unsigned frames) {
  unsigned long long deadline = now_ms() + 3000;
  while (now_ms() < deadline) {
    if (get_status(port, frames)) return;
    pause_ms();
  }
  assert(!"HTTP status did not recover with retained frame count");
}

static void inject_accept(int port, int error) {
  int seen = atomic_load(&faults_seen);
  atomic_store(&accept_error, error);
  int fd = connect_local(port);
  assert(fd >= 0);
  close(fd);
  await_counter(&faults_seen, seen + 1);
}

static void stop_promptly(void) {
  unsigned long long started = now_ms();
  pbs_h264_server_stop();
  pbs_h264_server_wait();
  assert(now_ms() - started < 1000);
  assert(!pbs_h264_server_running());
  assert(pbs_h264_server_port() == 0);
}

int main(void) {
  /* A broken accept loop or stop must fail within a bounded test runtime. */
  alarm(15);
  const unsigned char sps[] = {0x67, 0x42, 0, 0x1e};
  const unsigned char pps[] = {0x68, 0xce, 6, 0xe2};
  const unsigned char frame[] = {0, 0, 0, 1, 0x65, 0x88, 0x84};
  int requested = unused_port();
  assert(pbs_h264_server_start(requested, 640, 360, sps, sizeof sps, pps, sizeof pps) == 0);
  int port = pbs_h264_server_port();
  assert(port >= requested && port < requested + PBS_H264_PORT_SCAN);
  atomic_store(&recovery_port, port);
  assert(pbs_h264_publish(frame, sizeof frame, 0) == 0);
  unsigned frames = 1;
  await_status(port, frames);

  const int transient[] = {EINTR, ECONNABORTED, ECONNRESET, EPROTO, EAGAIN,
    EMFILE, ENFILE, ENOBUFS, ENOMEM};
  for (unsigned i = 0; i < sizeof transient / sizeof transient[0]; ++i) {
    inject_accept(port, transient[i]);
    await_status(port, frames);
    assert(atomic_load(&binds_seen) == 0);
  }

  const int network_errors[] = {ENETDOWN, ENETUNREACH};
  for (unsigned i = 0; i < sizeof network_errors / sizeof network_errors[0]; ++i) {
    int previous = atomic_load(&binds_seen);
    atomic_store(&bind_failures, 2);
    inject_accept(port, network_errors[i]);
    await_counter(&binds_seen, previous + 1);
    assert(pbs_h264_server_running());
    assert(pbs_h264_server_port() == port);
    assert(pbs_h264_publish(frame, sizeof frame, frames * 1500) == 0);
    ++frames;
    await_status(port, frames);
    assert(atomic_load(&binds_seen) == previous + 3);
    assert(!atomic_load(&wrong_port));
  }

  int previous = atomic_load(&binds_seen);
  atomic_store(&bind_failures, -1);
  inject_accept(port, ENETDOWN);
  await_counter(&binds_seen, previous + 3);
  assert(!atomic_load(&wrong_port));
  stop_promptly();

  /* Also stop a healthy listener while accept is blocked without a client. */
  atomic_store(&recovery_port, 0);
  atomic_store(&bind_failures, 0);
  int entered = atomic_load(&accepts_entered);
  assert(pbs_h264_server_start(port, 640, 360, sps, sizeof sps, pps, sizeof pps) == 0);
  await_counter(&accepts_entered, entered + 1);
  stop_promptly();
  alarm(0);
  puts("HTTP network recovery and shutdown: PASS");
  fflush(stdout);
  return 0;
}
