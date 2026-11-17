// SPDX-License-Identifier: GPL-3.0-or-later
// Local integration fixture only: real TLS, RFC 6455, UDP, MLS/DAVE and Opus.
#include "capture_bridge.h"
#include "external_sender.h"
#include "fixture.hpp"
#include "media.hpp"
#include "net.hpp"
#include "voice.hpp"
#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <openssl/sha.h>
#include <opus/opus.h>
#include <poll.h>
#include <chrono>
#include <iterator>
#include <map>
#include <optional>
#include <stdexcept>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

using namespace pbsd;
namespace dd = discord::dave;
namespace {
const Bytes pacing_sei_uuid{0x50, 0x42, 0x53, 0x44, 0x70, 0x61, 0x63, 0x69,
                            0x6e, 0x67, 0x74, 0x65, 0x73, 0x74, 0x30, 0x31};
void check(bool condition, const std::string &message) {
  if (!condition)
    throw std::runtime_error(message);
}
struct Socket {
  int fd{-1};
  explicit Socket(int type) {
    fd = ::socket(AF_INET, type, 0);
    check(fd >= 0, "socket");
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    check(::bind(fd, reinterpret_cast<sockaddr *>(&a), sizeof a) == 0, "bind");
  }
  ~Socket() {
    if (fd >= 0)
      ::close(fd);
  }
  unsigned port() const {
    sockaddr_in a{};
    socklen_t n = sizeof a;
    check(getsockname(fd, reinterpret_cast<sockaddr *>(&a), &n) == 0,
          "getsockname");
    return ntohs(a.sin_port);
  }
};
std::uint16_t u16(View b, std::size_t at) {
  return std::uint16_t((b[at] << 8) | b[at + 1]);
}
std::uint32_t u32(View b, std::size_t at) {
  return (std::uint32_t(u16(b, at)) << 16) | u16(b, at + 2);
}
// Fixture-side RFC 8285 decoding; never assume a fixed RTP payload offset.
std::size_t video_payload_offset(View p) {
  const std::size_t base = 12 + (p[0] & 15) * 4;
  check((p[0] & 0x10) && p.size() >= base + 4,
        "missing video RTP extensions");
  check(u16(p, base) == 0xbede, "video RTP extension profile");
  const auto end = base + 4 + 4 * std::size_t(u16(p, base + 2));
  check(end < p.size(), "truncated video RTP extensions");
  std::map<unsigned, Bytes> extensions;
  for (std::size_t at = base + 4; at < end;) {
    const auto element = p[at++];
    if (!element)
      continue;
    const unsigned id = element >> 4;
    const std::size_t length = (element & 15) + 1;
    check(id != 0 && id != 15 && at + length <= end &&
              !extensions.contains(id), "invalid video RTP extension element");
    extensions[id] = Bytes(p.begin() + at, p.begin() + at + length);
    at += length;
  }
  check(u16(p, base + 2) == 3 && extensions.size() == 3,
        "video RTP extension layout");
  check(extensions.contains(5) && extensions.at(5).size() == 2,
        "video transport sequence extension");
  check(extensions.contains(7) && extensions.at(7) == Bytes{1},
        "video screen content extension");
  check(extensions.contains(11) && extensions.at(11) == Bytes{'1', '0', '0'},
        "video RID extension");
  return end;
}
void write_bytes(std::ofstream &out, View b) {
  out.write(reinterpret_cast<const char *>(b.data()),
            static_cast<std::streamsize>(b.size()));
  check(out.good(), "output write failed");
}

// Server-side codec deliberately does not reuse the production WS codec.
class ServerWs {
  Tls tls_;
  Bytes buffer_;
  std::uint16_t sequence_{};

public:
  void accept(int fd, Context context) {
    tls_.accept(fd, std::move(context));
    std::string head;
    auto end = monotonic_ms() + 5000;
    while (head.find("\r\n\r\n") == std::string::npos) {
      auto part = tls_.read(8192, end);
      check(part.has_value(), "upgrade timeout");
      head.append(reinterpret_cast<const char *>(part->data()), part->size());
      check(head.size() < 32768, "upgrade too large");
    }
    auto at = head.find("Sec-WebSocket-Key:");
    check(at != std::string::npos, "missing websocket key");
    at += 18;
    while (head[at] == ' ')
      ++at;
    auto key = head.substr(at, head.find("\r\n", at) - at);
    auto proof = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    unsigned char digest[SHA_DIGEST_LENGTH];
    SHA1(reinterpret_cast<const unsigned char *>(proof.data()), proof.size(),
         digest);
    tls_.write("HTTP/1.1 101 Switching Protocols\r\nUpgrade: "
               "websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " +
                   base64(digest) + "\r\n\r\n",
               end);
    const auto body = head.find("\r\n\r\n") + 4;
    buffer_.assign(head.begin() + body, head.end());
  }
  void send(unsigned opcode, View payload) {
    check(payload.size() <= 65535, "fixture WS payload too large");
    Bytes b{std::uint8_t(0x80 | opcode)};
    if (payload.size() < 126)
      b.push_back(std::uint8_t(payload.size()));
    else {
      b.push_back(126);
      b.push_back(std::uint8_t(payload.size() >> 8));
      b.push_back(std::uint8_t(payload.size()));
    }
    b.insert(b.end(), payload.begin(), payload.end());
    tls_.write(b, monotonic_ms() + 3000);
  }
  void json(int opcode, const Json &data) {
    auto s = Json{{"op", opcode}, {"d", data}, {"seq", sequence_++}}.dump();
    send(1, {reinterpret_cast<const std::uint8_t *>(s.data()), s.size()});
  }
  void binary(unsigned opcode, View data) {
    Bytes b{std::uint8_t(sequence_ >> 8), std::uint8_t(sequence_),
            std::uint8_t(opcode)};
    ++sequence_;
    b.insert(b.end(), data.begin(), data.end());
    send(2, b);
  }
  std::optional<std::pair<bool, Bytes>> receive(std::uint64_t deadline) {
    for (;;) {
      if (buffer_.size() >= 2) {
        const unsigned opcode = buffer_[0] & 15;
        check((buffer_[0] & 0xf0) == 0x80, "fixture expects unfragmented WS");
        check(buffer_[1] & 0x80, "client WS is not masked");
        std::size_t n = buffer_[1] & 127, header = 2;
        if (n == 126) {
          if (buffer_.size() < 4)
            goto more;
          n = u16(buffer_, 2);
          header = 4;
        } else
          check(n != 127, "unexpected huge client WS frame");
        check(n <= 65535, "client WS too large");
        if (buffer_.size() < header + 4 + n)
          goto more;
        Bytes payload(n);
        for (std::size_t i = 0; i < n; ++i)
          payload[i] = buffer_[header + 4 + i] ^ buffer_[header + i % 4];
        buffer_.erase(buffer_.begin(), buffer_.begin() + header + 4 + n);
        if (opcode == 9) {
          send(10, payload);
          continue;
        }
        check(opcode == 1 || opcode == 2, "unexpected client WS opcode");
        return std::make_pair(opcode == 2, std::move(payload));
      }
    more:
      auto part = tls_.read(8192, deadline);
      if (!part)
        return std::nullopt;
      buffer_.insert(buffer_.end(), part->begin(), part->end());
      check(buffer_.size() < 131072, "client WS buffer too large");
    }
  }
};

struct Fixture {
  std::atomic<bool> quit{};
  std::thread thread;
  std::exception_ptr failure;
  explicit Fixture(const std::string &path)
      : thread([this, path] {
          try {
            run_fixture(path, 640, 360, 30, quit);
          } catch (...) {
            failure = std::current_exception();
          }
        }) {}
  ~Fixture() {
    quit = true;
    if (thread.joinable())
      thread.join();
  }
  void finish() {
    quit = true;
    if (thread.joinable())
      thread.join();
    if (failure)
      std::rethrow_exception(failure);
  }
};

// Producer that mimics the console capture bridge exactly as PBS drives it:
// configure() receives bare SPS/PPS NALs without start codes (the Avcap2
// GetNonVclNalUnitStream layout), video arrives as 3-byte Annex-B access
// units in 90 kHz wall-clock PTS, audio arrives as irregular f32 blocks, and
// a capture recovery replays discontinuity() followed by configure().
struct PbsFixture {
  std::atomic<bool> quit{};
  std::atomic<std::uint64_t> produced_frames{}, produced_bytes{};
  std::thread thread;
  std::exception_ptr failure;
  explicit PbsFixture(const std::string &path, bool pacing = false)
      : thread([this, path, pacing] {
          try {
            run(path, pacing);
          } catch (...) {
            failure = std::current_exception();
          }
        }) {}
  ~PbsFixture() {
    quit = true;
    if (thread.joinable())
      thread.join();
  }
  void finish() {
    quit = true;
    if (thread.joinable())
      thread.join();
    if (failure)
      std::rethrow_exception(failure);
  }
  void run(const std::string &path, bool pacing) {
    std::ifstream f(path, std::ios::binary);
    check(f.good(), "cannot open H264 fixture");
    Bytes data((std::istreambuf_iterator<char>(f)),
               std::istreambuf_iterator<char>());
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
      while (end + 3 < data.size() &&
             !(data[end] == 0 && data[end + 1] == 0 &&
               (data[end + 2] == 1 ||
                (end + 3 < data.size() && data[end + 2] == 0 &&
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
    check(frames.size() >= 60 && !sps.empty() && !pps.empty(),
          "fixture lacks AUD-delimited frames or parameter sets");
    if (pacing) {
      // Separate sustained producer: preserve every coded slice and parameter
      // set, and increase wire load using user_data_unregistered SEI NALs.
      // Real IDRs carry 250/350/450 KB bursts every two seconds; average
      // production remains near 4 Mbps, preserving every coded picture.
      check(h264_keyframe(frames.front()) && frames.size() == 60,
            "pacing fixture requires the two-second 30 fps clip starting at IDR");
      pbsd_capture_configure(640, 360, 60, sps.data(), sps.size(), pps.data(), pps.size());
      const auto start = std::chrono::steady_clock::now();
      for (std::uint64_t index = 0; !quit; ++index) {
        std::this_thread::sleep_until(start + std::chrono::microseconds(index * 1000000 / 60));
        if (quit) break;
        Bytes au = frames[index % frames.size()];
        const bool burst = index % 120 == 0;
        check(!burst || h264_keyframe(au), "pacing burst must be an actual IDR");
        const std::size_t target = burst ? 250000 + (index / 120 % 3) * 100000 : 5400;
        {
          // payloadSize counts the UUID and user bytes, and uses repeated 255
          // extension bytes. All payload bytes are nonzero, so no emulation
          // prevention bytes are needed in this test's SEI payload.
          const auto available = std::max<std::size_t>(
              target > au.size() ? target - au.size() : 0, 24);
          auto payload_bytes = available - 8;
          while (7 + payload_bytes / 255 + 1 + payload_bytes > available)
            --payload_bytes;
          check(payload_bytes >= pacing_sei_uuid.size(), "SEI UUID does not fit");
          Bytes sei{0, 0, 0, 1, 6, 5};
          auto remaining = payload_bytes;
          while (remaining >= 255) {
            sei.push_back(255);
            remaining -= 255;
          }
          sei.push_back(static_cast<std::uint8_t>(remaining));
          sei.insert(sei.end(), pacing_sei_uuid.begin(), pacing_sei_uuid.end());
          sei.insert(sei.end(), payload_bytes - pacing_sei_uuid.size(), 0xff);
          sei.push_back(0x80); // rbsp_stop_one_bit
          // SEI precedes the coded picture. A trailing SEI starts the next
          // access unit for a decoder and would leave an empty picture at EOF.
          au.insert(au.begin(), sei.begin(), sei.end());
        }
        pbsd_capture_video(au.data(), au.size(), static_cast<std::uint32_t>(index * 1500));
        produced_bytes += au.size();
        ++produced_frames;
        // Exactly 800 stereo samples per 60 fps video access unit.
        std::vector<float> audio(800 * 2);
        for (std::size_t i = 0; i < 800; ++i) {
          const float v = static_cast<float>(.08 * std::sin(
              2 * 3.141592653589793 * 440 * (index * 800 + i) / 48000));
          audio[i * 2] = audio[i * 2 + 1] = v;
        }
        pbsd_capture_audio(audio.data(), audio.size() * sizeof(float),
                           static_cast<std::uint32_t>(index * 800));
      }
      pbsd_capture_stopped();
      return;
    }
    auto configure = [&] {
      // Bare NALs, no start codes: the exact console bridge contract.
      pbsd_capture_configure(640, 360, 30, sps.data(), sps.size(), pps.data(),
                             pps.size());
    };
    configure();
    const auto start = std::chrono::steady_clock::now();
    std::uint64_t index = 0, samples = 0;
    for (int cycle = 0; cycle != 2 && !quit; ++cycle) {
      if (cycle == 1) {
        pbsd_capture_discontinuity();
        configure();
      }
      for (std::size_t i = 0; i < frames.size() && !quit; ++i) {
        std::this_thread::sleep_until(start +
                                      std::chrono::microseconds(index * 1000000 /
                                                                30));
        Bytes au = frames[i];
        // Avcap2 ReadVideo emits three-byte start codes.
        if (au.size() >= 4 && !au[0] && !au[1] && !au[2] && au[3] == 1)
          au.erase(au.begin());
        pbsd_capture_video(au.data(), au.size(),
                           static_cast<std::uint32_t>(index * 90000 / 30));
        std::uint64_t target = (index + 1) * 1600;
        std::vector<float> block;
        while (samples < target) {
          std::size_t take = static_cast<std::size_t>(
              std::min<std::uint64_t>(target - samples, 500 + samples % 400));
          block.resize(take * 2);
          for (std::size_t n = 0; n < take; ++n, ++samples) {
            float v = static_cast<float>(
                .08 * std::sin(2 * 3.141592653589793 * 440 * samples / 48000));
            block[n * 2] = block[n * 2 + 1] = v;
          }
          pbsd_capture_audio(block.data(), block.size() * sizeof(float),
                             static_cast<std::uint32_t>(samples));
        }
        ++index;
      }
    }
    pbsd_capture_stopped();
  }
};

class Receiver {
  // Replay windows are per SSRC: audio can overtake a fragmented video frame.
  std::unique_ptr<dd::IDecryptor> audio_decryptor_, video_decryptor_;
  std::unique_ptr<OpusDecoder, decltype(&opus_decoder_destroy)> opus_{
      nullptr, opus_decoder_destroy};
  std::ofstream video_, audio_;
  Bytes access_;
  bool fragment_{};
  bool have_seq_{};
  bool recovery_idr_{};
  std::uint16_t sequence_{};
  std::uint32_t timestamp_{};
  Bytes decrypt(dd::MediaType type, View data) {
    auto &decryptor = type == dd::Audio ? audio_decryptor_ : video_decryptor_;
    Bytes out(decryptor->GetMaxPlaintextByteSize(type, data.size()) + 64);
    std::size_t written = 0;
    auto result = decryptor->Decrypt(type, {data.data(), data.size()},
                                     {out.data(), out.size()}, &written);
    check(result == dd::IDecryptor::Success,
          "DAVE media authentication failed");
    check(written <= out.size(), "DAVE output overflow");
    out.resize(written);
    return out;
  }
  static bool idr(View b) {
    for (std::size_t i = 0; i + 4 < b.size(); ++i)
      if (b[i] == 0 && b[i + 1] == 0 &&
          ((b[i + 2] == 1 && (b[i + 3] & 31) == 5) ||
           (b[i + 2] == 0 && b[i + 3] == 1 && (b[i + 4] & 31) == 5)))
        return true;
    return false;
  }

public:
  std::uint64_t frames{}, video_packets{}, audio_packets{}, bytes{};
  std::uint64_t video_plain_bytes{}, pacing_sei_frames{}, max_pacing_idr_bytes{};
  std::uint64_t first_frame_ms{}, last_frame_ms{}, max_frame_gap_ms{};
  double energy{};
  void reset_timing() { first_frame_ms = last_frame_ms = max_frame_gap_ms = 0; }
  Receiver(const std::filesystem::path &directory)
      : video_(directory / "recovered.h264", std::ios::binary),
        audio_(directory / "recovered.f32", std::ios::binary) {
    check(video_.good() && audio_.good(), "cannot open recovered media");
    int err = 0;
    opus_.reset(opus_decoder_create(48000, 2, &err));
    check(err == OPUS_OK && opus_, "Opus decoder create failed");
  }
  void ratchet(dd::mls::ISession &session) {
    auto audio_key = session.GetKeyRatchet("1001"),
         video_key = session.GetKeyRatchet("1001");
    check(audio_key && video_key, "receiver missing sender ratchets");
    audio_decryptor_ = dd::CreateDecryptor();
    video_decryptor_ = dd::CreateDecryptor();
    check(audio_decryptor_ && video_decryptor_,
          "receiver decryptor create failed");
    audio_decryptor_->TransitionToKeyRatchet(std::move(audio_key));
    video_decryptor_->TransitionToKeyRatchet(std::move(video_key));
  }
  void packet(View packet, TransportCrypto &crypto) {
    check(packet.size() <= 1200, "oversized UDP datagram");
    check(packet.size() >= 8, "short UDP media");
    bytes += packet.size();
    if (packet[1] >= 192 && packet[1] <= 223) {
      check(crypto.open(packet, 8).has_value(),
            "sender report transport authentication failed");
      return;
    }
    check(audio_decryptor_ && video_decryptor_, "media sent before MLS commit");
    const auto type = packet[1] & 127;
    check(type == 101 || type == 120, "unexpected payload type");
    const std::size_t clear = type == 101 ? 16 : 12;
    auto opened = crypto.open(packet, clear);
    check(opened.has_value(), "RTP transport authentication failed");
    const auto &p = *opened;
    check((p[0] & 0xc0) == 0x80 && (p[0] & 15) == 0, "RTP header flags");
    if (type == 120) {
      check(!(p[0] & 0x10) && p.size() > 12, "audio RTP header");
      auto plain = decrypt(dd::Audio, View(p).subspan(12));
      float pcm[5760 * 2];
      int count = opus_decode_float(opus_.get(), plain.data(),
                                    static_cast<opus_int32>(plain.size()), pcm,
                                    5760, 0);
      check(count > 0, "Opus decode failed");
      for (int i = 0; i < count * 2; ++i) {
        check(std::isfinite(pcm[i]), "nonfinite decoded audio");
        energy += double(pcm[i]) * pcm[i];
      }
      write_bytes(audio_, {reinterpret_cast<const std::uint8_t *>(pcm),
                           std::size_t(count) * 2 * sizeof(float)});
      ++audio_packets;
      return;
    }
    ++video_packets;
    const auto payload_offset = video_payload_offset(p);
    check(u32(p, 8) == 4243, "video SSRC");
    const auto seq = u16(p, 2);
    const auto ts = u32(p, 4);
    if (have_seq_)
      check(seq == std::uint16_t(sequence_ + 1),
            "RTP sequence gap: expected " + std::to_string(std::uint16_t(sequence_ + 1)) +
            ", received " + std::to_string(seq));
    have_seq_ = true;
    sequence_ = seq;
    if (access_.empty())
      timestamp_ = ts;
    else if (ts != timestamp_) {
      // A capture discontinuity can discard the unsent end of a large AU.
      // Receivers abandon that incomplete AU and wait for the new IDR.
      access_.clear();
      fragment_ = false;
      recovery_idr_ = true;
      timestamp_ = ts;
    }
    const auto payload = View(p).subspan(payload_offset);
    const auto nal = payload[0] & 31;
    if (nal == 28) {
      check(payload.size() > 2, "short FU-A");
      const bool start = payload[1] & 0x80, end = payload[1] & 0x40;
      check(!(start && end), "invalid FU-A start/end");
      if (start) {
        check(!fragment_, "nested FU-A");
        fragment_ = true;
        access_.insert(access_.end(),
                       {0, 0, 0, 1,
                        std::uint8_t((payload[0] & 0xe0) | (payload[1] & 31))});
      } else
        check(fragment_, "FU-A continuation without start");
      access_.insert(access_.end(), payload.begin() + 2, payload.end());
      if (end)
        fragment_ = false;
    } else {
      check(nal > 0 && nal < 24 && !fragment_, "unsupported NAL packet");
      access_.insert(access_.end(), {0, 0, 0, 1});
      access_.insert(access_.end(), payload.begin(), payload.end());
    }
    check(access_.size() < 8 * 1024 * 1024, "reassembled frame overflow");
    if (p[1] & 0x80) {
      check(!fragment_, "frame marker within FU-A");
      auto plain = decrypt(dd::Video, access_);
      video_plain_bytes += plain.size();
      if (std::search(plain.begin(), plain.end(), pacing_sei_uuid.begin(),
                      pacing_sei_uuid.end()) != plain.end()) {
        ++pacing_sei_frames;
        if (idr(plain)) max_pacing_idr_bytes = std::max<std::uint64_t>(max_pacing_idr_bytes, plain.size());
      }
      if (!frames || recovery_idr_)
        check(idr(plain), "startup frame is not an IDR");
      recovery_idr_ = false;
      write_bytes(video_, plain);
      const auto completed = monotonic_ms();
      if (!first_frame_ms) first_frame_ms = completed;
      if (last_frame_ms)
        max_frame_gap_ms = std::max(max_frame_gap_ms, completed - last_frame_ms);
      last_frame_ms = completed;
      ++frames;
      access_.clear();
    }
  }
  void finish() {
    video_.flush();
    audio_.flush();
    check(video_.good() && audio_.good(), "recovered output flush failed");
  }
};
} // namespace

int main(int argc, char **argv) {
  try {
    check(argc == 3 || (argc == 4 && (std::string(argv[3]) == "pbs" ||
                                     std::string(argv[3]) == "pacing")),
          "usage: stream_test AUD-delimited-640x360-30fps.h264 "
          "output-directory [pbs|pacing]");
    const bool pbs_mode = argc == 4 && std::string(argv[3]) == "pbs";
    const bool pacing_mode = argc == 4 && std::string(argv[3]) == "pacing";
    std::filesystem::path output = argv[2];
    std::filesystem::create_directories(output);
    auto certdir = (output / "tls").string();
    std::string fingerprint;
    auto context = server_context(certdir, fingerprint);
    auto cert = certdir + "/server.crt";
    check(setenv("PBSD_TEST_CA", cert.c_str(), 1) == 0, "setenv failed");
    Socket tcp(SOCK_STREAM), udp(SOCK_DGRAM);
    check(::listen(tcp.fd, 1) == 0, "listen failed");
    std::optional<Fixture> fixture;
    std::optional<PbsFixture> pbs;
    if (pbs_mode || pacing_mode)
      pbs.emplace(argv[1], pacing_mode);
    else
      fixture.emplace(argv[1]);
    std::shared_ptr<mlspp::SignaturePrivateKey> sender_identity,
        receiver_identity;
    RtcParams params;
    params.server_id = "501";
    params.channel_id = "500";
    params.user_id = "1001";
    params.session_id = "offline-test-session";
    params.token = "offline-test-token";
    params.endpoint = "wss://localhost:" + std::to_string(tcp.port());
    params.members = {"1001", "1002"};
    params.stream = true;
    VoiceSession sender(params, sender_identity);
    pollfd listener{tcp.fd, POLLIN, 0};
    check(::poll(&listener, 1, 8000) > 0,
          "voice did not connect to local TLS fixture: " + sender.error());
    int accepted = ::accept(tcp.fd, nullptr, nullptr);
    check(accepted >= 0, "accept failed");
    ServerWs ws;
    ws.accept(accepted, context);
    dd::test::ExternalSender external(1, 500);
    auto peer = dd::mls::CreateSession(
        nullptr, "", [](const std::string &, const std::string &) {});
    check(peer != nullptr, "MLS peer create failed");
    peer->Init(1, 500, "1002", receiver_identity);
    peer->SetExternalSender(external.GetMarshalledExternalSender());
    auto receiver_key = peer->GetMarshalledKeyPackage();
    check(!receiver_key.empty(), "receiver key package missing");
    Bytes secret(32);
    for (std::size_t i = 0; i < secret.size(); ++i)
      secret[i] = std::uint8_t(i + 1);
    TransportCrypto transport("aead_xchacha20_poly1305_rtpsize", secret);
    Receiver receiver(output);
    ws.json(8, {{"heartbeat_interval", 500}});
    bool identified = false, selected = false, discovered = false,
         commit_sent = false, ready = false;
    std::uint64_t commit_after = 0, start_media = 0;
    std::uint64_t stable_start = 0, initial_drops = 0, initial_frames = 0,
                  initial_sender_frames = 0, initial_source_bytes = 0,
                  initial_video_bytes = 0, initial_sei_frames = 0;
    Bytes commit;
    const auto deadline = monotonic_ms() + 15000;
    while (monotonic_ms() < deadline) {
      if (auto message = ws.receive(monotonic_ms() + 2)) {
        if (message->first) {
          auto &b = message->second;
          check(!b.empty(), "empty client binary");
          if (b[0] == 26) {
            check(selected, "key before Session Description");
            check(!commit_sent, "duplicate key package");
            ws.binary(27, external.ProposeAdd(0, receiver_key));
          } else if (b[0] == 28) {
            auto split =
                external.SplitCommitWelcome(Bytes(b.begin() + 1, b.end()));
            commit = std::move(split.first);
            check(
                peer->ProcessWelcome(std::move(split.second), {"1001", "1002"})
                    .has_value(),
                "receiver welcome rejected");
            receiver.ratchet(*peer);
            commit_after = monotonic_ms() + 250;
          } else
            throw std::runtime_error("unexpected client binary opcode " +
                                     std::to_string(b[0]));
        } else {
          auto m = Json::parse(message->second);
          const int op = m.at("op");
          const auto &data = m["d"];
          if (op == 0) {
            check(data.at("user_id") == "1001" &&
                      data.at("server_id") == "501" &&
                      data.at("channel_id") == "500",
                  "Identify identity mismatch");
            check(data.contains("streams") && data["streams"].is_array() &&
                      !data["streams"].empty() &&
                      data["streams"][0].value("type", "") == "screen",
                  "Identify must advertise a screen stream");
            identified = true;
            Json allocation{{"ssrc", 4242},
                    {"ip", "127.0.0.1"},
                    {"port", udp.port()},
                    {"modes", Json::array({"aead_xchacha20_poly1305_rtpsize"})}};
            allocation["streams"] = Json::array({{{"ssrc", 4243}, {"rtx_ssrc", 4244}}});
            ws.json(2, allocation);
          } else if (op == 1) {
            check(identified && discovered, "SelectProtocol before discovery");
            check(data["data"]["mode"] == "aead_xchacha20_poly1305_rtpsize",
                  "wrong transport selection");
            bool h264 = false;
            for (const auto &codec : data.at("codecs"))
              if (codec.value("name", "") == "H264") {
                check(codec.at("payload_type") == 101 &&
                          codec.at("rtx_payload_type") == 102,
                      "incorrect H264 payload negotiation");
                h264 = true;
              }
            check(h264,
                  "SelectProtocol codec offer mismatch");
            selected = true;
            Json description{{"mode", "aead_xchacha20_poly1305_rtpsize"},
                        {"secret_key", secret},
                         {"dave_protocol_version", 1}};
            description["video_codec"] = "H264";
            ws.json(4, description);
            ws.json(11, {{"user_ids", Json::array({"1001", "1002"})}});
            ws.binary(25, external.GetMarshalledExternalSender());
          } else if (op == 3)
            ws.json(6, data);
          else if (op == 23) {
            check(commit_sent && data.at("transition_id") == 0,
                  "DAVE ready before commit");
            ready = true;
            start_media = monotonic_ms();
          } else
            check(op == 5 || op == 12,
                  "unexpected client JSON opcode " + std::to_string(op));
        }
      }
      for (unsigned i = 0; i < 64; ++i) {
        sockaddr_in remote{};
        socklen_t length = sizeof remote;
        std::uint8_t buffer[65536];
        auto n = recvfrom(udp.fd, buffer, sizeof buffer, MSG_DONTWAIT,
                          reinterpret_cast<sockaddr *>(&remote), &length);
        if (n < 0) {
          check(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR,
                "UDP receive failed");
          break;
        }
        View packet(buffer, static_cast<std::size_t>(n));
        if (!discovered) {
          check(n == 74 && packet[1] == 1 && u32(packet, 4) == 4242,
                "bad UDP discovery");
          Bytes response(packet.begin(), packet.end());
          response[1] = 2;
          std::fill(response.begin() + 8, response.end(), 0);
          std::string address = "127.0.0.1";
          std::copy(address.begin(), address.end(), response.begin() + 8);
          auto port = ntohs(remote.sin_port);
          response[72] = std::uint8_t(port >> 8);
          response[73] = std::uint8_t(port);
          check(sendto(udp.fd, response.data(), response.size(), 0,
                       reinterpret_cast<sockaddr *>(&remote), length) == 74,
                "discovery response failed");
          discovered = true;
        } else {
          check(commit_sent, "media emitted before MLS commit");
          receiver.packet(packet, transport);
        }
      }
      if (!commit.empty() && !commit_sent && monotonic_ms() >= commit_after) {
        check(capture_feed().info().running,
              "capture inactive during precommit media gate check");
        auto gated = sender.status();
        check(gated["video_packets"] == 0 && gated["audio_packets"] == 0 &&
                  !gated["dave_ready"].get<bool>(),
              "sender media gate opened before MLS commit");
        Bytes payload{0, 0};
        payload.insert(payload.end(), commit.begin(), commit.end());
        ws.binary(29, payload);
        commit_sent = true;
      }
      if (pacing_mode) {
        // DAVE activation flushes precommit media. Measure only after that
        // startup gate and warmup, then keep the 60 fps producer running.
        if (ready && !stable_start && monotonic_ms() - start_media >= 750 &&
            receiver.frames >= 30) {
          stable_start = monotonic_ms();
          initial_drops = capture_feed().dropped();
          initial_frames = receiver.frames;
          initial_sender_frames = sender.status().at("frames").get<std::uint64_t>();
          initial_source_bytes = pbs->produced_bytes.load();
          initial_video_bytes = receiver.video_plain_bytes;
          initial_sei_frames = receiver.pacing_sei_frames;
          receiver.max_pacing_idr_bytes = 0;
          receiver.reset_timing();
        }
        if (stable_start && monotonic_ms() - stable_start >= 5500) break;
      } else if (ready && ((receiver.frames >= 30 && receiver.audio_packets >= 20) ||
                           monotonic_ms() - start_media >= 4000))
        break;
      check(sender.error().empty(), "sender failed: " + sender.error());
    }
    auto status = sender.status();
    if (pacing_mode) {
      check(stable_start != 0, "pacing stable window never started");
      const auto stable_ms = monotonic_ms() - stable_start;
      const auto drops = capture_feed().dropped() - initial_drops;
      const auto recovered = receiver.frames - initial_frames;
      const auto span = receiver.last_frame_ms - receiver.first_frame_ms;
      const auto source_bytes = pbs->produced_bytes.load() - initial_source_bytes;
      const auto video_bytes = receiver.video_plain_bytes - initial_video_bytes;
      const auto sei_frames = receiver.pacing_sei_frames - initial_sei_frames;
      const auto observed_mbps = video_bytes * 8.0 / (stable_ms * 1000.0);
      Json metrics{{"stable_ms", stable_ms}, {"receiver_frames", recovered},
                   {"receiver_span_ms", span},
                   {"receiver_max_gap_ms", receiver.max_frame_gap_ms},
                   {"sender_frames", status.at("frames").get<std::uint64_t>() - initial_sender_frames},
                   {"startup_drops", initial_drops}, {"stable_drops", drops},
                   {"source_mbps", source_bytes * 8.0 / (stable_ms * 1000.0)},
                   {"observed_video_bytes", video_bytes}, {"observed_video_mbps", observed_mbps},
                   {"received_pacing_sei_frames", sei_frames},
                   {"max_pacing_idr_bytes", receiver.max_pacing_idr_bytes}};
      std::ofstream metrics_file(output / "pacing-metrics.json");
      metrics_file << metrics.dump(2) << '\n';
      check(metrics_file.good(), "cannot write pacing metrics");
      std::cout << "pacing metrics: " << metrics.dump() << '\n';
      check(recovered >= 300 && span >= 5000,
            "pacing did not sustain at least 300 frames over five seconds");
      check(drops == 0, "capture queue dropped frames during sustained pacing");
      check(receiver.max_frame_gap_ms < 200, "sustained receiver frame gap exceeds 200 ms");
      check(observed_mbps >= 3.3, "received video load was below 3.3 Mbps");
      check(sei_frames >= 300 && receiver.max_pacing_idr_bytes >= 449000,
            "receiver did not recover SEI padding and a 450 KB actual IDR");
    }
    check(ready, "DAVE transition never became ready: " + sender.error());
    check(receiver.frames >= 30,
          "fewer than 30 recovered video frames: " + sender.error());
    check(receiver.audio_packets >= 20 && receiver.energy > 0.001,
          "missing or silent recovered audio");
    check(status.at("dave_ready").get<bool>(), "sender DAVE status false");
    check(status.at("video_packets").get<std::uint64_t>() >=
                  receiver.video_packets &&
              status.at("audio_packets").get<std::uint64_t>() >=
                  receiver.audio_packets,
          "sender packet counters inconsistent");
    sender.stop();
    if (pbs)
      pbs->finish();
    else if (fixture)
      fixture->finish();
    receiver.finish();
    check(sender.error().empty(), "sender final error: " + sender.error());
    std::cout << "stream integration passed: " << receiver.frames
              << " video frames, " << receiver.audio_packets
              << " Opus packets, " << receiver.bytes << " wire bytes\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "stream integration failed: " << e.what() << '\n';
    return 1;
  }
}
