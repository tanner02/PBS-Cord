// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stddef.h>
#include <stdint.h>

/* The listener binds the requested port or, when another process already owns
 * it (typically a previous payload instance), the next free port inside this
 * window. Callers advertise pbs_h264_server_port(), not the request. */
#define PBS_H264_PORT_SCAN 10
/* RTP only: a sender that falls this far behind live treats the backlog as
 * stale, jumps to the newest frame and asks capture for a fresh IDR instead of
 * replaying lag. A shorter stall keeps the queued frames. */
#define PBS_H264_RTP_MAX_BACKLOG 6
int pbs_h264_server_start(int port, int width, int height, const unsigned char *sps, size_t sps_len,
                           const unsigned char *pps, size_t pps_len);
int pbs_h264_server_start_at_rate(int port, int width, int height, int fps,
    const unsigned char *sps, size_t sps_len, const unsigned char *pps, size_t pps_len);
/* Start a recovery dashboard without configuring any capture feed.
 * Fails if an HTTP server is already running; explicit stop/wait still applies. */
int pbs_h264_dashboard_start(int port);
int pbs_h264_dashboard_start_with_error(int port, const char *reason);
/* Port actually bound by the running listener, 0 when it is not running. */
int pbs_h264_server_port(void);
int pbs_h264_publish(const void *data, size_t length, uint32_t pts);
int pbs_audio_publish(const void *data, size_t length, uint32_t pts);
void pbs_audio_configure(const char *codec, int channels, int rate);
int pbs_h264_server_running(void);
/* Capture thread: consume client requests and periodic encoder refreshes. */
int pbs_h264_take_keyframe_request(void);
/* Capture thread: stop the capture feed without shutting down HTTP or Discord.
 * A successful start_at_rate clears the diagnostic. */
void pbs_h264_capture_failed(const char *reason);
void pbs_h264_server_stop(void);
/* Call from the capture thread after stop, before the payload can unload. */
void pbs_h264_server_wait(void);
void pbs_h264_discontinuity(void);
int pbs_h264_take_restart_request(void);
/* Only enable on backends that can reopen all advertised capture profiles. */
void pbs_h264_enable_profiles(void);
void pbs_h264_enable_profile_mask(unsigned mask);
/* Frame-rate capabilities are opt-in; existing backends keep their rate. */
void pbs_h264_enable_framerates(unsigned mask);
int pbs_h264_take_profile_request(int *height);
int pbs_h264_take_video_profile_request(int *height, int *fps);
void pbs_h264_profile_failed(void);
/* Announce a backend-selected compatibility profile before reopening capture.
 * Active dimensions/rate remain unchanged until start_at_rate succeeds. */
void pbs_h264_profile_fallback(int from_height, int height, int fps);
void pbs_h264_clear_profile_fallback(void);
