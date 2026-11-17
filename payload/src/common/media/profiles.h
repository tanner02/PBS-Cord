// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
/* The capture feature and size are one profile. Never reuse the 2160p
 * feature when reopening at 1080p (or vice versa). */
typedef struct { int width, height; unsigned feature; } pbs_capture_profile;
#define PBS_CAPTURE_PROFILES_ALL 15u
#define PBS_CAPTURE_FPS_30 1u
#define PBS_CAPTURE_FPS_60 2u
#define PBS_CAPTURE_FRAMERATES_ALL (PBS_CAPTURE_FPS_30 | PBS_CAPTURE_FPS_60)
static inline unsigned pbs_capture_framerate_bit(int fps) {
  return fps == 30 ? PBS_CAPTURE_FPS_30 : fps == 60 ? PBS_CAPTURE_FPS_60 : 0;
}
static inline unsigned pbs_capture_profile_bit(int height) {
  switch (height) { case 720:return 1; case 1080:return 2; case 1440:return 4; case 2160:return 8; default:return 0; }
}
static inline int pbs_capture_profile_for_height(int height, pbs_capture_profile *out) {
  pbs_capture_profile p;
  switch (height) {
    case 720: p = (pbs_capture_profile){1280, 720, 12}; break;
    case 1080: p = (pbs_capture_profile){1920, 1080, 12}; break;
    case 1440: p = (pbs_capture_profile){2560, 1440, 11}; break;
    case 2160: p = (pbs_capture_profile){3840, 2160, 10}; break;
    default: return -1;
  }
  if (out) *out = p;
  return 0;
}
