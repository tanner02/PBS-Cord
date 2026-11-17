// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from Acheron Core/Media/H264Sps.cpp, H264Sps.hpp, BitReader.hpp,
// and Discord/Voice/NalUnits.hpp (GPL-3.0). The bounded parser and atomic,
// fail-closed public API are local adaptations.
#include "sps.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <stdexcept>

namespace pbsd {
namespace {
[[noreturn]] void malformed() {
  throw std::invalid_argument("malformed or unsupported H.264 SPS");
}
class Reader {
  View data_;
  std::size_t at_{};

public:
  explicit Reader(View data) : data_(data) {}
  std::size_t position() const { return at_; }
  std::size_t left() const { return data_.size() * 8 - at_; }
  void skip(std::size_t n) {
    if (n > left())
      malformed();
    at_ += n;
  }
  std::uint32_t bits(unsigned n) {
    if (n > 32 || n > left())
      malformed();
    std::uint32_t value = 0;
    while (n--) {
      value = (value << 1) | ((data_[at_ / 8] >> (7 - at_ % 8)) & 1);
      ++at_;
    }
    return value;
  }
  bool flag() { return bits(1) != 0; }
  std::uint32_t
  ue(std::uint32_t maximum = std::numeric_limits<std::uint32_t>::max() - 1) {
    unsigned zeros = 0;
    while (!flag())
      if (++zeros > 31)
        malformed();
    auto value = (std::uint32_t(1) << zeros) - 1 + bits(zeros);
    if (value > maximum)
      malformed();
    return value;
  }
  std::int32_t se() {
    const auto code = ue();
    const auto magnitude = std::int32_t((code + std::uint64_t(1)) / 2);
    return code & 1 ? magnitude : -magnitude;
  }
  void trailing() {
    if (!flag() || left() > 7)
      malformed();
    while (left())
      if (flag())
        malformed();
  }
};
class Writer {
  Bytes bytes_;
  std::size_t used_{};

public:
  void bits(std::uint64_t value, unsigned count) {
    for (unsigned i = count; i > 0; --i, ++used_) {
      if (used_ % 8 == 0)
        bytes_.push_back(0);
      bytes_.back() |=
          std::uint8_t(((value >> (i - 1)) & 1) << (7 - used_ % 8));
    }
  }
  void flag(bool value) { bits(value, 1); }
  void ue(std::uint32_t value) {
    const auto code = std::uint64_t(value) + 1;
    const unsigned count = std::bit_width(code);
    bits(0, count - 1);
    bits(code, count);
  }
  Bytes finish() {
    flag(true);
    return std::move(bytes_);
  }
};
Bytes unescape(View nal) {
  if (nal.size() > 4096)
    malformed();
  Bytes rbsp;
  rbsp.reserve(nal.size());
  unsigned zeros = 0;
  for (std::size_t i = 0; i < nal.size(); ++i) {
    auto byte = nal[i];
    if (zeros >= 2 && byte == 3) {
      if (i + 1 == nal.size() || nal[i + 1] > 3)
        malformed();
      zeros = 0;
      continue;
    }
    if (zeros >= 2 && byte < 3)
      malformed();
    rbsp.push_back(byte);
    zeros = byte == 0 ? zeros + 1 : 0;
  }
  return rbsp;
}
Bytes escape(View rbsp) {
  Bytes out;
  unsigned zeros = 0;
  for (auto byte : rbsp) {
    if (zeros >= 2 && byte <= 3) {
      out.push_back(3);
      zeros = 0;
    }
    out.push_back(byte);
    zeros = byte == 0 ? zeros + 1 : 0;
  }
  return out;
}
void scaling(Reader &r, unsigned size) {
  int last = 8, next = 8;
  for (unsigned i = 0; i < size; ++i) {
    if (next) {
      const auto delta = r.se();
      if (delta < -128 || delta > 127)
        malformed();
      next = (last + delta) & 255;
    }
    if (next)
      last = next;
  }
}
void hrd(Reader &r) {
  const auto count = r.ue(31) + 1;
  r.skip(8);
  for (unsigned i = 0; i < count; ++i) {
    r.ue();
    r.ue();
    r.skip(1);
  }
  r.skip(20);
}
struct Parsed {
  Bytes rbsp;
  std::size_t kept{};
  bool vui{}, restriction{};
  std::uint32_t refs{}, reorder{}, buffer{};
};
Parsed parse(View nal) {
  if (nal.empty() || (nal[0] & 0x9f) != 7 || (nal[0] & 0x60) == 0)
    malformed();
  Parsed p;
  p.rbsp = unescape(nal);
  Reader r(p.rbsp);
  r.skip(8);
  const auto profile = r.bits(8);
  const auto constraints = r.bits(8);
  if (constraints & 3)
    malformed();
  r.skip(8);
  r.ue(31);
  constexpr std::array<unsigned, 13> high{100, 110, 122, 244, 44,  83, 86,
                                          118, 128, 138, 139, 134, 135};
  if (std::find(high.begin(), high.end(), profile) != high.end()) {
    const auto chroma = r.ue(3);
    if (chroma == 3)
      r.skip(1);
    r.ue(6);
    r.ue(6);
    r.skip(1);
    if (r.flag())
      for (unsigned i = 0; i < (chroma == 3 ? 12u : 8u); ++i)
        if (r.flag())
          scaling(r, i < 6 ? 16 : 64);
  } else if (profile != 66 && profile != 77 && profile != 88)
    malformed();
  r.ue(12);
  const auto poc = r.ue(2);
  if (poc == 0)
    r.ue(12);
  else if (poc == 1) {
    r.skip(1);
    r.se();
    r.se();
    const auto cycle = r.ue(255);
    for (unsigned i = 0; i < cycle; ++i)
      r.se();
  }
  p.refs = r.ue(16);
  r.skip(1);
  r.ue(65535);
  r.ue(65535);
  if (!r.flag())
    r.skip(1);
  r.skip(1);
  if (r.flag())
    for (unsigned i = 0; i < 4; ++i)
      r.ue(65535);
  p.kept = r.position();
  p.vui = r.flag();
  if (p.vui) {
    if (r.flag()) {
      const auto ratio = r.bits(8);
      if (ratio == 255) {
        if (!r.bits(16) || !r.bits(16))
          malformed();
      } else if (ratio > 16)
        malformed();
    }
    if (r.flag())
      r.skip(1);
    if (r.flag()) {
      const auto format = r.bits(3);
      if (format > 5)
        malformed();
      r.skip(1);
      if (r.flag())
        r.skip(24);
    }
    if (r.flag()) {
      r.ue(5);
      r.ue(5);
    }
    if (r.flag()) {
      const auto units = r.bits(32), scale = r.bits(32);
      r.skip(1);
      if (!units || !scale)
        malformed();
    }
    const bool nal_hrd = r.flag();
    if (nal_hrd)
      hrd(r);
    const bool vcl_hrd = r.flag();
    if (vcl_hrd)
      hrd(r);
    if (nal_hrd || vcl_hrd)
      r.skip(1);
    r.skip(1);
    p.kept = r.position();
    p.restriction = r.flag();
    if (p.restriction) {
      r.skip(1);
      r.ue(16);
      r.ue(16);
      r.ue(16);
      r.ue(16);
      p.kept = r.position();
      p.reorder = r.ue(16);
      p.buffer = r.ue(16);
      if (p.reorder > p.buffer || p.buffer < p.refs)
        malformed();
    }
  }
  r.trailing();
  return p;
}
Bytes rewrite(View nal) {
  const auto p = parse(nal);
  if (p.restriction && p.reorder == 0 && p.buffer == p.refs)
    return {};
  Reader kept(p.rbsp);
  Writer w;
  for (auto left = p.kept; left;) {
    const unsigned count = unsigned(std::min<std::size_t>(left, 32));
    w.bits(kept.bits(count), count);
    left -= count;
  }
  if (!p.vui) {
    w.flag(true);
    w.bits(0, 8);
  }
  if (!p.restriction) {
    w.flag(true);
    w.flag(true);
    w.ue(2);
    w.ue(1);
    w.ue(16);
    w.ue(16);
  }
  w.ue(0);
  w.ue(p.refs);
  return escape(w.finish());
}
std::size_t start(View b, std::size_t i) {
  if (i + 3 <= b.size() && b[i] == 0 && b[i + 1] == 0) {
    if (b[i + 2] == 1)
      return 3;
    if (i + 4 <= b.size() && b[i + 2] == 0 && b[i + 3] == 1)
      return 4;
  }
  return 0;
}
} // namespace
bool rewrite_sps(Bytes &access_unit) {
  View data(access_unit);
  if (data.empty() || data.size() > 16 * 1024 * 1024)
    malformed();
  std::size_t at = 0, copied = 0, count = 0;
  Bytes out;
  bool changed = false;
  while (at < data.size() && !start(data, at)) {
    if (data[at++])
      malformed();
  }
  if (at == data.size())
    malformed();
  while (at < data.size()) {
    if (++count > 4096)
      malformed();
    const auto first = at + start(data, at);
    at = first;
    while (at < data.size() && !start(data, at))
      ++at;
    auto end = at;
    while (end > first && data[end - 1] == 0)
      --end;
    if (end == first || (data[first] & 0x80) || !(data[first] & 31) ||
        (data[first] & 31) >= 24)
      malformed();
    if ((data[first] & 31) != 7)
      continue;
    auto replacement = rewrite(data.subspan(first, end - first));
    if (replacement.empty())
      continue;
    out.insert(out.end(), data.begin() + copied, data.begin() + first);
    out.insert(out.end(), replacement.begin(), replacement.end());
    copied = end;
    changed = true;
  }
  if (changed) {
    out.insert(out.end(), data.begin() + copied, data.end());
    access_unit = std::move(out);
  }
  return changed;
}
} // namespace pbsd
