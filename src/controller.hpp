// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "voice.hpp"
#include "login_verification.hpp"
#include "channel_cache.hpp"
#include "activity_settings.hpp"
#include "game_artwork.hpp"
#include "account_store.hpp"
#include <deque>
namespace pbsd {
class Controller {
public:
  explicit Controller(std::string directory);
  ~Controller();
  Json status() const;
  Json api(const std::string &path, const Json &body);
  void stop();

private:
  std::string directory_, token_;
  ActivitySettings activity_settings_;
  GameArtwork game_artwork_;
  AccountStore account_store_;
  bool remember_login_{};
  std::string account_save_error_;
  std::optional<GameTitle> activity_game_;
  mutable std::mutex mutex_;
  std::mutex operation_;
  std::string state_{"disconnected"}, error_, qr_url_;
  Json account_ = nullptr, guilds_ = Json::array(), dms_ = Json::array();
  ChannelCache channels_;
  std::deque<Json> commands_;
  std::atomic<bool> stopping_{};
  std::thread worker_;
  std::shared_ptr<mlspp::SignaturePrivateKey> identity_;
  std::unique_ptr<VoiceSession> call_, stream_;
  Json target_ = nullptr;
  LoginVerification verification_;
  void connect_account(std::string token, bool remember = false);
  void link(bool remember = true);
  void gateway();
  void set_state(std::string state, std::string error = {});
  std::string token_copy() const;
  Json rest(const std::string &method, const std::string &path,
            const Json &body = nullptr);
  void bootstrap(const Json &data);
};
} // namespace pbsd
