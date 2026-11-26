// SPDX-License-Identifier: GPL-3.0-or-later
// DAVE lifecycle follows Acheron, Copyright its respective contributors.
#include "crypto.hpp"
#include "net.hpp"
#include <algorithm>
#include <cstdio>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <mutex>
#include <openssl/evp.h>
#include <sodium.h>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
namespace pbsd {
static void require(bool b, const char *m) {
  if (!b)
    throw std::runtime_error(m);
}
TransportCrypto::TransportCrypto(std::string mode, View key)
    : mode_(std::move(mode)), key_(key.begin(), key.end()) {
  require(sodium_init() >= 0, "Transport crypto initialization failed");
  require(key_.size() == 32, "Invalid media key");
  require(mode_ == "aead_xchacha20_poly1305_rtpsize" ||
              mode_ == "aead_aes256_gcm_rtpsize",
          "Unsupported media encryption mode");
  if (mode_ == "aead_aes256_gcm_rtpsize")
    require(crypto_aead_aes256gcm_is_available(),
            "AES media encryption unavailable");
}
TransportCrypto::~TransportCrypto() { secure_erase(key_.data(), key_.size()); }
Bytes TransportCrypto::seal(View p, std::size_t clear) {
  require(clear <= p.size() && p.size() <= 65516, "Invalid media packet");
  require(counter_ <= UINT32_MAX, "Media nonce exhausted; reconnect required");
  unsigned char nonce[24]{};
  auto n = static_cast<std::uint32_t>(counter_++);
  for (int i = 0; i < 4; ++i)
    nonce[i] = n >> (24 - i * 8);
  Bytes out(p.size() + 20);
  std::copy_n(p.begin(), clear, out.begin());
  unsigned long long written = 0;
  int rc =
      mode_ == "aead_aes256_gcm_rtpsize"
          ? crypto_aead_aes256gcm_encrypt(
                out.data() + clear, &written, p.data() + clear,
                p.size() - clear, p.data(), clear, nullptr, nonce, key_.data())
          : crypto_aead_xchacha20poly1305_ietf_encrypt(
                out.data() + clear, &written, p.data() + clear,
                p.size() - clear, p.data(), clear, nullptr, nonce, key_.data());
  require(rc == 0, "Media encryption failed");
  out.resize(clear + written + 4);
  std::copy_n(nonce, 4, out.end() - 4);
  return out;
}
std::optional<Bytes> TransportCrypto::open(View p, std::size_t clear) const {
  if (clear > p.size() || p.size() < clear + 20 || p.size() > 65536)
    return std::nullopt;
  unsigned char nonce[24]{};
  std::copy(p.end() - 4, p.end(), nonce);
  Bytes out(p.size() - 20);
  std::copy_n(p.begin(), clear, out.begin());
  unsigned long long written = 0;
  int rc = mode_ == "aead_aes256_gcm_rtpsize"
               ? crypto_aead_aes256gcm_decrypt(
                     out.data() + clear, &written, nullptr, p.data() + clear,
                     p.size() - clear - 4, p.data(), clear, nonce, key_.data())
               : crypto_aead_xchacha20poly1305_ietf_decrypt(
                     out.data() + clear, &written, nullptr, p.data() + clear,
                     p.size() - clear - 4, p.data(), clear, nonce, key_.data());
  if (rc)
    return std::nullopt;
  out.resize(clear + written);
  return out;
}
Dave::Dave(std::uint64_t group, std::string user,
           std::shared_ptr<mlspp::SignaturePrivateKey> &identity, SendJson json,
           SendBinary binary)
    : group_(group), user_(std::move(user)), identity_(identity),
      send_json_(std::move(json)), send_binary_(std::move(binary)) {
  require(group_ && user_.find_first_not_of("0123456789") == std::string::npos,
          "Invalid DAVE identity");
  session_ = discord::dave::mls::CreateSession(
      nullptr, "", [](const std::string &, const std::string &) {});
  require(session_ != nullptr, "DAVE initialization failed");
  members_.insert(user_);
}
void Dave::member(const std::string &id, bool present) {
  if (id.empty() || id.size() > 20 ||
      id.find_first_not_of("0123456789") != std::string::npos)
    return;
  if (present) {
    require(members_.size() < 1024 || members_.contains(id),
            "Too many media members");
    members_.insert(id);
  } else if (id != user_) {
    members_.erase(id);
  }
}
void Dave::ssrcs(std::uint32_t audio, std::uint32_t video) {
  audio_ = audio;
  video_ = video;
  if (encryptor_) {
    if (audio_)
      encryptor_->AssignSsrcToCodec(audio_, discord::dave::Opus);
    if (video_)
      encryptor_->AssignSsrcToCodec(video_, discord::dave::H264);
  }
}
void Dave::init(std::uint16_t version) {
  require(version <= discord::dave::MaxSupportedProtocolVersion(),
          "DAVE version unsupported");
  version_ = pending_version_ = version;
  reset();
}
void Dave::reset() {
  ready_ = false;
  pending_ = false;
  transition_ = -1;
  ++epoch_;
  if (!version_) {
    session_->Reset();
    encryptor_.reset();
    return;
  }
  {
    static std::mutex identity_mutex;
    std::lock_guard lock(identity_mutex);
    session_->Init(version_, group_, user_, identity_);
  }
  encryptor_ = discord::dave::CreateEncryptor();
  require(encryptor_ != nullptr, "DAVE encryptor unavailable");
  ssrcs(audio_, video_);
  auto key = session_->GetMarshalledKeyPackage();
  require(!key.empty() && key.size() <= 65536, "DAVE key package unavailable");
  send_binary_(26, key);
}
void Dave::complete(int transition) {
  require(transition_ == transition && pending_,
          "DAVE transition out of order");
  auto ratchet = session_->GetKeyRatchet(user_);
  require(ratchet != nullptr, "DAVE sender keys unavailable");
  encryptor_->SetKeyRatchet(std::move(ratchet));
  version_ = pending_version_;
  pending_ = false;
  transition_ = -1;
  ready_ = version_ > 0;
  ++epoch_;
}
void Dave::json(int op, const Json &d) {
  if (op == 21) {
    int t = d.at("transition_id").get<int>();
    int v = d.at("protocol_version").get<int>();
    require(t >= 0 && t <= 65535 && v > 0 &&
                v <= discord::dave::MaxSupportedProtocolVersion(),
            "Unsupported DAVE transition");
    ready_ = false;
    ++epoch_;
    transition_ = t;
    pending_version_ = v;
    send_json_(23, {{"transition_id", t}});
  } else if (op == 22) {
    int t = d.at("transition_id").get<int>();
    if (t != transition_ || !pending_) {
      reset();
      return;
    }
    complete(t);
  } else if (op == 24 && d.value("epoch", 0) == 1) {
    int v = d.at("protocol_version").get<int>();
    require(v > 0 && v <= discord::dave::MaxSupportedProtocolVersion(),
            "Unsupported DAVE epoch");
    version_ = pending_version_ = v;
    reset();
  }
}
void Dave::binary(int op, View b) {
  require(b.size() <= 65536, "DAVE message too large");
  if (!version_)
    return;
  if (op == 25) {
    session_->SetExternalSender(Bytes(b.begin(), b.end()));
  } else if (op == 27) {
    auto response =
        session_->ProcessProposals(Bytes(b.begin(), b.end()), members_);
    if (response)
      send_binary_(28, *response);
  } else if (op == 29 || op == 30) {
    require(b.size() >= 2, "Truncated DAVE transition");
    int t = (b[0] << 8) | b[1];
    Bytes data(b.begin() + 2, b.end());
    bool ok = false;
    if (op == 29) {
      auto r = session_->ProcessCommit(std::move(data));
      if (std::holds_alternative<discord::dave::ignored_t>(r))
        return;
      ok = std::holds_alternative<discord::dave::RosterMap>(r);
    } else
      ok = session_->ProcessWelcome(std::move(data), members_).has_value();
    ready_ = false;
    ++epoch_;
    transition_ = t;
    if (ok) {
      pending_ = true;
      send_json_(23, {{"transition_id", t}});
      if (t == 0)
        complete(t);
    } else {
      pending_ = false;
      send_json_(31, {{"transition_id", t}});
      reset();
    }
  }
}
Bytes Dave::encrypt(bool video, std::uint32_t ssrc, View frame) {
  require(ready_ && encryptor_ && encryptor_->HasKeyRatchet(),
          "DAVE keys not ready");
  require(frame.size() <= 2 * 1024 * 1024, "Media frame too large");
  std::size_t cap =
      encryptor_->GetMaxCiphertextByteSize(
          video ? discord::dave::Video : discord::dave::Audio, frame.size()) +
      frame.size() * 5 + 256;
  require(cap <= 13 * 1024 * 1024, "Encrypted frame budget exceeded");
  Bytes out(cap);
  std::size_t count = 0;
  auto result = encryptor_->Encrypt(
      video ? discord::dave::Video : discord::dave::Audio, ssrc,
      {frame.data(), frame.size()}, {out.data(), out.size()}, &count);
  require(result == discord::dave::IEncryptor::Success && count <= out.size(),
          "DAVE frame encryption failed");
  out.resize(count);
  return out;
}
} // namespace pbsd
