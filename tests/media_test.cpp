// SPDX-License-Identifier: GPL-3.0-or-later
#include "media.hpp"
#include <algorithm>
#include <cassert>
#include <iostream>
#include <map>
#include <stdexcept>
using namespace pbsd;
namespace {
struct Extensions {
  std::size_t payload;
  std::map<unsigned, Bytes> values;
};
// Decode RFC 8285 one-byte elements independently of the packet writer.
Extensions extensions(View packet) {
  assert(packet.size() >= 12 && (packet[0] & 0xc0) == 0x80);
  const std::size_t base = 12 + (packet[0] & 15) * 4;
  assert((packet[0] & 0x10) && packet.size() >= base + 4);
  assert(packet[base] == 0xbe && packet[base + 1] == 0xde);
  const auto words = (std::size_t(packet[base + 2]) << 8) | packet[base + 3];
  Extensions out{base + 4 + words * 4, {}};
  assert(out.payload < packet.size());
  for (std::size_t at = base + 4; at < out.payload;) {
    const auto element = packet[at++];
    if (!element)
      continue;
    const unsigned id = element >> 4;
    const std::size_t length = (element & 15) + 1;
    assert(id != 0 && id != 15 && at + length <= out.payload);
    assert(!out.values.contains(id));
    out.values[id] = Bytes(packet.begin() + at, packet.begin() + at + length);
    at += length;
  }
  return out;
}
void video_extensions(const Extensions &ext, unsigned rid,
                      std::uint16_t transport) {
  assert(ext.payload == 28 && ext.values.size() == 3);
  assert(ext.values.at(5) == (Bytes{std::uint8_t(transport >> 8),
                                    std::uint8_t(transport)}));
  assert(ext.values.at(7) == Bytes{1});
  assert(ext.values.at(rid) == (Bytes{'1', '0', '0'}));
  assert(!ext.values.contains(3));
}
void append(Bytes &a, const Bytes &b) { a.insert(a.end(), b.begin(), b.end()); }
void rejected(const Bytes &b) {
  bool threw = false;
  try {
    (void)normalize_h264(b);
  } catch (const std::invalid_argument &) {
    threw = true;
  }
  assert(threw);
  assert(!h264_keyframe(b));
}
void empty_feedback(const Bytes &b) {
  auto f = parse_rtcp(b, 42);
  assert(!f.keyframe && f.lost.empty() && !f.bitrate);
}
// An encrypted NAL may end in zeros or contain a zero before the next start
// code. Packetization must preserve every authenticated byte exactly.
void ciphertext_zeros_roundtrip(std::size_t size, std::size_t zeros,
                                bool following_nal) {
  Bytes frame(5 + size + zeros, 0);
  frame[3] = 1;
  frame[4] = 0x65;
  std::fill_n(frame.begin() + 5, size, 0x55);
  if (following_nal)
    append(frame, {0, 0, 0, 1, 0x41, 0x80});
  std::uint16_t seq = 0, transport = 0;
  const auto packets = packetize_h264(frame, 123, 42, seq, transport);
  Bytes recovered;
  bool fragmented = false;
  for (std::size_t i = 0; i < packets.size(); ++i) {
    const auto &packet = packets[i].bytes;
    assert(bool(packet[1] & 0x80) == (i + 1 == packets.size()));
    const auto payload = View(packet).subspan(extensions(packet).payload);
    if ((payload[0] & 31) == 28) {
      fragmented = true;
      if (payload[1] & 0x80)
        append(recovered, {0, 0, 0, 1,
                           std::uint8_t((payload[0] & 0xe0) |
                                        (payload[1] & 31))});
      recovered.insert(recovered.end(), payload.begin() + 2, payload.end());
    } else {
      append(recovered, {0, 0, 0, 1});
      recovered.insert(recovered.end(), payload.begin(), payload.end());
    }
  }
  assert(fragmented == (size == 3000));
  assert(recovered == frame);
}
} // namespace
int main() {
  for (const auto size : {8u, 3000u})
    for (const auto zeros : {1u, 2u, 3u})
      for (const auto following_nal : {false, true})
        ciphertext_zeros_roundtrip(size, zeros, following_nal);
  Bytes frame{0, 0, 0, 1, 0x09, 0xf0, 0, 0, 1, 0x65};
  frame.insert(frame.end(), 3000, 0x55);
  append(frame, {0, 0, 1, 0x0c, 0xff, 0x80});
  auto normalized = normalize_h264(frame);
  assert(normalized.size() == 3005 && normalized[4] == 0x65);
  assert(h264_keyframe(frame));
  std::uint16_t seq = 65535, tw = 65535;
  auto packets = packetize_h264(normalized, 0x12345678, 42, seq, tw);
  assert(packets.size() == 3 && seq == 2 && tw == 2);
  Bytes reassembled{0x65};
  for (std::size_t i = 0; i < packets.size(); ++i) {
    auto &p = packets[i];
    assert(p.bytes.size() + 20 <= 1200);
    assert(p.sequence == std::uint16_t(65535 + i));
    assert(p.bytes[0] == 0x90 && (p.bytes[1] & 127) == 101);
    assert(bool(p.bytes[1] & 128) == (i + 1 == packets.size()));
    const auto ext = extensions(p.bytes);
    video_extensions(ext, 11, std::uint16_t(65535 + i));
    const auto payload = View(p.bytes).subspan(ext.payload);
    assert(payload[0] == 0x7c && (payload[1] & 31) == 5);
    assert(bool(payload[1] & 0x80) == (i == 0));
    assert(bool(payload[1] & 0x40) == (i + 1 == packets.size()));
    reassembled.insert(reassembled.end(), payload.begin() + 2, payload.end());

    std::uint16_t repair_seq = 23, repair_transport = 900;
    const auto repair = retransmit_h264(p.bytes, 0x01020304, repair_seq,
                                        repair_transport);
    assert(repair.sequence == 23 && repair_seq == 24 && repair_transport == 901);
    assert(repair.bytes.size() + 20 <= 1200);
    assert((repair.bytes[1] & 127) == 102);
    assert((repair.bytes[1] & 128) == (p.bytes[1] & 128));
    assert(repair.bytes[2] == 0 && repair.bytes[3] == 23);
    assert(std::equal(p.bytes.begin() + 4, p.bytes.begin() + 8,
                      repair.bytes.begin() + 4));
    assert((Bytes(repair.bytes.begin() + 8, repair.bytes.begin() + 12) ==
            Bytes{1, 2, 3, 4}));
    const auto repaired_ext = extensions(repair.bytes);
    video_extensions(repaired_ext, 12, 900);
    const auto repaired_payload = View(repair.bytes).subspan(repaired_ext.payload);
    assert(repaired_payload.size() == payload.size() + 2);
    assert(repaired_payload[0] == p.bytes[2] && repaired_payload[1] == p.bytes[3]);
    assert(std::equal(payload.begin(), payload.end(), repaired_payload.begin() + 2));
  }
  assert(reassembled == Bytes(normalized.begin() + 4, normalized.end()));
  const Bytes small{0, 0, 1, 0x41, 0x80, 0, 0, 1, 0x06, 0x80};
  auto singles = packetize_h264(small, 0, 42, seq, tw);
  assert(singles.size() == 2 && singles[0].bytes[extensions(singles[0].bytes).payload] == 0x41 &&
         !(singles[0].bytes[1] & 128));
  assert(singles[1].bytes[extensions(singles[1].bytes).payload] == 0x06 && (singles[1].bytes[1] & 128));
  std::vector<Bytes> invalid_primary{Bytes{}, Bytes(12, 0), Bytes(16, 0)};
  auto wrong_payload = singles[0].bytes;
  wrong_payload[1] = 105;
  invalid_primary.push_back(wrong_payload);
  auto wrong_primary_version = singles[0].bytes;
  wrong_primary_version[0] = 0x50;
  invalid_primary.push_back(wrong_primary_version);
  auto bad_extension = singles[0].bytes;
  bad_extension[15] = 255;
  invalid_primary.push_back(bad_extension);
  auto no_payload = singles[0].bytes;
  no_payload.resize(extensions(no_payload).payload);
  invalid_primary.push_back(no_payload);
  auto oversized = singles[0].bytes;
  oversized.resize(1200, 0x55);
  invalid_primary.push_back(oversized);
  for (const auto &invalid : invalid_primary) {
    std::uint16_t repair_seq = 23, repair_transport = 900;
    bool rejected = false;
    try {
      (void)retransmit_h264(invalid, 42, repair_seq, repair_transport);
    } catch (const std::invalid_argument &) {
      rejected = true;
    }
    assert(rejected && repair_seq == 23 && repair_transport == 900);
  }
  assert(!h264_keyframe(small));
  for (const auto &b :
       {Bytes{}, Bytes{0, 0, 1}, Bytes{0x65, 0x80}, Bytes{0, 0, 1, 0xe5, 0x80},
        Bytes{0, 0, 1, 0x78, 0x80}, Bytes{0, 0, 1, 0x65, 0, 0, 1}})
    rejected(b);
  rejected(Bytes(16 * 1024 * 1024 + 1, 0));

  Bytes nack{0x81, 205, 0, 3, 0, 0, 0, 1, 0, 0, 0, 42, 0xff, 0xff, 0x80, 0x03};
  Bytes pli{0x81, 206, 0, 2, 0, 0, 0, 1, 0, 0, 0, 42};
  Bytes fir{0x84, 206, 0, 4, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 42, 1, 0, 0, 0};
  Bytes remb{0x8f, 206, 0,   5,   0, 0,    0,    1,    0, 0, 0, 0,
             'R',  'E', 'M', 'B', 1, 0x09, 0x86, 0xa0, 0, 0, 0, 42};
  auto compound = nack;
  append(compound, pli);
  append(compound, remb);
  auto f = parse_rtcp(compound, 42);
  assert(f.keyframe && f.lost == (std::vector<std::uint16_t>{65535, 0, 1, 15}));
  assert(f.bitrate == 400000);
  assert(parse_rtcp(fir, 42).keyframe);
  assert(!parse_rtcp(fir, 43).keyframe);
  assert(parse_rtcp(compound, 43).lost.empty() &&
         !parse_rtcp(compound, 43).bitrate);
  for (std::size_t n = 1; n < nack.size(); ++n)
    empty_feedback(Bytes(nack.begin(), nack.begin() + n));
  auto truncated = compound;
  truncated.pop_back();
  empty_feedback(truncated);
  auto trailing = nack;
  trailing.push_back(0);
  empty_feedback(trailing);
  auto wrong_version = nack;
  wrong_version[0] = 1;
  empty_feedback(wrong_version);
  auto bad_count = remb;
  bad_count[16] = 2;
  empty_feedback(bad_count);
  auto overflow = remb;
  overflow[17] = 0xff;
  empty_feedback(overflow);
  auto padded = pli;
  padded[0] |= 0x20;
  padded[3] = 3;
  append(padded, {0, 0, 0, 4});
  assert(parse_rtcp(padded, 42).keyframe);
  padded.back() = 0;
  empty_feedback(padded);
  auto malformed_fir = fir;
  malformed_fir.resize(16);
  malformed_fir[3] = 3;
  empty_feedback(malformed_fir);
  auto malformed_pli = pli;
  malformed_pli.resize(16);
  malformed_pli[3] = 3;
  empty_feedback(malformed_pli);

  auto audio = make_audio_rtp(Bytes{0xf8, 0xff, 0xfe}, 123, 42, 65535);
  assert(audio.size() == 15 && audio[0] == 0x80 && audio[1] == 120 &&
         audio[2] == 255 && audio[3] == 255);
  bool audio_rejected = false;
  try {
    (void)make_audio_rtp(Bytes(1169, 1), 0, 0, 0);
  } catch (const std::invalid_argument &) {
    audio_rejected = true;
  }
  assert(audio_rejected);
  auto sr = make_sender_report(42, 123, 7, 8, 1500);
  assert(sr.size() == 28 && sr[1] == 200 && sr[3] == 6);
  assert(sr[8] == 0x83 && sr[9] == 0xaa && sr[10] == 0x7e && sr[11] == 0x81);
  assert(sr[12] == 0x80 && sr[13] == 0 && sr[14] == 0 && sr[15] == 0);
  assert(sr[19] == 123 && sr[23] == 7 && sr[27] == 8);
  std::cout << "media tests passed\n";
}
