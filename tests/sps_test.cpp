// SPDX-License-Identifier: GPL-3.0-or-later
#include "sps.hpp"
#include <bit>
#include <cassert>
#include <iostream>
#include <stdexcept>
#include <string_view>
using namespace pbsd;
namespace {
// Independent fixture writer: all VUI fields have nontrivial values so full
// byte equality against the desired SPS verifies their preservation.
struct Bits {
  Bytes b;
  unsigned used{};
  void put(std::uint64_t n, unsigned count) {
    while (count) {
      --count;
      if (used % 8 == 0)
        b.push_back(0);
      b.back() |= ((n >> count) & 1) << (7 - used % 8);
      ++used;
    }
  }
  void ue(std::uint32_t n) {
    const auto code = std::uint64_t(n) + 1;
    unsigned bits = std::bit_width(code);
    put(0, bits - 1);
    put(code, bits);
  }
  Bytes finish() {
    put(1, 1);
    Bytes out{0, 0, 0, 1};
    unsigned zeros = 0;
    for (auto byte : b) {
      if (zeros == 2 && byte <= 3) {
        out.push_back(3);
        zeros = 0;
      }
      out.push_back(byte);
      zeros = byte == 0 ? zeros + 1 : 0;
    }
    return out;
  }
};
void prefix(Bits &w, bool high = false) {
  w.put(0x67, 8);
  w.put(high ? 100 : 66, 8);
  w.put(0, 8);
  w.put(31, 8);
  w.ue(0);
  if (high) {
    w.ue(1);
    w.ue(0);
    w.ue(0);
    w.put(0, 1);
    w.put(1, 1);
    // Exercise scaling_list with all eight lists present (delta_scale = 0).
    for (unsigned i = 0; i < 8; ++i) {
      w.put(1, 1);
      for (unsigned j = 0; j < (i < 6 ? 16u : 64u); ++j)
        w.ue(0);
    }
  }
  w.ue(0);
  w.ue(0);
  w.ue(0);
  w.ue(4);
  w.put(0, 1);
  w.ue(79);
  w.ue(44);
  w.put(1, 1);
  w.put(1, 1);
  w.put(1, 1);
  w.ue(0);
  w.ue(0);
  w.ue(0);
  w.ue(4);
}
void vui_fields(Bits &w) {
  w.put(1, 1);
  w.put(255, 8);
  w.put(4, 16);
  w.put(3, 16); // extended SAR
  w.put(1, 1);
  w.put(1, 1); // overscan
  w.put(1, 1);
  w.put(5, 3);
  w.put(1, 1);
  w.put(1, 1);
  w.put(9, 8);
  w.put(16, 8);
  w.put(9, 8); // BT.2020/PQ colors
  w.put(1, 1);
  w.ue(2);
  w.ue(3); // chroma locations
  w.put(1, 1);
  w.put(1001, 32);
  w.put(60000, 32);
  w.put(1, 1);
  // Both HRD branches, including low_delay_hrd_flag and pic_struct_present.
  for (unsigned i = 0; i < 2; ++i) {
    w.put(1, 1);
    w.ue(0);
    w.put(0x12, 8);
    w.ue(127);
    w.ue(255);
    w.put(1, 1);
    w.put(0xabcde, 20);
  }
  w.put(1, 1);
  w.put(1, 1);
}
void restriction(Bits &w, unsigned reorder, unsigned buffer,
                 bool defaults = false) {
  w.put(1, 1);
  w.put(1, 1);
  w.ue(defaults ? 2 : 3);
  w.ue(defaults ? 1 : 2);
  w.ue(defaults ? 16 : 14);
  w.ue(defaults ? 16 : 13);
  w.ue(reorder);
  w.ue(buffer);
}
Bytes fixture(bool vui, bool restrict, unsigned reorder = 2,
              unsigned buffer = 6, bool high = false) {
  Bits w;
  prefix(w, high);
  w.put(vui, 1);
  if (vui) {
    vui_fields(w);
    if (restrict)
      restriction(w, reorder, buffer);
    else
      w.put(0, 1);
  }
  return w.finish();
}
Bytes expected_new(bool existing_vui, bool high = false) {
  Bits w;
  prefix(w, high);
  w.put(1, 1);
  if (existing_vui)
    vui_fields(w);
  else
    w.put(0, 8);
  restriction(w, 0, 4, true);
  return w.finish();
}
void rejects(Bytes data) {
  const auto original = data;
  bool threw = false;
  try {
    rewrite_sps(data);
  } catch (const std::invalid_argument &) {
    threw = true;
  }
  assert(threw && data == original);
}
Bytes hex(std::string_view text) {
  Bytes out{0, 0, 0, 1};
  auto digit = [](char c) {
    return unsigned(c <= '9' ? c - '0' : c - 'a' + 10);
  };
  for (std::size_t i = 0; i < text.size(); i += 2)
    out.push_back((digit(text[i]) << 4) | digit(text[i + 1]));
  return out;
}
void encoder_fixtures() {
  // Acheron tst_H264Sps.cpp fixtures: FFmpeg 8.1 h264_amf and h264_mf.
  // Expected bytes were independently checked with FFmpeg trace_headers there.
  for (auto pair :
       {std::pair{
            "6764042aace50a11bff00010001420404028000003000800000301e4784422cb",
            "6764042aace50a11bff00010001420404028000003000800000301e47844232c"},
        std::pair{"6764000bac2cac20da10000003001000000303c0da08845380",
                  "6764000bac2cac20da10000003001000000303c0da08846e"},
        std::pair{"6742c00b95a08364", "6742c00b95a0836806d0442350"}}) {
    auto source = hex(pair.first);
    assert(rewrite_sps(source));
    assert(source == hex(pair.second));
    assert(!rewrite_sps(source));
  }
  for (auto text :
       {"6764042aacb28508dff80008000a10202014000003000400000300f23c221196",
        "6742c0148c6820d30101e1108d40", "6742c00b8c6824f622230101e1108d40"}) {
    auto already = hex(text);
    assert(!rewrite_sps(already));
    assert(already == hex(text));
  }
}
void valid_and_preserved() {
  for (bool high : {false, true}) {
    auto no_vui = fixture(false, false, 2, 6, high);
    assert(rewrite_sps(no_vui));
    assert(no_vui == expected_new(false, high));
    assert(!rewrite_sps(no_vui));
    auto no_restriction = fixture(true, false, 2, 6, high);
    assert(rewrite_sps(no_restriction));
    assert(no_restriction == expected_new(true, high));
    assert(!rewrite_sps(no_restriction));
    auto existing = fixture(true, true, 2, 6, high);
    const auto desired = fixture(true, true, 0, 4, high);
    assert(rewrite_sps(existing));
    assert(existing == desired);
    assert(!rewrite_sps(existing));
    auto excess = fixture(true, true, 0, 6, high);
    assert(rewrite_sps(excess));
    assert(excess == desired);
  }
  Bytes no_sps{0, 0, 1, 0x65, 0x88};
  const auto original = no_sps;
  assert(!rewrite_sps(no_sps) && no_sps == original);
  auto first = fixture(false, false);
  auto second = fixture(true, false);
  Bytes multi{0, 0, 1, 0x09, 0x10};
  multi.insert(multi.end(), first.begin(), first.end());
  multi.insert(multi.end(), second.begin(), second.end());
  Bytes expected{0, 0, 1, 0x09, 0x10};
  auto a = expected_new(false), b = expected_new(true);
  expected.insert(expected.end(), a.begin(), a.end());
  expected.insert(expected.end(), b.begin(), b.end());
  assert(rewrite_sps(multi) && multi == expected && !rewrite_sps(multi));
}
void malformed_inputs() {
  const auto complete = fixture(true, true);
  for (std::size_t i = 0; i < complete.size(); ++i)
    rejects(Bytes(complete.begin(), complete.begin() + i));
  auto stop = fixture(false, false);
  stop.back() |= 1;
  rejects(stop);
  rejects({1, 2, 3});
  rejects({0, 0, 0, 1});
  rejects({0, 0, 0, 1, 0xe7, 1});
  rejects({0, 0, 0, 1, 0x67, 0, 0, 3});
  rejects({0, 0, 0, 1, 0x67, 0, 0, 3, 4});
  Bits ue_overflow;
  ue_overflow.put(0x6742001f, 32);
  ue_overflow.put(0, 32);
  ue_overflow.put(1, 1);
  rejects(ue_overflow.finish());
  Bits id_bound;
  id_bound.put(0x6742001f, 32);
  id_bound.ue(32);
  rejects(id_bound.finish());
  Bits chroma;
  chroma.put(0x6764001f, 32);
  chroma.ue(0);
  chroma.ue(4);
  rejects(chroma.finish());
  Bits poc;
  poc.put(0x6742001f, 32);
  poc.ue(0);
  poc.ue(0);
  poc.ue(3);
  rejects(poc.finish());
  rejects(fixture(true, true, 7, 6));
  rejects(fixture(true, true, 0, 3));
  Bytes too_large(4100, 0x55);
  too_large[0] = too_large[1] = too_large[2] = 0;
  too_large[3] = 1;
  too_large[4] = 0x67;
  rejects(too_large);
  // A later malformed SPS must not commit an earlier successful rewrite.
  auto atomic = fixture(false, false);
  atomic.insert(atomic.end(), {0, 0, 0, 1, 0x67, 0x42});
  rejects(atomic);
}
} // namespace
int main() {
  valid_and_preserved();
  encoder_fixtures();
  malformed_inputs();
  std::cout << "SPS tests passed\n";
}
