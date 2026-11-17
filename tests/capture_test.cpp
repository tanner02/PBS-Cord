// SPDX-License-Identifier: GPL-3.0-or-later
#include "capture.hpp"
#include "capture_bridge.h"
#include <cassert>
#include <iostream>
#include <thread>
using namespace pbsd;
namespace {
const Bytes sps{0x67, 0x42, 0x10, 0x1f}, pps{0x68, 0xce, 0x06};
const Bytes idr{0, 0, 0, 1, 0x65, 0x88}, delta{0, 0, 1, 0x41, 0x99};
void configure(CaptureFeed &f) { f.configure(1280, 720, 60, sps, pps); }
void recovery() {
  CaptureFeed f;
  configure(f);
  assert(f.consume_keyframe_request());
  assert(!f.consume_keyframe_request());
  f.video(delta, 1);
  assert(!f.take_video());
  f.video(idr, 2);
  auto first = f.take_video();
  assert(first && first->keyframe && first->timestamp == 2);
  const Bytes expected{0,    0, 0, 1, 0x67, 0x42, 0x10,
                       0x1f, 0, 0, 0, 1,    0x68, 0xce,
                       0x06, 0, 0, 0, 1,    0x65, 0x88};
  assert(first->data == expected);
  // Actual producer overflow flushes the sixteen queued frames, including their
  // IDR.
  f.video(idr, 3);
  for (unsigned i = 4; i < 19; ++i)
    f.video(delta, i);
  f.video(delta, 19);
  assert(!f.take_video());
  assert(f.consume_keyframe_request());
  f.video(delta, 20);
  assert(!f.take_video());
  f.video(idr, 21);
  assert(f.take_video()->data == expected);
  // A new IDR can recover in the same call that exhausts the queue.
  f.video(idr, 22);
  for (unsigned i = 23; i < 38; ++i)
    f.video(delta, i);
  f.video(idr, 38);
  auto recovered = f.take_video();
  assert(recovered && recovered->timestamp == 38);
  assert(!f.take_video());
  assert(f.dropped() >= 10);
}
void generations_and_bounds() {
  CaptureFeed f;
  configure(f);
  const auto g = f.info().generation;
  f.video(idr, 1);
  f.audio(Bytes{1, 2, 3}, 2);
  f.discontinuity();
  assert(f.info().generation == g + 1 && !f.take_video() && !f.take_audio());
  f.video(delta, 3);
  assert(!f.take_video());
  f.video(idr, 4);
  assert(f.take_video()->generation == g + 1);
  f.audio(Bytes{5}, 5);
  assert(f.take_audio()->generation == g + 1);
  f.video(idr, 6);
  f.audio(Bytes{9}, 6);
  configure(f);
  assert(f.info().generation == g + 2 && !f.take_video() && !f.take_audio());
  Bytes big(2 * 1024 * 1024 + 1, 1);
  f.video(big, 7);
  assert(!f.take_video());
  // The post-prepend size is bounded too.
  big.resize(2 * 1024 * 1024);
  big[0] = big[1] = big[2] = 0;
  big[3] = 1;
  big[4] = 0x65;
  f.video(big, 8);
  assert(!f.take_video());
  assert(f.consume_keyframe_request());
  // With in-band parameter sets the exact 2 MiB limit is accepted, and four
  // such frames fill the complete 8 MiB video budget without exceeding it.
  Bytes full{0, 0, 0, 1, 0x67, 0x42, 0, 0, 0, 1, 0x68, 0xce, 0, 0, 0, 1, 0x65};
  full.resize(2 * 1024 * 1024, 0x55);
  for (unsigned i = 0; i < 4; ++i)
    f.video(full, 30 + i);
  std::size_t bytes = 0;
  for (unsigned i = 0; i < 4; ++i) {
    auto frame = f.take_video();
    assert(frame);
    bytes += frame->data.size();
  }
  assert(bytes == 8 * 1024 * 1024 && !f.take_video());
  Bytes audio(64 * 1024, 0x44);
  for (unsigned i = 0; i < 17; ++i)
    f.audio(audio, i);
  for (unsigned i = 1; i < 17; ++i) {
    auto b = f.take_audio();
    assert(b && b->timestamp == i && b->data.size() == 65536 &&
           b->generation == g + 2);
  }
  assert(!f.take_audio());
  audio.push_back(1);
  f.audio(audio, 18);
  assert(!f.take_audio());
  f.video(idr, 19);
  f.audio(Bytes{1}, 19);
  f.stopped();
  assert(!f.info().running && !f.take_video() && !f.take_audio());
  f.video(idr, 20);
  f.audio(Bytes{1}, 20);
  assert(!f.take_video() && !f.take_audio());
}
void parameters_and_ownership() {
  CaptureFeed f;
  configure(f);
  Bytes combined{0, 0,    1,    0x67, 0x64, 0x11, 0,    0,
                 1, 0x68, 0xfa, 0,    0,    1,    0x65, 0x88};
  f.video(combined, 1);
  auto v = f.take_video();
  assert(v && v->data.size() == combined.size() + 3);
  combined.assign(combined.size(), 0xff);
  assert(v->data[4] == 0x67);
  f.discontinuity();
  f.video(idr, 2);
  auto recovered = f.take_video();
  assert(recovered && recovered->data[5] == 0x64 &&
         recovered->data[12] == 0xfa);
  f.flush();
  f.video(delta, 3);
  assert(!f.take_video());
  // Invalid C inputs and malformed media neither throw nor retain caller
  // memory.
  pbsd_capture_configure(1280, 720, 60, sps.data(), sps.size(), pps.data(),
                         pps.size());
  pbsd_capture_video(nullptr, 2, 0);
  pbsd_capture_audio(nullptr, 2, 0);
  Bytes bad{1, 2, 3};
  pbsd_capture_video(bad.data(), bad.size(), 0);
  pbsd_capture_video(idr.data(), idr.size(), 1);
  assert(capture_feed().take_video());
  pbsd_capture_stopped();
}
void concurrent() {
  CaptureFeed f;
  configure(f);
  std::thread producer([&] {
    for (unsigned i = 0; i < 2000; ++i) {
      f.video(i % 10 ? delta : idr, i);
      f.audio(Bytes{1, 2}, i);
    }
  });
  std::thread consumer([&] {
    for (unsigned i = 0; i < 2000; ++i) {
      f.take_video();
      f.take_audio();
      f.info();
      f.consume_keyframe_request();
    }
  });
  for (unsigned i = 0; i < 100; ++i)
    f.discontinuity();
  producer.join();
  consumer.join();
  std::size_t frames = 0, blocks = 0;
  while (f.take_video())
    ++frames;
  while (f.take_audio())
    ++blocks;
  assert(frames <= 4 && blocks <= 16);
}
} // namespace
int main() {
  recovery();
  generations_and_bounds();
  parameters_and_ownership();
  concurrent();
  std::cout << "capture tests passed\n";
}
