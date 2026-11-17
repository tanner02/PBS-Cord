// SPDX-License-Identifier: GPL-3.0-or-later
/* Exercise profile controls through the real loopback HTTP routes. */
#include "common/http/server.h"
#include "common/media/profiles.h"
#include <arpa/inet.h>
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static const unsigned char sps[] = {0x67, 0x42, 0, 0x1e};
static const unsigned char pps[] = {0x68, 0xce, 6, 0xe2};
static int port;
static char response[4096];

static void configure(int height, int fps) {
  int width = height == 720 ? 1280 : 1920;
  assert(pbs_h264_server_start_at_rate(port, width, height, fps,
      sps, sizeof sps, pps, sizeof pps) == 0);
}

static void request(const char *method, const char *path, int expected) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  assert(fd >= 0);
  const struct timeval timeout = {1, 0};
  assert(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout) == 0);
  struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(port),
      .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
  assert(connect(fd, (struct sockaddr *)&address, sizeof address) == 0);
  char text[1024];
  int n = snprintf(text, sizeof text,
      "%s %s HTTP/1.1\r\nHost: localhost\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", method, path);
  assert(n > 0 && (size_t)n < sizeof text);
  assert(send(fd, text, (size_t)n, 0) == n);
  size_t used = 0;
  ssize_t got;
  while (used < sizeof response - 1 &&
      (got = recv(fd, response + used, sizeof response - 1 - used, 0)) > 0)
    used += (size_t)got;
  response[used] = 0;
  close(fd);
  char status[32];
  snprintf(status, sizeof status, "HTTP/1.1 %d ", expected);
  if (strncmp(response, status, strlen(status))) fprintf(stderr, "%s %s: %s\n", method, path, response);
  assert(!strncmp(response, status, strlen(status)));
}

static void contains(const char *text) { assert(strstr(response, text)); }
static void config(void) { request("GET", "/config.json", 200); }
static void post(const char *path, int code) { request("POST", path, code); }

