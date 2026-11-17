// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "types.hpp"
#include <optional>
namespace pbsd {
inline constexpr std::uint8_t h264_payload_type = 101;
inline constexpr std::uint8_t h264_rtx_payload_type = 102;
inline constexpr std::size_t video_rtp_header_size = 28;
struct RtpPacket {
  Bytes bytes;
  std::uint16_t sequence{};
};
// The returned RTP header is 16 bytes clear; extension content follows it and
// is encrypted with the payload. Entire datagrams including crypto fit 1200 B.
std::vector<RtpPacket> packetize_h264(View frame, std::uint32_t timestamp,
                                      std::uint32_t ssrc,
                                      std::uint16_t &sequence,
                                      std::uint16_t &transport_sequence);
RtpPacket retransmit_h264(View original, std::uint32_t rtx_ssrc,
                          std::uint16_t &sequence,
                          std::uint16_t &transport_sequence);
bool h264_keyframe(View frame);
Bytes normalize_h264(View frame);
struct Feedback {
  bool keyframe{};
  std::vector<std::uint16_t> lost;
  std::optional<std::uint64_t> bitrate;
};
Feedback parse_rtcp(View plain, std::uint32_t media_ssrc);
Bytes make_audio_rtp(View opus, std::uint32_t timestamp, std::uint32_t ssrc,
                     std::uint16_t sequence);
Bytes make_sender_report(std::uint32_t ssrc, std::uint32_t timestamp,
                         std::uint32_t packets, std::uint32_t octets,
                         std::uint64_t unix_ms);
} // namespace pbsd
