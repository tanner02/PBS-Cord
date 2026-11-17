// SPDX-License-Identifier: GPL-3.0-or-later
#include "pacer.hpp"
#include <cassert>
#include <deque>
#include <iostream>
#include <limits>

namespace {
void check_schedule() {
  pbsd::VideoPacer pacer;
  assert(pacer.bitrate() == 10000000);
  pacer.reset(100000);
  assert(pacer.ready(100000));
  pacer.sent(1200, 100000);
  assert(pacer.next_send_us() == 100960);
  assert(!pacer.ready(100959));
  assert(pacer.ready(100960));
  pacer.begin_frame(100500);
  assert(pacer.next_send_us() == 100960); // Preserve outstanding packet debt.

  pacer.set_target_bitrate(0);
  assert(pacer.bitrate() == 1250000);
  pacer.reset(0);
  pacer.sent(1198, 0);
  assert(pacer.next_send_us() == 7668); // 7667.2 us must round up.
  pacer.set_target_bitrate(2000000);
  assert(pacer.bitrate() == 5000000);
  pacer.set_target_bitrate(std::numeric_limits<std::uint64_t>::max());
  assert(pacer.bitrate() == 10000000);
  pacer.reset(50);
  pacer.sent(1, 50);
  assert(pacer.next_send_us() == 150); // Small packets retain the 100 us floor.
  pacer.reset(999);
  assert(pacer.next_send_us() == 999);
  assert(pacer.bitrate() == 10000000);
}

void check_bounded_credit() {
  pbsd::VideoPacer pacer;
  pacer.reset(0);
  pacer.begin_frame(1000);
  assert(pacer.next_send_us() == 0); // Saturating subtraction near clock zero.
  pacer.begin_frame(1000000);
  assert(pacer.next_send_us() == 995000);
  unsigned burst = 0;
  while (pacer.ready(1000000)) {
    pacer.sent(1200, 1000000);
    ++burst;
    assert(burst <= 6);
  }
  assert(burst == 6); // Five ms of saved credit, plus the currently due packet.
  assert((burst - 1) * 960 <= 5000);
  assert(pacer.next_send_us() > 1000000);
  assert(pacer.next_send_us() <= 1000960);
  // sent() itself must bound credit when a scheduler stalls mid-access-unit.
  pacer.sent(1200, 10000000);
  assert(pacer.next_send_us() == 9995960);

  const auto limit = std::numeric_limits<std::uint64_t>::max();
  pacer.reset(limit - 10);
  pacer.sent(1200, limit - 10);
  assert(pacer.next_send_us() == limit);
  pacer.reset(0);
  pacer.sent(std::numeric_limits<std::size_t>::max(), 0);
  assert(pacer.next_send_us() > 0); // Large input cannot wrap into free credit.
}

void check_frame_budget() {
  pbsd::VideoPacer pacer;
  pacer.reset(100000);
  pacer.begin_frame(100000, 7000);
  assert(pacer.bitrate() == 10000000); // Ordinary frames retain the base pace.
  pacer.sent(1200, 100000);
  assert(pacer.next_send_us() == 100960);
  pacer.begin_frame(100500, 450000);
  assert(pacer.next_send_us() == 100960); // A new frame cannot erase packet debt.
  assert(pacer.bitrate() == 64000000);
  pacer.set_target_bitrate(2000000);
  assert(pacer.bitrate() == 32000000);
  pacer.set_target_bitrate(500000);
  assert(pacer.bitrate() == 8000000); // Feedback applies during this same frame.
  pacer.reset(0);
  assert(pacer.bitrate() == 1250000); // Reset forgets the burst demand.
  pacer.set_target_bitrate(4000000);
  pacer.begin_frame(0, 30000, 1);
  assert(pacer.bitrate() == 12000000); // Minimum budget is twenty milliseconds.
  pacer.begin_frame(0, 100000, std::numeric_limits<std::uint64_t>::max());
  assert(pacer.bitrate() == 16000000); // Huge intervals saturate at fifty ms.
  pacer.begin_frame(0, std::numeric_limits<std::size_t>::max());
  assert(pacer.bitrate() == 64000000); // Demand arithmetic saturates, never wraps.
  pacer.sent(1, 0);
  assert(pacer.next_send_us() == 100); // Burst pace retains the small-packet floor.
  pacer.begin_frame(100);
  assert(pacer.bitrate() == 10000000); // Legacy calls clear any previous boost.
}

void check_burst_stream(std::size_t idr_bytes, std::size_t delta_bytes,
                        bool frame_aware) {
  struct Frame {
    std::size_t bytes;
    std::uint64_t captured;
    bool idr;
  };
  pbsd::VideoPacer pacer;
  pacer.reset(0);
  std::deque<Frame> queue;
  Frame pending{};
  std::size_t remaining = 0, captured_bytes = 0, sent_bytes = 0;
  std::size_t max_queue = 0, dropped = 0, frames_sent = 0, next_frame = 0;
  std::uint64_t longest_idr_us = 0, now = 0;
  bool waiting_keyframe = false;
  // Forty seconds at 60 fps, with one actual-IDR-sized burst every two seconds.
  // Each fragment carries 50 bytes of RTP/FU/transport-crypto overhead.
  // Model the sender's 2 ms loop, one pending AU and 16-packet work cap.
  for (; now < 45000000; now += 2000) {
    while (next_frame < 2400 && next_frame * 1000000 / 60 <= now) {
      Frame frame{next_frame % 120 == 0 ? idr_bytes : delta_bytes,
                  next_frame * 1000000 / 60, next_frame % 120 == 0};
      captured_bytes += frame.bytes;
      ++next_frame;
      if (queue.size() >= 16) {
        dropped += queue.size(); queue.clear(); waiting_keyframe = true;
      }
      if (waiting_keyframe && !frame.idr) { ++dropped; continue; }
      waiting_keyframe = false;
      queue.push_back(frame);
      max_queue = std::max(max_queue, queue.size());
    }
    if (!remaining && !queue.empty()) {
      pending = queue.front(); queue.pop_front(); remaining = pending.bytes;
      // Count every packet's wire overhead in the complete-frame budget.
      const auto wire_bytes = remaining + ((remaining + 1147) / 1148) * 50;
      pacer.begin_frame(now, frame_aware ? wire_bytes : 0);
    }
    for (unsigned count = 0; count < 16 && remaining && pacer.ready(now); ++count) {
      const auto payload = std::min<std::size_t>(1148, remaining);
      pacer.sent(payload + 50, now);
      remaining -= payload;
      sent_bytes += payload;
      if (!remaining) {
        ++frames_sent;
        if (pending.idr) longest_idr_us = std::max(longest_idr_us, now - pending.captured);
      }
    }
    if (next_frame == 2400 && queue.empty() && !remaining) break;
  }
  assert(dropped == 0);
  assert(max_queue <= 16);
  assert(frames_sent == 2400);
  assert(sent_bytes == captured_bytes);
  assert(longest_idr_us < 100000);
  assert(now < 45000000);
  std::cout << "Burst stream (IDR " << idr_bytes << " B): " << frames_sent << " frames, " << sent_bytes
            << " bytes preserved, max queue " << max_queue << ", longest IDR "
            << longest_idr_us << " us\n";
}
} // namespace

int main() {
  check_schedule();
  check_bounded_credit();
  check_frame_budget();
  check_burst_stream(90000, 7083, false); // Existing one-argument API behavior.
  check_burst_stream(250000, 6000, true); // 3.856 Mbps average source.
  check_burst_stream(350000, 5400, true); // 3.9704 Mbps average source.
  check_burst_stream(450000, 4500, true); // 3.942 Mbps average source.
  std::cout << "Video pacer tests passed\n";
}
