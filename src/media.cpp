// SPDX-License-Identifier: GPL-3.0-or-later
#include "media.hpp"
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace pbsd {
namespace {
constexpr std::size_t max_frame = 16 * 1024 * 1024;
constexpr std::size_t max_payload =
    1200 - 20 - video_rtp_header_size - 2; // Reserve RTX original-sequence bytes.
void put16(Bytes &b, std::uint16_t n) {
  b.push_back(std::uint8_t(n >> 8));
  b.push_back(std::uint8_t(n & 255));
}
void put32(Bytes &b, std::uint32_t n) {
  put16(b, std::uint16_t(n >> 16));
  put16(b, std::uint16_t(n & 65535));
}
std::uint16_t get16(View b, std::size_t i) {
  return std::uint16_t((std::uint16_t(b[i]) << 8) | b[i + 1]);
}
std::uint32_t get32(View b, std::size_t i) {
  return (std::uint32_t(get16(b, i)) << 16) | get16(b, i + 2);
}
[[noreturn]] void bad_frame() {
  throw std::invalid_argument("invalid or oversized Annex B H.264 access unit");
}
std::size_t start_code(View b, std::size_t i) {
  if (i + 3 <= b.size() && b[i] == 0 && b[i + 1] == 0) {
    if (b[i + 2] == 1)
      return 3;
    if (i + 4 <= b.size() && b[i + 2] == 0 && b[i + 3] == 1)
      return 4;
  }
  return 0;
}
std::vector<View> nals(View b, bool strip_trailing_zeros = true) {
  if (b.empty() || b.size() > max_frame)
    bad_frame();
  std::size_t at = 0;
  while (at < b.size() && !start_code(b, at)) {
    if (b[at++] != 0)
      bad_frame();
  }
  if (at == b.size())
    bad_frame();
  std::vector<View> result;
  while (at < b.size()) {
    auto first = at + start_code(b, at);
    at = first;
    while (at < b.size() && !start_code(b, at))
      ++at;
    auto end = at;
    while (strip_trailing_zeros && end > first && b[end - 1] == 0)
      --end; // Annex B trailing_zero_8bits
    if (end == first || (b[first] & 0x80) || (b[first] & 31) == 0 ||
        (b[first] & 31) >= 24)
      bad_frame();
    if (result.size() >= 4096)
      bad_frame();
    result.push_back(b.subspan(first, end - first));
  }
  return result;
}
Bytes header(std::uint8_t pt, std::uint32_t ts, std::uint32_t ssrc,
             std::uint16_t seq, bool ext) {
  Bytes b{std::uint8_t(ext ? 0x90 : 0x80), pt};
  put16(b, seq);
  put32(b, ts);
  put32(b, ssrc);
  return b;
}
Bytes video_header(std::uint8_t pt, bool marker, std::uint32_t timestamp,
                   std::uint32_t ssrc, std::uint16_t sequence,
                   std::uint16_t &transport_sequence, unsigned rid_id) {
  auto b = header(std::uint8_t(pt | (marker ? 0x80 : 0)), timestamp, ssrc,
                  sequence, true);
  put16(b, 0xbede);
  put16(b, 3);
  // The native UDP extension IDs differ from the WebRTC SDP path.
  b.push_back(0x51); // id 5: two-byte transport-wide sequence number
  put16(b, transport_sequence++);
  b.insert(b.end(), {0x70, 1, // id 7: screen content
                    std::uint8_t((rid_id << 4) | 2), '1', '0', '0'});
  b.resize(video_rtp_header_size, 0);
  return b;
}
} // namespace

Bytes normalize_h264(View frame) {
  Bytes out;
  for (auto nal : nals(frame)) {
    const auto type = nal[0] & 31;
    if (type == 9 || type == 12)
      continue;
    // SPS VUI is preserved. Rewriting requires a full bounded SPS parser.
    if (nal.size() + 4 > max_frame - out.size())
      bad_frame();
    out.insert(out.end(), {0, 0, 0, 1});
    out.insert(out.end(), nal.begin(), nal.end());
  }
  return out;
}
bool h264_keyframe(View frame) {
  try {
    const auto units = nals(frame);
    return std::any_of(units.begin(), units.end(),
                       [](View n) { return (n[0] & 31) == 5; });
  } catch (const std::invalid_argument &) {
    return false;
  }
}
std::vector<RtpPacket> packetize_h264(View frame, std::uint32_t timestamp,
                                      std::uint32_t ssrc,
                                      std::uint16_t &sequence,
                                      std::uint16_t &transport_sequence) {
  // Input may already carry DAVE authentication. Never normalize, discard,
  // or rewrite NALs after encryption; only RFC 6184 packet framing belongs
  // here.
  if (frame.empty())
    return {};
  // libdave emits canonical four-byte start codes. A zero before one of
  // those codes (or at frame EOF) may be authenticated ciphertext, so retain
  // it rather than treating it as plaintext Annex B trailing_zero_8bits.
  const auto units = nals(frame, false);
  std::vector<RtpPacket> out;
  auto emit = [&](View payload, bool marker) {
    auto b = video_header(h264_payload_type, marker, timestamp, ssrc, sequence,
                          transport_sequence, 11);
    b.insert(b.end(), payload.begin(), payload.end());
    out.push_back({std::move(b), sequence++});
  };
  for (std::size_t i = 0; i < units.size(); ++i) {
    const auto nal = units[i];
    if (nal.size() <= max_payload) {
      emit(nal, i + 1 == units.size());
      continue;
    }
    for (std::size_t at = 1; at < nal.size();) {
      const auto count = std::min(max_payload - 2, nal.size() - at);
      const bool last = at + count == nal.size();
      Bytes fragment{std::uint8_t((nal[0] & 0xe0) | 28),
                     std::uint8_t((nal[0] & 31) | (at == 1 ? 0x80 : 0) |
                                  (last ? 0x40 : 0))};
      fragment.insert(fragment.end(), nal.begin() + at,
                      nal.begin() + at + count);
      emit(fragment, last && i + 1 == units.size());
      at += count;
    }
  }
  return out;
}

RtpPacket retransmit_h264(View original, std::uint32_t rtx_ssrc,
                          std::uint16_t &sequence,
                          std::uint16_t &transport_sequence) {
  if (original.size() <= video_rtp_header_size || original.size() > 1178 ||
      original[0] != 0x90 || (original[1] & 127) != h264_payload_type ||
      get16(original, 12) != 0xbede || get16(original, 14) != 3 ||
      original[16] != 0x51 || original[19] != 0x70 || original[20] != 1 ||
      original[21] != 0xb2 || original[22] != '1' || original[23] != '0' ||
      original[24] != '0' || !rtx_ssrc)
    throw std::invalid_argument("invalid H.264 retransmission source");
  auto b = video_header(h264_rtx_payload_type, original[1] & 0x80,
                        get32(original, 4), rtx_ssrc, sequence,
                        transport_sequence, 12);
  put16(b, get16(original, 2));
  b.insert(b.end(), original.begin() + video_rtp_header_size, original.end());
  return {std::move(b), sequence++};
}

Feedback parse_rtcp(View plain, std::uint32_t media_ssrc) {
  Feedback result;
  if (plain.size() > 1024 * 1024)
    return {};
  for (std::size_t at = 0; at < plain.size();) {
    if (plain.size() - at < 4)
      return {};
    const auto p = plain.subspan(at);
    const std::size_t wire_size = (std::size_t(get16(p, 2)) + 1) * 4;
    if ((p[0] >> 6) != 2 || wire_size > p.size() || p[1] < 192 || p[1] > 223)
      return {};
    auto size = wire_size;
    if (p[0] & 0x20) {
      if (at + wire_size != plain.size())
        return {};
      const auto padding = p[wire_size - 1];
      if (!padding || padding > wire_size - 4)
        return {};
      size -= padding;
    }
    const auto fmt = p[0] & 31;
    const auto type = p[1];
    if (type == 200 && size < 28u + 24u * fmt)
      return {};
    if (type == 201 && size < 8u + 24u * fmt)
      return {};
    if (type == 205 || type == 206) {
      if (size < 12)
        return {};
      const bool matches = get32(p, 8) == media_ssrc;
      if (type == 205 && fmt == 1) {
        if (size < 16 || (size - 12) % 4)
          return {};
        for (std::size_t j = 12; j < size; j += 4) {
          const auto pid = get16(p, j), mask = get16(p, j + 2);
          if (!matches)
            continue;
          result.lost.push_back(pid);
          for (unsigned bit = 0; bit < 16; ++bit)
            if (mask & (1u << bit))
              result.lost.push_back(std::uint16_t(pid + bit + 1));
        }
      } else if (type == 206 && fmt == 1) {
        if (size != 12)
          return {};
        result.keyframe |= matches;
      } else if (type == 206 && fmt == 4) {
        if (size < 20 || (size - 12) % 8)
          return {};
        for (std::size_t j = 12; j < size; j += 8)
          result.keyframe |= get32(p, j) == media_ssrc;
      } else if (type == 206 && fmt == 15 && size >= 16 && p[12] == 'R' &&
                 p[13] == 'E' && p[14] == 'M' && p[15] == 'B') {
        if (size < 20 || size != 20u + 4u * p[16])
          return {};
        bool target = false;
        for (std::size_t j = 20; j < size; j += 4)
          target |= get32(p, j) == media_ssrc;
        const unsigned exponent = p[17] >> 2;
        const std::uint64_t mantissa =
            (std::uint64_t(p[17] & 3) << 16) | get16(p, 18);
        if (mantissa > (std::numeric_limits<std::uint64_t>::max() >> exponent))
          return {};
        if (target)
          result.bitrate = mantissa << exponent;
      }
    }
    at += wire_size;
  }
  return result;
}
Bytes make_audio_rtp(View opus, std::uint32_t timestamp, std::uint32_t ssrc,
                     std::uint16_t sequence) {
  if (opus.empty() || opus.size() > 1200 - 20 - 12)
    throw std::invalid_argument("invalid or oversized Opus packet");
  auto b = header(120, timestamp, ssrc, sequence, false);
  b.insert(b.end(), opus.begin(), opus.end());
  return b;
}
Bytes make_sender_report(std::uint32_t ssrc, std::uint32_t timestamp,
                         std::uint32_t packets, std::uint32_t octets,
                         std::uint64_t unix_ms) {
  Bytes b{0x80, 200, 0, 6};
  put32(b, ssrc);
  put32(b, std::uint32_t(unix_ms / 1000 + 2208988800ULL));
  put32(b, std::uint32_t(((unix_ms % 1000) << 32) / 1000));
  put32(b, timestamp);
  put32(b, packets);
  put32(b, octets);
  return b;
}
} // namespace pbsd
