// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace pbsd {
// Clock values are supplied by the caller in microseconds. Capture bitrate
// stays separate from this transport pacing rate and its bounded burst credit.
class VideoPacer {
public:
  void reset(std::uint64_t now_us) { next_send_us_ = now_us; frame_bitrate_ = 0; }
  void begin_frame(std::uint64_t now_us, std::size_t wire_bytes = 0,
                   std::uint64_t frame_interval_us = 16667) {
    bound_credit(now_us);
    // Large access units get a bounded transport burst without changing the
    // encoded bitrate. Clamp before doubling to handle any caller interval.
    const auto budget = frame_interval_us >= 25000 ? 50000 :
        std::max<std::uint64_t>(20000, frame_interval_us * 2);
    const auto whole = static_cast<std::uint64_t>(wire_bytes) / budget;
    const auto remainder = static_cast<std::uint64_t>(wire_bytes) % budget;
    const auto fraction = (remainder * 8000000 + budget - 1) / budget;
    const auto limit = std::numeric_limits<std::uint64_t>::max();
    frame_bitrate_ = whole > (limit - fraction) / 8000000 ? limit :
        whole * 8000000 + fraction;
  }
  void set_target_bitrate(std::uint64_t bps) {
    target_bitrate_ = std::clamp<std::uint64_t>(bps, 500000, 4000000);
  }
  std::uint64_t bitrate() const {
    // Derive the cap from the current target, so new congestion feedback also
    // limits an access unit that is already partially transmitted.
    return std::min(target_bitrate_ * 16,
                    std::max(target_bitrate_ * 5 / 2, frame_bitrate_));
  }
  bool ready(std::uint64_t now_us) const { return now_us >= next_send_us_; }
  void sent(std::size_t wire_bytes, std::uint64_t now_us) {
    bound_credit(now_us);
    // Divide before multiplying to avoid overflow even for a size_t-sized
    // input. Round every packet upward, so rounding never increases send rate.
    const auto rate = bitrate();
    const auto whole = static_cast<std::uint64_t>(wire_bytes) / rate;
    const auto remainder = static_cast<std::uint64_t>(wire_bytes) % rate;
    const auto fraction = (remainder * 8000000 + rate - 1) / rate;
    const auto limit = std::numeric_limits<std::uint64_t>::max();
    const auto interval = whole > (limit - fraction) / 8000000
                              ? limit
                              : std::max<std::uint64_t>(100, whole * 8000000 + fraction);
    next_send_us_ = interval > limit - next_send_us_ ? limit : next_send_us_ + interval;
  }
  std::uint64_t next_send_us() const { return next_send_us_; }

private:
  void bound_credit(std::uint64_t now_us) {
    const auto earliest = now_us > 5000 ? now_us - 5000 : 0;
    next_send_us_ = std::max(next_send_us_, earliest);
  }
  std::uint64_t target_bitrate_{4000000};
  std::uint64_t frame_bitrate_{};
  std::uint64_t next_send_us_{};
};
} // namespace pbsd
