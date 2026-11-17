// SPDX-License-Identifier: GPL-3.0-or-later
// Uses libdave's official ExternalSender fixture (Discord contributors).
#include "crypto.hpp"
#include "external_sender.h"
#include "net.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <opus/opus.h>
#include <stdexcept>
#include <string_view>
#include <unistd.h>

using namespace pbsd;
namespace {
void check(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}
template <class F> void rejects(F &&call, const char *message) {
  bool rejected = false;
  try {
    call();
  } catch (const std::exception &) {
    rejected = true;
  }
  check(rejected, message);
}
Bytes hex(std::string_view text) {
  Bytes result;
  auto nibble = [](char c) {
    return unsigned(c <= '9' ? c - '0' : c - 'a' + 10);
  };
  for (std::size_t i = 0; i < text.size(); i += 2)
    result.push_back((nibble(text[i]) << 4) | nibble(text[i + 1]));
  return result;
}
void transport() {
  Bytes key(32);
  for (unsigned i = 0; i < key.size(); ++i)
    key[i] = i;
  TransportCrypto crypto("aead_xchacha20_poly1305_rtpsize", key);
  // RTP + extension header remain clear; its extension data and payload are
  // sealed.
  Bytes rtp = hex("906900010102030400001122bede0002520000003100010065b8fafafa");
  auto first = crypto.seal(rtp, 16), second = crypto.seal(rtp, 16);
  check(first.size() == rtp.size() + 20,
        "transport tag/nonce overhead incorrect");
  check(std::equal(rtp.begin(), rtp.begin() + 16, first.begin()),
        "transport modified RTP clear header");
  check(first != second &&
            !std::equal(first.end() - 4, first.end(), second.end() - 4),
        "transport nonce reused");
  check(crypto.open(first, 16) == rtp && crypto.open(second, 16) == rtp,
        "transport RTP round trip failed");
  for (auto index :
       {std::size_t(2), std::size_t(17), first.size() - 5, first.size() - 1}) {
    auto damaged = first;
    damaged[index] ^= 0x40;
    check(!crypto.open(damaged, 16),
          "transport accepted modified header/ciphertext/tag/nonce");
  }
  for (std::size_t size = 0; size < first.size(); ++size)
    check(!crypto.open(View(first).first(size), 16),
          "transport accepted truncated packet");
  check(!crypto.open(first, first.size() + 1),
        "transport accepted invalid clear bound");
  rejects([&] { crypto.seal(rtp, rtp.size() + 1); },
          "transport accepted invalid seal bound");
  rejects([&] { TransportCrypto invalid("plain", key); },
          "transport accepted plaintext mode");
  rejects(
      [&] {
        TransportCrypto invalid("aead_xchacha20_poly1305_rtpsize", Bytes(31));
      },
      "transport accepted invalid key size");
  // RTCP's distinct clear-header size and audio's ordinary RTP header also
  // work.
  const auto report =
      hex("80c80006112233440000000100000002000000030000000400000005");
  check(crypto.open(crypto.seal(report, 8), 8) == report,
        "transport RTCP round trip failed");
  const auto audio = hex("80780001000003c011223344fc112233");
  check(crypto.open(crypto.seal(audio, 12), 12) == audio,
        "transport audio RTP round trip failed");
}
Bytes opus_frame() {
  int error = 0;
  std::unique_ptr<OpusEncoder, decltype(&opus_encoder_destroy)> encoder(
      opus_encoder_create(48000, 2, OPUS_APPLICATION_AUDIO, &error),
      opus_encoder_destroy);
  check(error == OPUS_OK && encoder != nullptr,
        "Opus encoder initialization failed");
  std::vector<float> pcm(1920);
  for (unsigned i = 0; i < 960; ++i)
    pcm[2 * i] = pcm[2 * i + 1] = 0.2f * std::sin(float(i) * 0.057595865f);
  Bytes out(1275);
  const auto size =
      opus_encode_float(encoder.get(), pcm.data(), 960, out.data(), out.size());
  check(size > 3, "Opus fixture encoding failed");
  out.resize(size);
  return out;
}
struct Emitted {
  std::vector<std::pair<int, Json>> json;
  std::vector<std::pair<int, Bytes>> binary;
  Bytes latest(int opcode) const {
    for (auto i = binary.rbegin(); i != binary.rend(); ++i)
      if (i->first == opcode)
        return i->second;
    throw std::runtime_error("expected DAVE message was not emitted");
  }
};
void dave_mls() {
  namespace dd = discord::dave;
  constexpr std::uint64_t group = 1234567;
  const auto version = dd::MaxSupportedProtocolVersion();
  check(version > 0, "DAVE protocol unavailable");
  Emitted emitted;
  std::shared_ptr<mlspp::SignaturePrivateKey> identity;
  Dave sender(
      group, "1001", identity,
      [&](int op, const Json &data) { emitted.json.emplace_back(op, data); },
      [&](int op, View data) {
        emitted.binary.emplace_back(op, Bytes(data.begin(), data.end()));
      });
  sender.member("1002");
  sender.ssrcs(1111, 2222);
  const auto opus = opus_frame();
  rejects([&] { sender.encrypt(false, 1111, opus); },
          "DAVE encrypted before initialization");
  sender.init(version);
  check(!sender.ready(), "DAVE ready before MLS establishment");
  check(!emitted.latest(26).empty(), "DAVE key package missing");
  rejects([&] { sender.encrypt(false, 1111, opus); },
          "DAVE encrypted before receiving keys");
  auto receiver = dd::mls::CreateSession(
      nullptr, "", [](const std::string &, const std::string &) {});
  check(receiver != nullptr, "receiver MLS initialization failed");
  std::shared_ptr<mlspp::SignaturePrivateKey> receiver_identity;
  receiver->Init(version, group, "1002", receiver_identity);
  auto receiver_package = receiver->GetMarshalledKeyPackage();
  check(!receiver_package.empty(), "receiver key package missing");
  dd::test::ExternalSender external(version, group);
  const auto external_package = external.GetMarshalledExternalSender();
  sender.binary(25, external_package);
  receiver->SetExternalSender(external_package);
  sender.binary(27, external.ProposeAdd(0, receiver_package));
  auto [commit, welcome] = external.SplitCommitWelcome(emitted.latest(28));
  check(!commit.empty() && !welcome.empty(), "MLS commit/welcome missing");
  Bytes transition{0, 0};
  transition.insert(transition.end(), commit.begin(), commit.end());
  sender.binary(29, transition);
  check(sender.ready(), "sender failed initial MLS transition");
  check(!emitted.json.empty() && emitted.json.back().first == 23 &&
            emitted.json.back().second.at("transition_id") == 0,
        "initial transition ACK missing");
  check(receiver->ProcessWelcome(welcome, {"1001", "1002"}).has_value(),
        "independent receiver rejected MLS welcome");
  check(receiver->GetKeyRatchet("1001") != nullptr,
        "receiver missing sender ratchet");
  // Regression: ignored duplicate commits must preserve keys, readiness and
  // epoch.
  const auto epoch = sender.epoch(), binary_count = emitted.binary.size(),
             json_count = emitted.json.size();
  sender.binary(29, transition);
  check(sender.ready() && sender.epoch() == epoch,
        "ignored commit paused or advanced DAVE");
  check(emitted.binary.size() == binary_count &&
            emitted.json.size() == json_count,
        "ignored commit emitted a reset or transition ACK");
  auto decrypt = [&](dd::MediaType type, View ciphertext, Bytes &out) {
    auto decryptor = dd::CreateDecryptor();
    check(decryptor != nullptr, "DAVE decryptor unavailable");
    auto ratchet = receiver->GetKeyRatchet("1001");
    check(ratchet != nullptr, "sender ratchet lost");
    decryptor->TransitionToKeyRatchet(std::move(ratchet),
                                      std::chrono::seconds(0));
    out.resize(decryptor->GetMaxPlaintextByteSize(type, ciphertext.size()));
    std::size_t count = 0;
    const auto result =
        decryptor->Decrypt(type, {ciphertext.data(), ciphertext.size()},
                           {out.data(), out.size()}, &count);
    check(count <= out.size(), "DAVE decrypt output exceeded capacity");
    out.resize(count);
    return result;
  };
  // libdave's H264ShortIDR fixture: real Annex B SPS/PPS and a bounded slice.
  const auto h264 = hex("000000016742c00d8c8d40d0fbc900f08846a00000000168ce3c80"
                        "0000000165b8fafafa");
  for (bool video : {false, true}) {
    const auto &plain = video ? h264 : opus;
    const auto type = video ? dd::Video : dd::Audio;
    auto ciphertext = sender.encrypt(video, video ? 2222 : 1111, plain);
    check(ciphertext != plain && ciphertext.size() > plain.size(),
          "DAVE returned unprotected media");
    Bytes decoded;
    check(decrypt(type, ciphertext, decoded) == dd::IDecryptor::Success &&
              decoded == plain,
          "independent DAVE media decryption failed");
    // Fresh decryptor per attempt ensures rejection tests authentication, not
    // replay.
    for (auto index : {std::size_t(video ? ciphertext.size() / 2 : 1),
                       ciphertext.size() - 4}) {
      auto damaged = ciphertext;
      damaged[index] ^= 0x20;
      check(decrypt(type, damaged, decoded) != dd::IDecryptor::Success,
            "DAVE accepted tampered media");
    }
  }
  rejects(
      [&] {
        sender.json(21, {{"transition_id", -1}, {"protocol_version", version}});
      },
      "negative DAVE transition accepted");
  rejects(
      [&] {
        sender.json(21,
                    {{"transition_id", 65536}, {"protocol_version", version}});
      },
      "oversized DAVE transition accepted");
  rejects(
      [&] { sender.json(21, {{"transition_id", 4}, {"protocol_version", 0}}); },
      "DAVE downgrade accepted");
  check(sender.ready() && sender.epoch() == epoch,
        "invalid preparation changed active DAVE state");
  sender.json(21, {{"transition_id", 7}, {"protocol_version", version}});
  check(!sender.ready() && sender.epoch() > epoch,
        "DAVE preparation did not pause sending");
  check(emitted.json.back().first == 23 &&
            emitted.json.back().second.at("transition_id") == 7,
        "prepare transition ACK missing");
  rejects([&] { sender.encrypt(false, 1111, opus); },
          "DAVE encrypted during pending transition");
  const auto prepared = sender.epoch();
  sender.json(22, {{"transition_id", 8}});
  check(!sender.ready() && sender.epoch() > prepared &&
            emitted.binary.back().first == 26,
        "out-of-order execution did not reset safely");
  rejects([&] { sender.encrypt(false, 1111, opus); },
          "DAVE encrypted after invalid transition order");
}
struct TempVault {
  std::string directory, path;
  TempVault() {
    char pattern[] = "./crypto-vault-XXXXXX";
    auto *created = mkdtemp(pattern);
    check(created != nullptr, "cannot create vault test directory");
    directory = created;
    path = directory + "/account.vault";
  }
  ~TempVault() {
    unlink(path.c_str());
    unlink((path + ".tmp").c_str());
    rmdir(directory.c_str());
  }
};
std::string read_file(const std::string &path) {
  std::ifstream input(path, std::ios::binary);
  check(input.good(), "vault file missing");
  return std::string(std::istreambuf_iterator<char>(input), {});
}
void vault() {
  TempVault temp;
  const std::string pass = "this is a strong test-only passphrase",
                    token =
                        "test-only-session-token-not-a-live-account-0123456789";
  save_vault(temp.path, pass, token);
  const auto original = read_file(temp.path);
  check(original.find(token) == std::string::npos &&
            original.find(pass) == std::string::npos,
        "vault leaked plaintext credentials");
  check(load_vault(temp.path, pass) == token, "vault round trip failed");
  rejects([&] { load_vault(temp.path, "incorrect test-only passphrase"); },
          "vault accepted wrong passphrase");
  auto envelope = Json::parse(original);
  auto ciphertext = unbase64(envelope.at("data"));
  check(!ciphertext.empty(), "vault ciphertext missing");
  ciphertext[ciphertext.size() / 2] ^= 0x80;
  envelope["data"] = base64(ciphertext);
  {
    std::ofstream output(temp.path, std::ios::binary | std::ios::trunc);
    output << envelope.dump();
    check(output.good(), "cannot tamper owned test vault");
  }
  rejects([&] { load_vault(temp.path, pass); },
          "vault accepted modified ciphertext");
  save_vault(temp.path, pass, token);
  check(load_vault(temp.path, pass) == token, "vault replacement failed");
  check(read_file(temp.path) != original,
        "vault reused salt/nonce across saves");
}
} // namespace
int main() {
  try {
    transport();
    dave_mls();
    vault();
    std::cout << "crypto tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "crypto test failed: " << error.what() << '\n';
    return 1;
  }
}
