// SPDX-License-Identifier: GPL-3.0-or-later
// Protocol and sender behavior informed by Acheron and Yapfast. See
// THIRD_PARTY.md.
#include "voice.hpp"
#include "media.hpp"
#include "net.hpp"
#include "pacer.hpp"
#include "sps.hpp"
#include <algorithm>
#include <arpa/inet.h>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <map>
#include <netinet/in.h>
#include <opus/opus.h>
#include <stdexcept>
#include <sys/socket.h>
namespace pbsd {
static std::uint64_t microseconds() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
static std::uint64_t unix_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}
static std::uint32_t read32(View b, std::size_t p) {
  return (std::uint32_t(b[p]) << 24) | (std::uint32_t(b[p + 1]) << 16) |
         (std::uint32_t(b[p + 2]) << 8) | b[p + 3];
}
static void write32(Bytes &b, std::size_t p, std::uint32_t n) {
  for (int i = 0; i < 4; ++i)
    b[p + i] = n >> (24 - i * 8);
}
VoiceSession::VoiceSession(RtcParams p,
                           std::shared_ptr<mlspp::SignaturePrivateKey> &id)
    : params_(std::move(p)), identity_(id) {
  thread_ = std::thread(&VoiceSession::run, this);
}
VoiceSession::~VoiceSession() {
  stop();
  secure_erase(params_.token.data(), params_.token.size());
}
void VoiceSession::stop() {
  stop_ = true;
  if (thread_.joinable())
    thread_.join();
}
std::string VoiceSession::error() const {
  std::lock_guard lock(error_mutex_);
  return error_;
}
Json VoiceSession::status() const {
  auto c = capture_feed().info();
  return {{"connected", connected_.load()},
          {"dave_ready", dave_ready_.load()},
          {"video_packets", video_packets_.load()},
          {"audio_packets", audio_packets_.load()},
          {"bytes", bytes_.load()},
          {"frames", frames_.load()},
          {"repaired_packets", repaired_.load()},
          {"video_pacing_bps", pacing_bitrate_.load()},
          {"max_frame_send_ms", max_frame_send_us_.load() / 1000.0},
          {"max_frame_bytes", max_frame_bytes_.load()},
          {"max_frame_prepare_ms", max_frame_prepare_us_.load() / 1000.0},
          {"width", c.width},
          {"height", c.height},
          {"fps", c.fps}};
}
void VoiceSession::run() {
  try {
    const auto endpoint = media_gateway_endpoint(params_.endpoint);
    WebSocket ws;
    ws.connect(endpoint, "https://discord.com", 65536);
    auto send_json = [&](int op, const Json &d) {
      ws.text(Json{{"op", op}, {"d", d}}.dump());
    };
    auto send_binary = [&](int op, View b) {
      Bytes p{static_cast<std::uint8_t>(op)};
      p.insert(p.end(), b.begin(), b.end());
      ws.binary(p);
    };
    auto group = std::stoull(params_.channel_id);
    Dave dave(group, params_.user_id, identity_, send_json, send_binary);
    for (auto &id : params_.members)
      dave.member(id);
    Udp udp;
    std::unique_ptr<TransportCrypto> crypto;
    std::uint32_t audio_ssrc = 0, video_ssrc = 0, rtx_ssrc = 0;
    std::uint16_t video_seq = 0, audio_seq = 0, rtx_seq = 0, transport_seq = 0;
    auto random = random_bytes(8);
    video_seq = (random[0] << 8) | random[1];
    audio_seq = (random[2] << 8) | random[3];
    rtx_seq = (random[4] << 8) | random[5];
    std::uint64_t heartbeat_interval = 0, next_heartbeat = 0,
                  connect_deadline = monotonic_ms() + 30000;
    bool heartbeat_ack = true, discovery = false, described = false,
         announced = false;
    std::int64_t seq_ack = -1;
    std::uint64_t discovery_start = 0, last_report = 0, epoch = 0,
                  generation = 0, media_start = 0, media_unix = 0,
                  video_octets = 0, audio_octets = 0;
    std::uint32_t source_base = 0, video_base = 0, audio_timestamp = 0;
    std::deque<RtpPacket> pending;
    std::map<std::uint16_t, Bytes> cache;
    std::deque<std::uint16_t> cache_order;
    VideoPacer pacer;
    pacer.set_target_bitrate(4000000);
    pacing_bitrate_ = pacer.bitrate();
    std::uint64_t pending_frame_started_us = 0;
    std::vector<float> audio_buffer;
    std::uint64_t next_audio = 0;
    bool waiting_keyframe = true;
    std::unique_ptr<OpusEncoder, decltype(&opus_encoder_destroy)> encoder(
        nullptr, opus_encoder_destroy);
    if (params_.stream
    ) {
      int error;
      encoder.reset(
          opus_encoder_create(48000, params_.stream ? 2 : 1,
                              params_.stream ? OPUS_APPLICATION_AUDIO : OPUS_APPLICATION_VOIP, &error));
      if (error != OPUS_OK || !encoder)
        throw std::runtime_error("Opus encoder unavailable");
      opus_encoder_ctl(encoder.get(), OPUS_SET_BITRATE(params_.stream ? 128000 : 64000));
      opus_encoder_ctl(encoder.get(), OPUS_SET_COMPLEXITY(5));
      opus_encoder_ctl(encoder.get(), OPUS_SET_INBAND_FEC(1));
      if (!params_.stream) opus_encoder_ctl(encoder.get(), OPUS_SET_DTX(1));
    }
    auto clear_media = [&]() {
      // Packetization reserves sequence numbers ahead of transmission.
      // Reuse numbers belonging only to discarded, unsent primary packets.
      if (!pending.empty()) video_seq = pending.front().sequence;
      pending.clear();
      cache.clear();
      cache_order.clear();
      audio_buffer.clear();
      waiting_keyframe = true;
      if (params_.stream) capture_feed().flush();
      pacer.reset(microseconds());
      pending_frame_started_us = 0;
      next_audio = monotonic_ms();
      if (encoder)
        opus_encoder_ctl(encoder.get(), OPUS_RESET_STATE);
    };
    auto announce = [&]() {
      auto c = capture_feed().info();
      if (!params_.stream || !video_ssrc || !c.running)
        return;
      Json stream{
          {"type", "video"},
          {"rid", "100"},
          {"ssrc", video_ssrc},
          {"rtx_ssrc", rtx_ssrc},
          {"quality", 100},
          {"active", true},
          {"max_bitrate", 4000000},
          {"max_framerate", c.fps},
          {"max_resolution",
           {{"type", "fixed"}, {"width", c.width}, {"height", c.height}}}};
      send_json(12, {{"audio_ssrc", audio_ssrc},
                     {"video_ssrc", video_ssrc},
                     {"rtx_ssrc", rtx_ssrc},
                     {"streams", Json::array({stream})}});
      send_json(5, {{"speaking", 2}, {"delay", 0}, {"ssrc", audio_ssrc}});
      announced = true;
    };
    while (!stop_) {
      auto now = monotonic_ms();
      if (!described && now > connect_deadline)
        throw std::runtime_error("Discord media negotiation timed out");
      if (heartbeat_interval && now >= next_heartbeat) {
        if (!heartbeat_ack)
          throw std::runtime_error("Discord media heartbeat timed out");
        send_json(3, {{"t", now}, {"seq_ack", seq_ack}});
        heartbeat_ack = false;
        next_heartbeat = now + heartbeat_interval;
      }
      // Control traffic must not delay an already-due media packet. A quiet
      // WebSocket previously imposed another 2 ms wait on every sender loop.
      auto control_deadline = now + 2;
      if (!pending.empty()) {
        const auto clock_us = microseconds();
        const auto due = pacer.next_send_us();
        const auto remaining_us = due > clock_us ? due - clock_us : 0;
        control_deadline = now + std::min<std::uint64_t>(2, (remaining_us + 999) / 1000);
      }
      if (auto incoming = ws.receive(control_deadline)) {
        if (incoming->binary) {
          auto &b = incoming->data;
          if (b.size() < 3)
            throw std::runtime_error("Truncated media message");
          seq_ack = (b[0] << 8) | b[1];
          dave.binary(b[2], View(b).subspan(3));
        } else {
          auto msg = Json::parse(incoming->data);
          int op = msg.at("op");
          if (msg.contains("seq") && !msg["seq"].is_null())
            seq_ack = msg["seq"].get<std::int64_t>();
          const auto &d = msg["d"];
          if (op == 8) {
            heartbeat_interval =
                d.at("heartbeat_interval").get<std::uint64_t>();
            if (heartbeat_interval < 100 || heartbeat_interval > 120000)
              throw std::runtime_error("Invalid media heartbeat");
            next_heartbeat = now + heartbeat_interval;
            Json identify{{"server_id", params_.server_id},
                          {"channel_id", params_.channel_id},
                          {"user_id", params_.user_id},
                          {"session_id", params_.session_id},
                          {"token", params_.token},
                          {"video", params_.stream},
                          {"max_dave_protocol_version",
                           discord::dave::MaxSupportedProtocolVersion()}};
            if (params_.stream)
              identify["streams"] = Json::array(
                  {{{"type", "screen"}, {"rid", "100"}, {"quality", 100}}});
            send_json(0, identify);
          } else if (op == 6)
            heartbeat_ack = true;
          else if (op == 2) {
            audio_ssrc = d.at("ssrc");
            std::string ip = d.at("ip");
            int port = d.at("port");
            if (params_.stream) {
              auto streams = d.at("streams");
              if (streams.empty())
                throw std::runtime_error("Discord did not allocate video");
              video_ssrc = streams[0].at("ssrc");
              rtx_ssrc = streams[0].value("rtx_ssrc", video_ssrc + 1);
            }
            bool xchacha = false;
            for (auto &m : d.at("modes"))
              if (m == "aead_xchacha20_poly1305_rtpsize")
                xchacha = true;
            if (!xchacha)
              throw std::runtime_error("Required media encryption unavailable");
#ifndef PBSD_TESTING
            in_addr address{};
            if (inet_pton(AF_INET, ip.c_str(), &address) != 1)
              throw std::runtime_error("Invalid Discord UDP host");
            auto raw = ntohl(address.s_addr);
            if ((raw >> 24) == 127 || (raw >> 24) == 10 || (raw >> 24) == 0 ||
                (raw >> 24) >= 224 || (raw >> 16) == 0xc0a8 ||
                ((raw >> 20) == 0xac1) || (raw >> 16) == 0xa9fe)
              throw std::runtime_error("Non-public Discord UDP host");
#endif
            udp.connect(ip, port);
            Bytes probe(74);
            probe[1] = 1;
            probe[3] = 70;
            write32(probe, 4, audio_ssrc);
            udp.send(probe);
            discovery = true;
            discovery_start = now;
            dave.ssrcs(audio_ssrc, video_ssrc);
          } else if (op == 4) {
            if (params_.stream && d.contains("video_codec") &&
                lowercase(d.at("video_codec").get<std::string>()) != "h264")
              throw std::runtime_error("Discord selected an unsupported video codec; H264 is required");
            if (d.value("mode", "") != "aead_xchacha20_poly1305_rtpsize")
              throw std::runtime_error("Media encryption changed unexpectedly");
            auto key = d.at("secret_key").get<Bytes>();
            crypto = std::make_unique<TransportCrypto>(
                "aead_xchacha20_poly1305_rtpsize", key);
            secure_erase(key.data(), key.size());
            int version = d.value("dave_protocol_version", 0);
            dave.init(version);
            described = true;
            connected_ = true;
            clear_media();
            announce();
          } else if (op == 11) {
            if (d.contains("user_ids"))
              for (auto &id : d["user_ids"])
                dave.member(id.get<std::string>());
          } else if (op == 13) {
            dave.member(d.at("user_id").get<std::string>(), false);
          } else if (op == 21 || op == 22 || op == 24)
            dave.json(op, d);
          else if (op == 14 && params_.stream && d.contains("video_codec") &&
                   lowercase(d.at("video_codec").get<std::string>()) != "h264")
            throw std::runtime_error("Discord changed to an unsupported video codec; H264 is required");
          else if (op == 15 && params_.stream) {
            capture_feed().request_keyframe();
          }
        }
      }
      if (params_.stream) {
        auto info = capture_feed().info();
        if (epoch != dave.epoch() || generation != info.generation) {
          epoch = dave.epoch();
          generation = info.generation;
          clear_media();
          announced = false;
          if (described)
            announce();
        }
      }
      if (discovery && now - discovery_start > 8000)
        throw std::runtime_error("UDP address discovery timed out");
      for (int received = 0; received < 32; ++received) {
        auto packet = udp.receive();
        if (!packet)
          break;
        auto &b = *packet;
        if (discovery) {
          if (b.size() != 74 || b[0] != 0 || b[1] != 2 || b[2] != 0 ||
              b[3] != 70 || read32(b, 4) != audio_ssrc)
            continue;
          auto end = std::find(b.begin() + 8, b.begin() + 72, 0);
          if (end == b.begin() + 72)
            throw std::runtime_error("Malformed UDP discovery");
          std::string address(b.begin() + 8, end);
          in_addr addr{};
          if (inet_pton(AF_INET, address.c_str(), &addr) != 1)
            throw std::runtime_error("Malformed UDP discovery address");
          int port = (b[72] << 8) | b[73];
          Json codecs = Json::array({{{"name", "opus"},
                                      {"type", "audio"},
                                      {"priority", 1000},
                                      {"payload_type", 120}}});
          if (params_.stream)
            codecs.push_back({{"name", "H264"},
                              {"type", "video"},
                              {"priority", 1000},
                              {"payload_type", h264_payload_type},
                              {"rtx_payload_type", h264_rtx_payload_type},
                              {"encode", true},
                              {"decode", false}});
          send_json(1, {{"protocol", "udp"},
                        {"data",
                         {{"address", address},
                          {"port", port},
                          {"mode", "aead_xchacha20_poly1305_rtpsize"}}},
                        {"codecs", codecs}});
          discovery = false;
          continue;
        }
        if (!params_.stream || !crypto || b.size() < 8 || b[1] < 192 ||
            b[1] > 223)
          continue;
        auto plain = crypto->open(b, 8);
        if (!plain)
          continue;
        auto feedback = parse_rtcp(*plain, video_ssrc);
        if (feedback.keyframe)
          capture_feed().request_keyframe();
        if (feedback.bitrate) {
          pacer.set_target_bitrate(*feedback.bitrate);
          pacing_bitrate_ = pacer.bitrate();
        }
        for (auto lost : feedback.lost) {
          auto it = cache.find(lost);
          if (it == cache.end())
            continue;
          auto repair = retransmit_h264(it->second, rtx_ssrc, rtx_seq, transport_seq);
          auto sealed = crypto->seal(repair.bytes, 16);
          udp.send(sealed);
          ++repaired_;
          bytes_ += sealed.size();
        }
      }
      dave_ready_ = dave.ready();
      if (!params_.stream || !described || !crypto)
        continue;
      auto capture = capture_feed().info();
      if (!announced && capture.running)
        announce();
      if (epoch != dave.epoch() || generation != capture.generation) {
        epoch = dave.epoch();
        generation = capture.generation;
        clear_media();
        announced = false;
        announce();
      }
      if (!dave.ready() || !announced)
        continue;
      if (pending.empty()) {
        if (auto frame = capture_feed().take_video()) {
          if (frame->generation != generation)
            continue;
          if (waiting_keyframe && !frame->keyframe)
            continue;
          if (waiting_keyframe) {
            source_base = frame->timestamp;
            video_base =
                media_start
                    ? static_cast<std::uint32_t>((now - media_start) * 90)
                    : 0;
            if (!media_start) {
              media_start = now;
              media_unix = unix_ms();
            }
            audio_buffer.clear();
            next_audio = now;
            audio_timestamp =
                static_cast<std::uint32_t>((now - media_start) * 48);
            waiting_keyframe = false;
          }
          const auto preparation_started = microseconds();
          max_frame_bytes_ = std::max<std::uint64_t>(max_frame_bytes_.load(), frame->data.size());
          auto data = normalize_h264(frame->data);
          rewrite_sps(data);
          auto encrypted = dave.encrypt(true, video_ssrc, data);
          auto timestamp =
              video_base + std::uint32_t(frame->timestamp - source_base);
          auto packets = packetize_h264(encrypted, timestamp, video_ssrc,
                                        video_seq, transport_seq);
          if (packets.size() > 2048)
            throw std::runtime_error("Video frame packet budget exceeded");
          std::size_t wire_bytes = 0;
          for (const auto &p : packets) wire_bytes += p.bytes.size() + 20;
          for (auto &p : packets)
            pending.push_back(std::move(p));
          pending_frame_started_us = microseconds();
          max_frame_prepare_us_ = std::max(max_frame_prepare_us_.load(),
                                          pending_frame_started_us - preparation_started);
          pacer.begin_frame(pending_frame_started_us, wire_bytes,
                            1000000 / std::max(capture.fps, 1));
          pacing_bitrate_ = pacer.bitrate();
          ++frames_;
        }
      }
      for (int count = 0;
           count < 16 && !pending.empty() && pacer.ready(microseconds());
           ++count) {
        auto p = std::move(pending.front());
        pending.pop_front();
        auto sealed = crypto->seal(p.bytes, 16);
        udp.send(sealed);
        ++video_packets_;
        bytes_ += sealed.size();
        video_octets += p.bytes.size() - video_rtp_header_size;
        cache[p.sequence] = p.bytes;
        cache_order.push_back(p.sequence);
        while (cache_order.size() > 512) {
          cache.erase(cache_order.front());
          cache_order.pop_front();
        }
        const auto sent_us = microseconds();
        pacer.sent(sealed.size(), sent_us);
        if (pending.empty() && pending_frame_started_us) {
          max_frame_send_us_ = std::max(max_frame_send_us_.load(),
                                      sent_us - pending_frame_started_us);
          pending_frame_started_us = 0;
        }
      }
      if (!waiting_keyframe) {
        for (int count = 0; count < 16; ++count) {
          auto block = capture_feed().take_audio();
          if (!block)
            break;
          if (block->generation != generation)
            continue;
          std::size_t samples = block->data.size() / sizeof(float);
          if (samples % 2)
            continue;
          if (audio_buffer.size() + samples > 48000) {
            audio_buffer.clear();
            next_audio = now;
            audio_timestamp =
                static_cast<std::uint32_t>((now - media_start) * 48);
          }
          auto at = audio_buffer.size();
          audio_buffer.resize(at + samples);
          std::memcpy(audio_buffer.data() + at, block->data.data(),
                      samples * sizeof(float));
          for (std::size_t i = at; i < audio_buffer.size(); ++i)
            audio_buffer[i] = std::isfinite(audio_buffer[i])
                                  ? std::clamp(audio_buffer[i], -1.f, 1.f)
                                  : 0.f;
        }
        if (now >= next_audio && audio_buffer.size() >= 1920) {
          if (now > next_audio + 100) {
            next_audio = now;
            audio_timestamp =
                static_cast<std::uint32_t>((now - media_start) * 48);
            if (audio_buffer.size() > 1920 * 4)
              audio_buffer.erase(audio_buffer.begin(),
                                 audio_buffer.end() - 1920 * 4);
          }
          Bytes encoded(1275);
          int n = opus_encode_float(encoder.get(), audio_buffer.data(), 960,
                                    encoded.data(), encoded.size());
          if (n < 0)
            throw std::runtime_error("Game audio encoding failed");
          encoded.resize(n);
          audio_buffer.erase(audio_buffer.begin(), audio_buffer.begin() + 1920);
          auto protected_opus = dave.encrypt(false, audio_ssrc, encoded);
          auto packet = make_audio_rtp(protected_opus, audio_timestamp,
                                       audio_ssrc, audio_seq++);
          auto sealed = crypto->seal(packet, 12);
          udp.send(sealed);
          ++audio_packets_;
          bytes_ += sealed.size();
          audio_octets += protected_opus.size();
          audio_timestamp += 960;
          next_audio += 20;
        }
      }
      if (media_start && now - last_report >= 1000) {
        auto timestamp = static_cast<std::uint32_t>((now - media_start) * 90);
        auto video_report =
            make_sender_report(video_ssrc, timestamp, video_packets_,
                               video_octets, media_unix + now - media_start);
        udp.send(crypto->seal(video_report, 8));
        auto audio_report = make_sender_report(
            audio_ssrc, static_cast<std::uint32_t>((now - media_start) * 48),
            audio_packets_, audio_octets, media_unix + now - media_start);
        udp.send(crypto->seal(audio_report, 8));
        last_report = now;
      }
    }
    ws.close();
  } catch (const std::exception &e) {
    std::lock_guard lock(error_mutex_);
    error_ = e.what();
  }
  connected_ = false;
  dave_ready_ = false;
}
} // namespace pbsd
