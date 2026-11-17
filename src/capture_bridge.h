// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void pbsd_capture_configure(int width,int height,int fps,const void* sps,size_t sps_len,const void* pps,size_t pps_len);
void pbsd_capture_video(const void* data,size_t length,uint32_t pts);
void pbsd_capture_audio(const void* data,size_t length,uint32_t pts);
void pbsd_capture_discontinuity(void);
void pbsd_capture_stopped(void);
int pbsd_capture_keyframe_request(void);
#ifdef __cplusplus
}
#endif
