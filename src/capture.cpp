// SPDX-License-Identifier: GPL-3.0-or-later
#include "capture.hpp"
#include "capture_bridge.h"
#include "media.hpp"
#include <stdexcept>

namespace pbsd {
namespace {
constexpr std::size_t video_limit = 2 * 1024 * 1024,
                      video_budget = 8 * 1024 * 1024;
constexpr std::size_t video_queue_limit = 16;
constexpr std::size_t audio_limit = 64 * 1024, audio_budget = 1024 * 1024;
// normalize_h264 returns four-byte Annex B start codes. Cache only the small
// parameter NALs, never an entire caller-owned access unit.
Bytes parameter(View frame, unsigned type) {
  for (std::size_t i = 0; i + 4 < frame.size(); ++i) {
    if (frame[i] || frame[i + 1] || frame[i + 2] || frame[i + 3] != 1)
      continue;
    std::size_t end = i + 4;
    while (end + 4 <= frame.size() &&
           !(frame[end] == 0 && frame[end + 1] == 0 && frame[end + 2] == 0 &&
             frame[end + 3] == 1))
      ++end;
    if (end + 4 > frame.size())
      end = frame.size();
    if ((frame[i + 4] & 31) == type && end - i <= 4096)
      return Bytes(frame.begin() + i, frame.begin() + end);
  }
  return {};
}
Bytes configured_parameter(View raw, unsigned type) {
  if (raw.empty())
    return {};
  if (raw.size() > 4092)
    throw std::invalid_argument("parameter set too large");
  Bytes nal;
  if ((raw[0] & 31) == type) {
    nal = {0, 0, 0, 1};
    nal.insert(nal.end(), raw.begin(), raw.end());
  } else
    nal = normalize_h264(raw);
  auto result = parameter(nal, type);
  if (result.empty())
    throw std::invalid_argument("invalid parameter set");
  return result;
}
} // namespace
void CaptureFeed::configure(int width, int height, int fps, View sps,
                            View pps) {
  if (width <= 0 || height <= 0 || fps <= 0)
    throw std::invalid_argument("invalid capture dimensions");
  auto s = configured_parameter(sps, 7), p = configured_parameter(pps, 8);
  std::lock_guard lock(mutex_);
  video_.clear();
  audio_.clear();
  video_bytes_ = 0;
  info_.width = width;
  info_.height = height;
  info_.fps = fps;
  info_.running = true;
  ++info_.generation;
  info_.sps = std::move(s);
  info_.pps = std::move(p);
  waiting_keyframe_ = true;
  keyframe_ = true;
}
void CaptureFeed::video(View data, std::uint32_t pts) {
  std::lock_guard lock(mutex_);
  if (!info_.running)
    return;
  auto lose_video = [&] {
    dropped_ += video_.size();
    video_.clear();
    video_bytes_ = 0;
    waiting_keyframe_ = true;
    keyframe_ = true;
  };
  if (data.empty() || data.size() > video_limit) {
    lose_video();
    ++dropped_;
    return;
  }
  Bytes frame;
  try {
    frame = normalize_h264(data);
  } catch (const std::invalid_argument &) {
    lose_video();
    ++dropped_;
    return;
  }
  if (frame.empty() || frame.size() > video_limit) {
    lose_video();
    ++dropped_;
    return;
  }
  bool key = h264_keyframe(frame);
  auto s = parameter(frame, 7), p = parameter(frame, 8);
  if (!s.empty())
    info_.sps = s;
  if (!p.empty())
    info_.pps = p;
  // A hardware IDR can take longer than four frame intervals to transmit.
  // Retain a bounded burst window without changing the 8 MiB byte budget.
  if (video_.size() >= video_queue_limit || frame.size() > video_budget - video_bytes_)
    lose_video();
  if (waiting_keyframe_ && !key) {
    ++dropped_;
    return;
  }
  if (waiting_keyframe_ && key) {
    Bytes prefix;
    if (s.empty())
      prefix.insert(prefix.end(), info_.sps.begin(), info_.sps.end());
    if (p.empty())
      prefix.insert(prefix.end(), info_.pps.begin(), info_.pps.end());
    if (prefix.size() > video_limit - frame.size()) {
      lose_video();
      ++dropped_;
      return;
    }
    prefix.insert(prefix.end(), frame.begin(), frame.end());
    frame = std::move(prefix);
  }
  const auto size = frame.size();
  video_.push_back({std::move(frame), pts, key, info_.generation});
  video_bytes_ += size;
  waiting_keyframe_ = false;
}
void CaptureFeed::audio(View data, std::uint32_t pts) {
  std::lock_guard lock(mutex_);
  if (!info_.running)
    return;
  if (data.empty() || data.size() > audio_limit) {
    ++dropped_;
    return;
  }
  std::size_t total = 0;
  for (const auto &block : audio_)
    total += block.data.size();
  while (!audio_.empty() &&
         (audio_.size() >= 16 || data.size() > audio_budget - total)) {
    total -= audio_.front().data.size();
    audio_.pop_front();
    ++dropped_;
  }
  audio_.push_back({Bytes(data.begin(), data.end()), pts, info_.generation});
}
void CaptureFeed::discontinuity() {
  std::lock_guard lock(mutex_);
  ++info_.generation;
  video_.clear();
  audio_.clear();
  video_bytes_ = 0;
  waiting_keyframe_ = true;
  keyframe_ = true;
}
void CaptureFeed::stopped() {
  std::lock_guard lock(mutex_);
  ++info_.generation;
  info_.running = false;
  video_.clear();
  audio_.clear();
  video_bytes_ = 0;
  waiting_keyframe_ = true;
  keyframe_ = true;
}
CaptureInfo CaptureFeed::info() const {
  std::lock_guard lock(mutex_);
  return info_;
}
std::optional<VideoFrame> CaptureFeed::take_video() {
  std::lock_guard lock(mutex_);
  if (video_.empty())
    return {};
  auto frame = std::move(video_.front());
  video_.pop_front();
  video_bytes_ -= frame.data.size();
  return frame;
}
std::optional<AudioBlock> CaptureFeed::take_audio() {
  std::lock_guard lock(mutex_);
  if (audio_.empty())
    return {};
  auto block = std::move(audio_.front());
  audio_.pop_front();
  return block;
}
void CaptureFeed::flush() {
  std::lock_guard lock(mutex_);
  video_.clear();
  audio_.clear();
  video_bytes_ = 0;
  waiting_keyframe_ = true;
  keyframe_ = true;
}
void CaptureFeed::request_keyframe() { keyframe_ = true; }
bool CaptureFeed::consume_keyframe_request() {
  return keyframe_.exchange(false);
}
std::uint64_t CaptureFeed::dropped() const {
  std::lock_guard lock(mutex_);
  return dropped_;
}
CaptureFeed &capture_feed() {
  static CaptureFeed feed;
  return feed;
}
} // namespace pbsd

