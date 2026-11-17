// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "capture.hpp"
#include "crypto.hpp"
#include <atomic>
#include <mutex>
#include <thread>
namespace pbsd {
struct RtcParams {
  std::string server_id, channel_id, user_id, session_id, token, endpoint;
  std::vector<std::string> members;
  bool stream{};
};
class VoiceSession {
public:
  VoiceSession(RtcParams params,
               std::shared_ptr<mlspp::SignaturePrivateKey> &identity);
  ~VoiceSession();
  void stop();
  Json status() const;
  std::string error() const;

private:
  RtcParams params_;
  std::shared_ptr<mlspp::SignaturePrivateKey> &identity_;
  std::thread thread_;
  std::atomic<bool> stop_{};
  std::atomic<bool> connected_{}, dave_ready_{};
  std::atomic<std::uint64_t> video_packets_{}, audio_packets_{}, bytes_{},
      frames_{}, repaired_{}, pacing_bitrate_{}, max_frame_send_us_{},
      max_frame_bytes_{}, max_frame_prepare_us_{};
  mutable std::mutex error_mutex_;
  std::string error_;
  void run();
};
} // namespace pbsd
