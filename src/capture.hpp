// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "types.hpp"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
namespace pbsd {
struct VideoFrame {
  Bytes data;
  std::uint32_t timestamp{};
  bool keyframe{};
  std::uint64_t generation{};
};
struct AudioBlock {
  Bytes data;
  std::uint32_t timestamp{};
  std::uint64_t generation{};
};
struct CaptureInfo {
  int width{}, height{}, fps{60};
  bool running{};
  std::uint64_t generation{};
  Bytes sps, pps;
};
class CaptureFeed {
public:
  void configure(int width, int height, int fps, View sps, View pps);
  void video(View data, std::uint32_t pts);
  void audio(View data, std::uint32_t pts);
  void discontinuity();
  void stopped();
  CaptureInfo info() const;
  std::optional<VideoFrame> take_video();
  std::optional<AudioBlock> take_audio();
  void flush();
  void request_keyframe();
  bool consume_keyframe_request();
  std::uint64_t dropped() const;

private:
  mutable std::mutex mutex_;
  CaptureInfo info_;
  std::deque<VideoFrame> video_;
  std::deque<AudioBlock> audio_;
  std::size_t video_bytes_{};
  std::uint64_t dropped_{};
  bool waiting_keyframe_{true};
  std::atomic<bool> keyframe_{true};
};
CaptureFeed &capture_feed();
} // namespace pbsd
