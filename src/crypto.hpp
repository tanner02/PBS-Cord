// SPDX-License-Identifier: GPL-3.0-or-later
// DAVE lifecycle adapted from Acheron (ouwou), GPL-3.0-or-later.
#pragma once
#include "types.hpp"
#include <dave/dave_interfaces.h>
#include <functional>
#include <nlohmann/json.hpp>
#include <set>
namespace pbsd {
using Json = nlohmann::json;
class TransportCrypto {
public:
  TransportCrypto(std::string mode, View key);
  ~TransportCrypto();
  Bytes seal(View packet, std::size_t clear);
  std::optional<Bytes> open(View packet, std::size_t clear) const;
  const std::string &mode() const { return mode_; }

private:
  std::string mode_;
  Bytes key_;
  std::uint64_t counter_{};
};
class Dave {
public:
  using SendJson = std::function<void(int, const Json &)>;
  using SendBinary = std::function<void(int, View)>;
  Dave(std::uint64_t group, std::string user,
       std::shared_ptr<mlspp::SignaturePrivateKey> &identity, SendJson json,
       SendBinary binary);
  void init(std::uint16_t version);
  void ssrcs(std::uint32_t audio, std::uint32_t video);
  void member(const std::string &id, bool present = true);
  void json(int opcode, const Json &data);
  void binary(int opcode, View data);
  bool ready() const { return ready_; }
  std::uint64_t epoch() const { return epoch_; }
  Bytes encrypt(bool video, std::uint32_t ssrc, View frame);

private:
  std::uint64_t group_;
  std::string user_;
  std::shared_ptr<mlspp::SignaturePrivateKey> &identity_;
  std::unique_ptr<discord::dave::mls::ISession> session_;
  std::unique_ptr<discord::dave::IEncryptor> encryptor_;
  std::set<std::string> members_;
  SendJson send_json_;
  SendBinary send_binary_;
  std::uint16_t version_{};
  int transition_{-1};
  std::uint16_t pending_version_{};
  std::uint32_t audio_{}, video_{};
  bool ready_{}, pending_{};
  std::uint64_t epoch_{};
  void complete(int transition);
  void reset();
};
void save_vault(const std::string &path, const std::string &passphrase,
                const std::string &token);
std::string load_vault(const std::string &path, const std::string &passphrase);
} // namespace pbsd