// C callers retain ownership of every input buffer. All data is copied during
// these calls; allocation/parser exceptions must never unwind through C code.
extern "C" {
void pbsd_capture_configure(int w, int h, int fps, const void *s, size_t sn,
                            const void *p, size_t pn) {
  try {
    if ((!s && sn) || (!p && pn))
      return;
    pbsd::capture_feed().configure(w, h, fps,
                                   {static_cast<const std::uint8_t *>(s), sn},
                                   {static_cast<const std::uint8_t *>(p), pn});
  } catch (...) {
  }
}
void pbsd_capture_video(const void *data, size_t n, uint32_t pts) {
  try {
    if (!data && n)
      return;
    pbsd::capture_feed().video({static_cast<const std::uint8_t *>(data), n},
                               pts);
  } catch (...) {
    try {
      pbsd::capture_feed().discontinuity();
    } catch (...) {
    }
  }
}
void pbsd_capture_audio(const void *data, size_t n, uint32_t pts) {
  try {
    if (!data && n)
      return;
    pbsd::capture_feed().audio({static_cast<const std::uint8_t *>(data), n},
                               pts);
  } catch (...) {
  }
}
void pbsd_capture_discontinuity(void) {
  try {
    pbsd::capture_feed().discontinuity();
  } catch (...) {
  }
}
void pbsd_capture_stopped(void) {
  try {
    pbsd::capture_feed().stopped();
  } catch (...) {
  }
}
int pbsd_capture_keyframe_request(void) {
  try {
    return pbsd::capture_feed().consume_keyframe_request();
  } catch (...) {
    return 0;
  }
}
}
