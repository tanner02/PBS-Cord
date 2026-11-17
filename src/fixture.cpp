// SPDX-License-Identifier: GPL-3.0-or-later
#include "fixture.hpp"
#include "capture.hpp"
#include "media.hpp"
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <thread>
namespace pbsd {
void run_fixture(const std::string &path, int width, int height, int fps,
                 const std::atomic<bool> &quit) {
  std::ifstream f(path, std::ios::binary);
  if (!f)
    throw std::runtime_error("Cannot open H264 fixture");
  f.seekg(0, std::ios::end);
  auto size = f.tellg();
  if (size <= 0 || size > 32 * 1024 * 1024)
    throw std::runtime_error("Fixture must be at most32MiB");
  f.seekg(0);
  Bytes data(static_cast<std::size_t>(size));
  f.read(reinterpret_cast<char *>(data.data()), data.size());
  std::vector<Bytes> frames;
  Bytes current, sps, pps;
  for (std::size_t i = 0; i + 3 < data.size();) {
    std::size_t prefix = 0;
    if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)
      prefix = 3;
    else if (i + 4 < data.size() && data[i] == 0 && data[i + 1] == 0 &&
             data[i + 2] == 0 && data[i + 3] == 1)
      prefix = 4;
    if (!prefix) {
      ++i;
      continue;
    }
    auto begin = i + prefix;
    auto end = begin;
    while (
        end + 3 < data.size() &&
        !(data[end] == 0 && data[end + 1] == 0 &&
          (data[end + 2] == 1 || (end + 3 < data.size() && data[end + 2] == 0 &&
                                  data[end + 3] == 1))))
      ++end;
    if (end + 3 >= data.size())
      end = data.size();
    auto type = data[begin] & 31;
    if (type == 9 && !current.empty()) {
      frames.push_back(std::move(current));
      current.clear();
    }
    if (type == 7)
      sps = Bytes(data.begin() + begin, data.begin() + end);
    if (type == 8)
      pps = Bytes(data.begin() + begin, data.begin() + end);
    current.insert(current.end(), {0, 0, 0, 1});
    current.insert(current.end(), data.begin() + begin, data.begin() + end);
    i = end;
  }
  if (!current.empty())
    frames.push_back(std::move(current));
  if (frames.size() < 2 || sps.empty() || pps.empty())
    throw std::runtime_error(
        "Fixture requires AUD-delimited H264 frames and SPS/PPS");
  capture_feed().configure(width, height, fps, sps, pps);
  auto start = std::chrono::steady_clock::now();
  std::uint64_t index = 0, samples = 0;
  while (!quit) {
    auto target = start + std::chrono::microseconds(index * 1000000 / fps);
    std::this_thread::sleep_until(target);
    auto &frame = frames[index % frames.size()];
    capture_feed().video(frame,
                         static_cast<std::uint32_t>(index * 90000 / fps));
    std::uint64_t target_samples = (index + 1) * 48000 / fps;
    std::vector<float> audio((target_samples - samples) * 2);
    for (std::size_t i = 0; i < audio.size() / 2; ++i) {
      float v = static_cast<float>(
          .08 * std::sin(2 * 3.141592653589793 * 440 * (samples + i) / 48000));
      audio[i * 2] = audio[i * 2 + 1] = v;
    }
    capture_feed().audio({reinterpret_cast<const std::uint8_t *>(audio.data()),
                          audio.size() * sizeof(float)},
                         static_cast<std::uint32_t>(samples));
    samples = target_samples;
    ++index;
  }
  capture_feed().stopped();
}
} // namespace pbsd