int main(void) {
  alarm(10);
  int probe = socket(AF_INET, SOCK_STREAM, 0);
  assert(probe >= 0);
  struct sockaddr_in address = {.sin_family = AF_INET,
      .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
  assert(bind(probe, (struct sockaddr *)&address, sizeof address) == 0);
  socklen_t length = sizeof address;
  assert(getsockname(probe, (struct sockaddr *)&address, &length) == 0);
  port = ntohs(address.sin_port);
  close(probe);
  configure(720, 60);
  port = pbs_h264_server_port();
  assert(pbs_h264_dashboard_start_with_error(port, "Capture startup failed") == -1); // Never replace live capture.
  assert(pbs_h264_dashboard_start_with_error(port, "Startup failure") == -1);
  config(); contains("\"height\":720"); contains("\"capture_error\":\"\"");
  contains("\"framerates\":[60]"); contains("\"requested_fps\":60");
  post("/capture/profile?height=720&fps=30", 501);
  pbs_h264_enable_profile_mask(pbs_capture_profile_bit(720));
  post("/capture/profile?height=1080", 422);
  post("/capture/profile?height=720&fps=30", 422);
  configure(720, 30); // A legacy fixed-30 backend must not acquire 60 FPS.
  config(); contains("\"framerates\":[30]");
  post("/capture/profile?height=720&fps=60", 422);
  configure(720, 60);
  pbs_h264_enable_profiles();
  pbs_h264_enable_framerates(PBS_CAPTURE_FRAMERATES_ALL);
  config(); contains("\"framerates\":[30,60]");

  const char *invalid[] = {
      "/capture/profile?height=480&fps=30", "/capture/profile?height=720&fps=24",
      "/capture/profile?height=720&fps=", "/capture/profile?height=720&fps=+30",
      "/capture/profile?height=720&fps=30x", "/capture/profile?height=720&fps=30&fps=60",
      "/capture/profile?height=720&extra=1", "/capture/profile?height=-720",
      "/capture/profile?height=9999999999999999999999", "/capture/profile?fps=30&height=720"};
  for (unsigned i = 0; i < sizeof invalid / sizeof invalid[0]; ++i) post(invalid[i], 400);
  int height = 0, fps = 0;
  assert(!pbs_h264_take_video_profile_request(&height, &fps));
  assert(!pbs_h264_take_restart_request());
  post("/capture/profile?height=720&fps=60", 200);
  assert(!pbs_h264_take_video_profile_request(&height, &fps));

  // Same resolution with a new rate must trigger a real reopen request.
  post("/capture/profile?height=720&fps=30", 200);
  assert(pbs_h264_take_restart_request());
  assert(pbs_h264_take_video_profile_request(&height, &fps) && height == 720 && fps == 30);
  assert(!pbs_h264_take_video_profile_request(&height, &fps));
  config(); contains("\"requested_fps\":30"); contains("\"profile_changing\":true");
  post("/capture/profile?height=1080&fps=60", 409);
  configure(720, 60); // Correct height alone must not complete a rate change.
  config(); contains("\"profile_changing\":true");
  pbs_h264_profile_failed();
  config(); contains("\"profile_error\":true"); contains("\"profile_error_fps\":30");
  contains("\"profile_error_height\":720"); contains("\"requested_fps\":60");
  contains("\"profile_changing\":false");

  post("/capture/profile?height=1080&fps=60", 200);
  assert(pbs_h264_take_video_profile_request(&height, &fps) && height == 1080 && fps == 60);
  config(); contains("\"profile_error_fps\":0"); contains("\"profile_error\":false");
  configure(1080, 30); // Wrong FPS must not complete a resolution change.
  config(); contains("\"profile_changing\":true");
  configure(720, 60); // Wrong height must not complete it either.
  config(); contains("\"profile_changing\":true");
  configure(1080, 60);
  config(); contains("\"profile_changing\":false");

  // Height-only clients keep the active rate, including the legacy API.
  post("/capture/profile?height=720&fps=30", 200);
  assert(pbs_h264_take_video_profile_request(&height, &fps) && height == 720 && fps == 30);
  configure(720, 30);
  post("/capture/profile?height=1080", 200);
  assert(pbs_h264_take_video_profile_request(&height, &fps) && height == 1080 && fps == 30);
  configure(1080, 30);
  post("/capture/profile?height=720", 200);
  assert(pbs_h264_take_profile_request(&height) && height == 720);
  configure(720, 30);
  config(); contains("\"fps\":30"); contains("\"profile_changing\":false");

  // Compatibility fallback announces a new target without changing the active
  // capture. Preserve the rate, and retain the notice after successful reopen.
  const int fallback_rates[] = {60, 30};
  for (unsigned i = 0; i < 2; ++i) {
    int rate = fallback_rates[i];
    char path[96], expected[64];
    pbs_h264_enable_profiles();
    configure(720, rate);
    snprintf(path, sizeof path, "/capture/profile?height=1440&fps=%d", rate);
    post(path, 200);
    pbs_h264_profile_failed();
    post(path, 200); // Fallback must clear the superseded pending request.
    pbs_h264_profile_fallback(1440, 1080, rate);
    assert(!pbs_h264_take_video_profile_request(&height, &fps));
    config(); contains("\"height\":720"); contains("\"requested_height\":1080");
    contains("\"profile_changing\":true"); contains("\"profile_error\":false");
    contains("\"profile_error_height\":0"); contains("\"profile_error_fps\":0");
    contains("\"profile_fallback_height\":1440");
    // A compatible output may clear the warning without cancelling a profile
    // transition that is already pending.
    pbs_h264_clear_profile_fallback();
    config(); contains("\"profile_fallback_height\":0");
    contains("\"profile_changing\":true");
    pbs_h264_profile_fallback(1440, 1080, rate);
    snprintf(expected, sizeof expected, "\"requested_fps\":%d", rate);
    contains(expected);
    snprintf(expected, sizeof expected, "\"fps\":%d", rate);
    contains(expected);
    configure(1080, rate == 60 ? 30 : 60);
    config(); contains("\"profile_changing\":true");
    configure(720, rate);
    config(); contains("\"profile_changing\":true");
    configure(1080, rate);
    config(); contains("\"profile_changing\":false");
    contains("\"profile_fallback_height\":1440"); contains(expected);
    pbs_h264_enable_profile_mask(PBS_CAPTURE_PROFILES_ALL & ~pbs_capture_profile_bit(1440));
    post(path, 422);
    config(); contains("\"profile_fallback_height\":1440");
    snprintf(path, sizeof path, "/capture/profile?height=1080&fps=%d", rate);
    post(path, 200); // An explicit accepted selection acknowledges the notice.
    config(); contains("\"profile_fallback_height\":0");
    pbs_h264_profile_fallback(1440, 1080, rate);
    pbs_h264_profile_failed();
    config(); contains("\"profile_fallback_height\":0");
  }

  // A pending recording blocks profile changes without writing any media.
  post("/record/start", 200);
  post("/capture/profile?height=1080&fps=60", 409);
  assert(!pbs_h264_take_video_profile_request(&height, &fps));
  post("/record/stop", 200);
  pbs_h264_server_stop();
  pbs_h264_server_wait();
  // Startup fallback must also complete when no HTTP listener existed yet.
  pbs_h264_profile_fallback(1440, 1080, 30);
  configure(1080, 30);
  port = pbs_h264_server_port();
  config(); contains("\"profile_changing\":false");
  contains("\"height\":1080"); contains("\"fps\":30");
  contains("\"profile_fallback_height\":1440");
  post("/capture/profile?height=720&fps=30", 200);
  assert(pbs_h264_take_video_profile_request(&height, &fps) && height == 720 && fps == 30);
  config(); contains("\"profile_fallback_height\":0");

  // A terminal capture failure cancels pending work while HTTP stays available.
  pbs_h264_capture_failed("Encoder \"failed\"\\path\nreload\tneeded");
  assert(pbs_h264_server_running());
  assert(pbs_h264_server_port() == port);
  assert(!pbs_h264_take_video_profile_request(&height, &fps));
  assert(!pbs_h264_take_restart_request());
  config(); contains("\"capture_error\":\"Encoder \\\"failed\\\"\\\\path\\u000areload\\u0009needed\"");
  contains("\"profile_changing\":false");
  contains("\"requested_height\":1080");
  post("/capture/profile?height=720&fps=30", 409); contains("reload the payload");
  post("/capture/profile?height=1080&fps=30", 409); contains("reload the payload");
  assert(!pbs_h264_take_video_profile_request(&height, &fps));
  assert(!pbs_h264_take_restart_request());
  request("GET", "/", 200); contains("Content-Type: text/html");
  request("GET", "/frames", 503); contains("Capture unavailable");
  request("GET", "/audio", 503); contains("Capture unavailable");
  request("GET", "/rtp/video?port=5000", 503); contains("Capture unavailable");
  post("/record/start", 409); contains("reload the payload before recording");
  request("GET", "/status.json", 200); contains("\"backend\":\"h264\"");
  contains("\"recording\":false");
  config(); contains("\"capture_error\":\"Encoder");

  // Worst-case JSON expansion stays bounded and cannot hide the settings reply.
  char reason[512]; memset(reason, 1, sizeof reason - 1); reason[sizeof reason - 1] = 0;
  pbs_h264_capture_failed(reason);
  config(); contains("\"capture_error\":\"\\u0001");
  const char *error = strstr(response, "\"capture_error\":\"") + strlen("\"capture_error\":\"");
  const char *end = strchr(error, '"');
  assert(end && end - error == 191 * 6);
  configure(1080, 30);
  config(); contains("\"capture_error\":\"\"");
  post("/capture/profile?height=720&fps=30", 200);
  assert(pbs_h264_take_video_profile_request(&height, &fps) && height == 720 && fps == 30);
  pbs_h264_capture_failed(NULL);
  config(); contains("Capture stopped; reload the payload to retry");
  pbs_h264_server_stop();
  assert(!pbs_h264_server_running());
  assert(!pbs_h264_server_port());
  pbs_h264_server_wait();

  // Capture-error recovery serves the dashboard without capture metadata.
  assert(pbs_h264_dashboard_start_with_error(0, "Capture startup failed") == -1);
  assert(!pbs_h264_server_running());
  assert(pbs_h264_dashboard_start_with_error(port, "Capture startup failed") == 0);
  port = pbs_h264_server_port();
  assert(port > 0 && pbs_h264_server_running());
  request("GET", "/", 200); contains("Content-Type: text/html");
  config(); contains("\"width\":0"); contains("\"height\":0"); contains("\"fps\":0");
  contains("\"sps\":\"\""); contains("\"pps\":\"\""); contains("\"audio\":null");
  contains("\"capture_error\":\"Capture startup failed\"");
  contains("\"profile_changing\":false");
  request("GET", "/frames", 503);
  request("GET", "/audio", 503);
  request("GET", "/rtp/video?port=5000", 503);
  post("/record/start", 409); contains("Capture unavailable");
  post("/capture/profile?height=720&fps=30", 409); contains("reload the payload");
  assert(!pbs_h264_take_video_profile_request(&height, &fps));
  assert(!pbs_h264_take_restart_request());
  request("GET", "/status.json", 200); contains("\"width\":0"); contains("\"height\":0");
  contains("\"frames\":0"); contains("\"recording\":false");
  pbs_h264_server_stop();
  assert(!pbs_h264_server_running());
  assert(!pbs_h264_server_port());
  pbs_h264_server_wait();
  // A caller-provided encoder startup error reaches the dashboard.
  assert(pbs_h264_dashboard_start_with_error(port, "Encoder startup failed") == 0);
  port = pbs_h264_server_port();
  config(); contains("Encoder startup failed");
  request("GET", "/frames", 503);
  pbs_h264_server_stop();
  pbs_h264_server_wait();
  alarm(0);
  puts("HTTP video profiles, frame rates and compatibility: PASS");
  return 0;
}
