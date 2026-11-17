// SPDX-License-Identifier: GPL-3.0-or-later
// User Gateway, QR linking and stream signaling reference
// Acheron/Yapfast/Paicord.
#include "controller.hpp"
#include "destination_metadata.hpp"
#include "net.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/sha.h>
#include <stdexcept>
#include <thread>
namespace pbsd {
static std::string id_of(const Json &j, const char *key = "id") {
  return destination_metadata::id_of(j, key);
}
static Json parse_api(HttpResponse r) {
  if (r.status == 429)
    throw std::runtime_error("Discord rate limit; wait before retrying");
  if (r.status < 200 || r.status >= 300)
    throw std::runtime_error("Discord rejected the request (HTTP " +
                             std::to_string(r.status) + ")");
  if (r.body.empty())
    return nullptr;
  return Json::parse(r.body);
}
Controller::Controller(std::string directory)
    : directory_(std::move(directory)), activity_settings_(directory_),
      game_artwork_({}, directory_ + "/game-artwork.json"), account_store_(directory_) {
  try {
    if (auto token = account_store_.restore()) connect_account(std::move(*token));
  } catch (const std::exception &e) {
    account_save_error_ = e.what();
    state_ = "disconnected";
  }
}
Controller::~Controller() {
  stop();
  std::lock_guard lock(mutex_);
  secure_erase(token_.data(), token_.size());
}
void Controller::set_state(std::string state, std::string error) {
  std::lock_guard lock(mutex_);
  state_ = std::move(state);
  error_ = std::move(error);
}
std::string Controller::token_copy() const {
  std::lock_guard lock(mutex_);
  return token_;
}
void Controller::stop() {
  stopping_ = true;
  verification_.clear();
  if (worker_.joinable())
    worker_.join();
  verification_.clear();
  std::lock_guard lock(mutex_);
  activity_game_.reset();
}
Json Controller::status() const {
  std::lock_guard lock(mutex_);
  auto capture = capture_feed().info();
  Json stats = stream_
                   ? stream_->status()
                   : Json{{"dave_ready", false},    {"video_packets", 0},
                          {"audio_packets", 0},     {"bytes", 0},
                          {"width", capture.width}, {"height", capture.height},
                          {"fps", capture.fps}};
  if (!target_.is_null()) {
    stats["channel_id"] = target_["channel_id"];
    stats["guild_id"] = target_["guild_id"];
  }
  std::ifstream vault(directory_ + "/account.vault");
  return {{"state", state_},
          {"error", error_},
          {"account", account_},
          {"qr_url", qr_url_.empty() ? Json(nullptr) : Json(qr_url_)},
          {"verification", verification_.status(monotonic_ms())},
          {"activity", {{"enabled", activity_settings_.enabled()},
                        {"game", activity_game_ ? Json{{"title_id", activity_game_->title_id},
                                                      {"name", activity_game_->name},
                                                      {"image_url", activity_game_->image_url}} : Json(nullptr)}}},
          {"guilds", guilds_},
          {"dms", dms_},
          {"stream", stats},
          {"capture",
           {{"running", capture.running},
            {"width", capture.width},
            {"height", capture.height},
            {"fps", capture.fps},
            {"dropped", capture_feed().dropped()}}},
          {"vault_saved", vault.good() || account_store_.saved()},
          {"auto_login_saved", account_store_.saved()},
          {"account_save_error", account_save_error_}};
}
Json Controller::rest(const std::string &method, const std::string &path,
                      const Json &body) {
  auto token = token_copy();
  if (token.empty())
    throw std::runtime_error("Connect an account first");
  try {
    auto result = parse_api(https(method, "https://discord.com/api/v10" + path,
                                  body.is_null() ? "" : body.dump(), token));
    secure_erase(token.data(), token.size());
    return result;
  } catch (...) {
    secure_erase(token.data(), token.size());
    throw;
  }
}
void Controller::connect_account(std::string token, bool remember) {
  stop();
  {
    std::lock_guard lock(mutex_);
    secure_erase(token_.data(), token_.size());
    token_ = std::move(token);
    state_ = "connecting";
    remember_login_ = remember;
    account_save_error_.clear();
    error_.clear();
    qr_url_.clear();
    account_ = nullptr;
    guilds_ = Json::array();
    dms_ = Json::array();
    channels_.clear();
    commands_.clear();
    target_ = nullptr;
  }
  stopping_ = false;
  worker_ = std::thread([this]() {
    try {
      gateway();
    } catch (const std::exception &e) {
      set_state("error", e.what());
    }
    std::unique_ptr<VoiceSession> s, c;
    {
      std::lock_guard lock(mutex_);
      s = std::move(stream_);
      c = std::move(call_);
    }
    s.reset();
    c.reset();
  });
}
static Bytes rsa_decrypt(EVP_PKEY *key, const std::string &text) {
  auto cipher = unbase64(text);
  std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> ctx(
      EVP_PKEY_CTX_new(key, nullptr), EVP_PKEY_CTX_free);
  if (!ctx || EVP_PKEY_decrypt_init(ctx.get()) <= 0 ||
      EVP_PKEY_CTX_set_rsa_padding(ctx.get(), RSA_PKCS1_OAEP_PADDING) <= 0 ||
      EVP_PKEY_CTX_set_rsa_oaep_md(ctx.get(), EVP_sha256()) <= 0 ||
      EVP_PKEY_CTX_set_rsa_mgf1_md(ctx.get(), EVP_sha256()) <= 0)
    throw std::runtime_error("QR decryption setup failed");
  std::size_t n = 0;
  if (EVP_PKEY_decrypt(ctx.get(), nullptr, &n, cipher.data(), cipher.size()) <=
          0 ||
      n > 4096)
    throw std::runtime_error("Invalid QR handshake");
  Bytes plain(n);
  if (EVP_PKEY_decrypt(ctx.get(), plain.data(), &n, cipher.data(),
                       cipher.size()) <= 0)
    throw std::runtime_error("QR handshake decryption failed");
  plain.resize(n);
  return plain;
}
void Controller::link(bool remember) {
  stop();
  {
    std::lock_guard lock(mutex_);
    secure_erase(token_.data(), token_.size());
    token_.clear();
    state_ = "linking";
    remember_login_ = remember;
    account_save_error_.clear();
    error_.clear();
    qr_url_.clear();
    account_ = nullptr;
    guilds_ = Json::array();
    dms_ = Json::array();
    channels_.clear();
    commands_.clear();
    target_ = nullptr;
  }
  stopping_ = false;
  worker_ = std::thread([this]() {
    try {
      std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> keyctx(
          EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr), EVP_PKEY_CTX_free);
      EVP_PKEY *raw = nullptr;
      if (!keyctx || EVP_PKEY_keygen_init(keyctx.get()) <= 0 ||
          EVP_PKEY_CTX_set_rsa_keygen_bits(keyctx.get(), 2048) <= 0 ||
          EVP_PKEY_keygen(keyctx.get(), &raw) <= 0)
        throw std::runtime_error("QR signing key generation failed");
      std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(raw,
                                                              EVP_PKEY_free);
      int length = i2d_PUBKEY(raw, nullptr);
      if (length <= 0)
        throw std::runtime_error("QR public key unavailable");
      Bytes der(length);
      auto *ptr = der.data();
      i2d_PUBKEY(raw, &ptr);
      WebSocket ws;
      ws.connect("wss://remote-auth-gateway.discord.gg/?v=2",
                 "https://discord.com", 65536);
      std::uint64_t interval = 0, next = 0, deadline = monotonic_ms() + 180000;
      bool ack = true;
      std::string token;
      auto service_gateway = [&]() -> std::optional<WebSocket::Message> {
        auto now = monotonic_ms();
        if (interval && now >= next) {
          if (!ack)
            throw std::runtime_error("QR connection expired; link again");
          ws.text(Json{{"op", "heartbeat"}}.dump());
          ack = false;
          next = now + interval;
        }
        auto message = ws.receive(now + 50);
        if (message && !message->binary) {
          auto event = Json::parse(message->data);
          if (event.value("op", "") == "heartbeat_ack") ack = true;
          if (event.value("op", "") == "cancel")
            throw std::runtime_error("Account linking cancelled on your phone");
        }
        return message;
      };
      while (!stopping_ && monotonic_ms() < deadline) {
        auto now = monotonic_ms();
        auto message = service_gateway();
        if (!message || message->binary)
          continue;
        auto j = Json::parse(message->data);
        auto op = j.value("op", "");
        if (op == "hello") {
          interval = j.at("heartbeat_interval");
          if (interval < 1000 || interval > 120000)
            throw std::runtime_error("Invalid QR heartbeat");
          next = now + interval;
          ws.text(
              Json{{"op", "init"}, {"encoded_public_key", base64(der)}}.dump());
        } else if (op == "heartbeat_ack")
          ack = true;
        else if (op == "nonce_proof") {
          auto nonce = rsa_decrypt(raw, j.at("encrypted_nonce"));
          unsigned char hash[32];
          SHA256(nonce.data(), nonce.size(), hash);
          secure_erase(nonce.data(), nonce.size());
          ws.text(Json{{"op", "nonce_proof"}, {"proof", base64(hash, true)}}
                      .dump());
        } else if (op == "pending_remote_init") {
          auto fingerprint = j.at("fingerprint").get<std::string>();
          if (fingerprint.size() > 256 ||
              fingerprint.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefgh"
                                            "ijklmnopqrstuvwxyz0123456789-_") !=
                  std::string::npos)
            throw std::runtime_error("Invalid QR link");
          std::lock_guard lock(mutex_);
          qr_url_ = "https://discord.com/ra/" + fingerprint;
        } else if (op == "pending_login") {
          auto ticket = j.at("ticket").get<std::string>();
          if (ticket.empty() || ticket.size() > 4096)
            throw std::runtime_error("QR ticket too large");
          std::map<std::string, std::string> headers;
          unsigned challenges = 0;
          bool qr_gateway_open = true;
          for (;;) {
            if (stopping_) return;
            auto response = https("POST",
                "https://discord.com/api/v10/users/@me/remote-auth/login",
                Json{{"ticket", ticket}}.dump(), {}, headers);
            for (auto &[name, value] : headers)
              secure_erase(value.data(), value.size());
            headers.clear();
            if (stopping_) return;
            if (response.status == 429)
              throw std::runtime_error("Discord login rate limit; wait before linking again");
            auto result = Json::parse(response.body);
            if (response.status == 200 && result.contains("encrypted_token")) {
              auto plain = rsa_decrypt(raw, result.at("encrypted_token"));
              token.assign(plain.begin(), plain.end());
              secure_erase(plain.data(), plain.size());
              break;
            }
            if (!result.contains("captcha_key") && !result.contains("captcha_sitekey"))
              throw std::runtime_error("Discord rejected the confirmed QR login (HTTP " +
                                       std::to_string(response.status) + ")");
            if (++challenges > 3)
              throw std::runtime_error("Discord did not accept verification; link again later");
            verification_.issue(result, deadline);
            {
              std::lock_guard lock(mutex_);
              qr_url_.clear();
              state_ = "verification_required";
              error_.clear();
            }
            // A user-completed response permits one exchange of this ticket.
            while (!stopping_ && monotonic_ms() < deadline) {
              if (auto submitted = verification_.take(monotonic_ms())) {
                headers = std::move(*submitted);
                break;
              }
              if (qr_gateway_open) {
                try { service_gateway(); }
                catch (const WebSocketClose &closed) {
                  if (closed.code() != 1000 && closed.code() != 1001) throw;
                  qr_gateway_open = false;
                  ws.close();
                }
              } else std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            if (stopping_) return;
            if (headers.empty())
              throw std::runtime_error("Login verification expired; link again");
            set_state("verifying");
          }
          break;
        } else if (op == "cancel")
          throw std::runtime_error("Account linking cancelled on your phone");
      }
      ws.close();
      verification_.clear();
      if (stopping_)
        return;
      if (token.size() < 20 || token.size() > 4096)
        throw std::runtime_error("QR code expired; link again");
      {
        std::lock_guard lock(mutex_);
        token_ = std::move(token);
        qr_url_.clear();
        state_ = "connecting";
      }
      gateway();
    } catch (const std::exception &e) {
      verification_.clear();
      {
        std::lock_guard lock(mutex_);
        qr_url_.clear();
      }
      if (!stopping_)
        set_state("error", e.what());
    }
    std::unique_ptr<VoiceSession> s, c;
    {
      std::lock_guard lock(mutex_);
      s = std::move(stream_);
      c = std::move(call_);
    }
    s.reset();
    c.reset();
  });
}
void Controller::bootstrap(const Json &data) {
  Json guilds = Json::array(), dms = Json::array();
  std::map<std::string, std::string> users;
  auto user = data.at("user");
  auto self = id_of(user);
  if (self.empty())
    throw std::runtime_error("Discord account identity missing");
  if (data.contains("users"))
    for (auto &u : data["users"]) {
      auto id = id_of(u);
      if (!id.empty())
        users[id] = u.value("global_name", Json(nullptr)).is_string()
                        ? u["global_name"].get<std::string>()
                        : u.value("username", id);
    }
  if (data.contains("guilds"))
    for (auto &g : data["guilds"]) {
      auto guild = discovered_guild(g);
      if (guild.is_null() || guilds.size() >= 4096)
        continue;
      guilds.push_back(std::move(guild));
    }
  if (data.contains("private_channels"))
    for (auto &c : data["private_channels"]) {
      int type = c.value("type", -1);
      auto id = id_of(c);
      if (id.empty() || (type != 1 && type != 3) ||
          c.value("is_message_request", false) || dms.size() >= 4096)
        continue;
      std::string name = c.value("name", Json(nullptr)).is_string()
                             ? c["name"].get<std::string>()
                             : "";
      if (name.empty() && c.contains("recipients")) {
        for (auto &r : c["recipients"]) {
          auto rid = r.is_string() ? r.get<std::string>() : id_of(r);
          if (rid == self)
            continue;
          std::string recipient = users.contains(rid) ? users[rid] : rid;
          if (r.is_object())
            recipient = r.value("username", recipient);
          if (!name.empty())
            name += ", ";
          name += recipient;
        }
      }
      if (name.empty() && c.contains("recipient_ids"))
        for (auto &r : c["recipient_ids"]) {
          auto rid = r.get<std::string>();
          if (rid != self) {
            if (!name.empty())
              name += ", ";
            name += users.contains(rid) ? users[rid] : rid;
          }
        }
      if (name.empty())
        name = "Direct call " + id;
      dms.push_back(
          {{"id", id}, {"name", name}, {"kind", type == 3 ? "group" : "dm"}});
    }
  // Authentication succeeded. Save once for this login, outside the state
  // mutex so password derivation and disk writes cannot block status polling.
  bool remember;
  { std::lock_guard lock(mutex_); remember = remember_login_; remember_login_ = false; }
  std::string save_error;
  if (remember) {
    auto token = token_copy();
    try { account_store_.save(token); }
    catch (const std::exception &e) { save_error = e.what(); }
    secure_erase(token.data(), token.size());
  }
  std::lock_guard lock(mutex_);
  account_save_error_ = std::move(save_error);
  channels_.clear();
  account_ = {{"id", self}, {"username", user.value("username", self)}};
  guilds_ = std::move(guilds);
  dms_ = std::move(dms);
  state_ = "ready";
  error_.clear();
}
Json Controller::api(const std::string &path, const Json &body) {
  std::lock_guard operation(operation_);
  if (path == "/api/status")
    return status();
  if (path == "/api/activity") {
    if (body.contains("enabled")) {
      if (body.size() != 1 || !body["enabled"].is_boolean())
        throw std::runtime_error("Activity setting must be true or false");
      activity_settings_.set_enabled(body["enabled"].get<bool>());
    } else if (!body.empty()) {
      throw std::runtime_error("Expected the activity enabled setting");
    }
    std::lock_guard lock(mutex_);
    if (!activity_settings_.enabled()) activity_game_.reset();
    return {{"enabled", activity_settings_.enabled()},
            {"game", activity_game_ ? Json{{"title_id", activity_game_->title_id},
                                          {"name", activity_game_->name},
                                          {"image_url", activity_game_->image_url}} : Json(nullptr)}};
  }
  if (path == "/api/probe") {
    {
      std::lock_guard lock(mutex_);
      if (!target_.is_null())
        throw std::runtime_error("Stop the stream before a capture probe");
    }
    capture_feed().flush();
    const auto deadline = monotonic_ms() + 4000;
    while (monotonic_ms() < deadline) {
      if (auto frame = capture_feed().take_video(); frame && frame->keyframe) {
        auto info = capture_feed().info();
        if (frame->generation != info.generation) continue;
        return {{"width", info.width}, {"height", info.height},
                {"fps", info.fps}, {"h264", base64(frame->data)}};
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    throw std::runtime_error("No capture keyframe arrived");
  }
  if (path == "/api/link") {
    if (body.contains("remember") && !body["remember"].is_boolean())
      throw std::runtime_error("Remember account must be true or false");
    link(body.value("remember", true));
    return {{"ok", true}};
  }
  if (path == "/api/verify") {
    verification_.submit(body.at("challenge_id").get<std::string>(),
                         body.at("solution").get<std::string>(), monotonic_ms());
    return {{"ok", true}};
  }
  if (path == "/api/cancel-link") {
    {
      std::lock_guard lock(mutex_);
      if (state_ != "linking" && state_ != "verification_required" && state_ != "verifying")
        throw std::runtime_error("No account link is in progress");
    }
    stop();
    {
      std::lock_guard lock(mutex_);
      qr_url_.clear();
      state_ = "disconnected";
      error_.clear();
    }
    return {{"ok", true}};
  }
  if (path == "/api/unlock") {
    auto token =
        load_vault(directory_ + "/account.vault", body.at("passphrase"));
    if (body.contains("remember") && !body["remember"].is_boolean()) {
      secure_erase(token.data(), token.size());
      throw std::runtime_error("Remember account must be true or false");
    }
    connect_account(std::move(token), body.value("remember", true));
    return {{"ok", true}};
  }
  if (path == "/api/vault") {
    {
      std::lock_guard lock(mutex_);
      if (account_.is_null())
        throw std::runtime_error("Wait for Discord to validate the account");
    }
    auto token = token_copy();
    if (token.empty())
      throw std::runtime_error("Connect an account first");
    try {
      if (body.contains("automatic") && !body["automatic"].is_boolean())
        throw std::runtime_error("Automatic sign-in must be true or false");
      if (body.value("automatic", false)) account_store_.save(token);
      else save_vault(directory_ + "/account.vault", body.at("passphrase"), token);
      secure_erase(token.data(), token.size());
    } catch (const std::exception &e) {
      secure_erase(token.data(), token.size());
      { std::lock_guard lock(mutex_); account_save_error_ = e.what(); }
      throw;
    } catch (...) {
      secure_erase(token.data(), token.size());
      throw;
    }
    { std::lock_guard lock(mutex_); account_save_error_.clear(); }
    return {{"ok", true}};
  }
  if (path == "/api/logout") {
    stop();
    if (body.value("forget", false)) account_store_.forget();
    std::lock_guard lock(mutex_);
    secure_erase(token_.data(), token_.size());
    token_.clear();
    account_ = nullptr;
    guilds_ = Json::array();
    dms_ = Json::array();
    channels_.clear();
    state_ = "disconnected";
    error_.clear();
    qr_url_.clear();
    target_ = nullptr;
    remember_login_ = false;
    account_save_error_.clear();
    if (body.value("forget", false))
      std::remove((directory_ + "/account.vault").c_str());
    return {{"ok", true}};
  }
  if (path == "/api/channels") {
    auto guild = id_of(body, "guild_id");
    const bool refresh = body.value("refresh", false);
    std::uint64_t revision;
    {
      std::lock_guard lock(mutex_);
      bool found = false;
      for (auto &g : guilds_)
        if (g["id"] == guild)
          found = true;
      if (!found)
        throw std::runtime_error("Choose one of your servers");
      const auto now = monotonic_ms();
      if (auto cached = channels_.lookup(guild, now, refresh))
        return *cached;
      if (channels_.cooling_down(now))
        throw std::runtime_error("Discord channel rate limit; wait before refreshing channels");
      revision = channels_.revision();
    }
    auto token = token_copy();
    if (token.empty())
      throw std::runtime_error("Connect an account first");
    HttpResponse response;
    try {
      response = https("GET", "https://discord.com/api/v10/guilds/" + guild +
                                  "/channels", "", token);
      secure_erase(token.data(), token.size());
    } catch (...) {
      secure_erase(token.data(), token.size());
      throw;
    }
    if (response.status == 429) {
      std::lock_guard lock(mutex_);
      if (channels_.revision() == revision)
        channels_.rate_limited(response, monotonic_ms());
      throw std::runtime_error("Discord channel rate limit; wait before refreshing channels");
    }
    auto remote = parse_api(std::move(response));
    auto out = discovered_voice_channels(remote);
    std::lock_guard lock(mutex_);
    if (channels_.revision() != revision)
      throw std::runtime_error("Server list changed; select the server again");
    channels_.success(guild, out, monotonic_ms());
    return out;
  }
  if (path == "/api/start") {
    const auto &destination = body.contains("target") ? body.at("target") : body;
    auto channel = id_of(destination, "channel_id");
    std::string guild = destination.value("guild_id", Json(nullptr)).is_null()
                            ? ""
                            : id_of(destination, "guild_id");
    std::lock_guard lock(mutex_);
    if (account_.is_null())
      throw std::runtime_error("Connect an account first");
    if (state_ != "ready" && state_ != "error")
      throw std::runtime_error("A call is already starting or streaming");
    bool found = false;
    if (guild.empty()) {
      for (auto &c : dms_)
        if (c["id"] == channel)
          found = true;
    } else if (const auto *choices = channels_.previous(guild)) {
      for (const auto &c : *choices)
        if (c["id"] == channel)
          found = true;
    }
    if (!found)
      throw std::runtime_error("Choose a discovered voice destination");
    if (!capture_feed().info().running)
      throw std::runtime_error("PS5 capture is not ready");
    target_ = {{"channel_id", channel},
               {"guild_id", guild.empty() ? Json(nullptr) : Json(guild)}};
    commands_.push_back({{"op", "start"}, {"target", target_}});
    state_ = "joining";
    error_.clear();
    return {{"ok", true}};
  }
  if (path == "/api/stop") {
    std::lock_guard lock(mutex_);
    commands_.push_back({{"op", "stop"}});
    return {{"ok", true}};
  }
  throw std::runtime_error("Unknown control operation");
}
void Controller::gateway() {
  auto token = token_copy();
  WebSocket ws;
  ws.connect("wss://gateway.discord.gg/?v=10&encoding=json");
  auto send = [&](int op, const Json &d) {
    ws.text(Json{{"op", op}, {"d", d}}.dump());
  };
  std::int64_t sequence = -1;
  std::string self, session, voice_session, voice_endpoint, voice_token,
      stream_server, stream_endpoint, stream_token, stream_key;
  std::string channel, guild;
  std::vector<std::string> members;
  bool want_call = false, want_stream = false, stream_requested = false, heartbeat_ack = true;
  std::uint64_t interval = 0, next_heartbeat = 0, join_deadline = 0,
                ready_deadline = monotonic_ms() + 30000;
  std::uint64_t next_activity_poll = 0;
  Json last_activities = Json::array();
  auto clear = [&](bool leave) {
    std::unique_ptr<VoiceSession> s, c;
    {
      std::lock_guard lock(mutex_);
      s = std::move(stream_);
      c = std::move(call_);
      target_ = nullptr;
    }
    s.reset();
    c.reset();
    if (!stream_key.empty()) {
      send(19, {{"stream_key", stream_key}});
      stream_key.clear();
    }
    if (leave)
      send(4, {{"guild_id", guild.empty() ? Json(nullptr) : Json(guild)},
               {"channel_id", nullptr},
               {"self_mute", true},
               {"self_deaf", true},
               {"self_video", false}});
    want_call = want_stream = stream_requested = false;
    members.clear();
    voice_session.clear();
    voice_endpoint.clear();
    voice_token.clear();
    stream_server.clear();
    stream_endpoint.clear();
    stream_token.clear();
    capture_feed().flush();
  };
  auto finish_stream = [&](const std::string &error) {
    clear(true);
    set_state("ready", error);
  };
  try {
    while (!stopping_) {
      auto now = monotonic_ms();
      if (self.empty() && now > ready_deadline)
        throw std::runtime_error("Discord account connection timed out");
      if (interval && now >= next_heartbeat) {
        if (!heartbeat_ack)
          throw std::runtime_error(
              "Discord account connection lost; reconnect the account");
        send(1, sequence < 0 ? Json(nullptr) : Json(sequence));
        heartbeat_ack = false;
        next_heartbeat = now + interval;
      }
      Json command;
      {
        std::lock_guard lock(mutex_);
        if (!commands_.empty()) {
          command = std::move(commands_.front());
          commands_.pop_front();
        }
      }
      if (!command.is_null()) {
        auto op = command.at("op");
        if (op == "stop") {
          clear(true);
          set_state("ready");
        } else if (op == "start") {
          auto target = command.at("target");
          channel = target.at("channel_id");
          guild = target.at("guild_id").is_null()
                      ? ""
                      : target["guild_id"].get<std::string>();
          if (!want_call && guild.empty()) {
            try {
              rest("GET", "/channels/" + channel + "/call");
            } catch (const std::exception &) { /* Joining a new call allocates
                                                  it through voice state. */
            }
          }
          send(4, {{"guild_id", guild.empty() ? Json(nullptr) : Json(guild)},
                   {"channel_id", channel},
                   {"self_mute", true},
                   {"self_deaf", true},
                   {"self_video", false}});
          want_call = true;
          want_stream = true;
          join_deadline = now + 30000;
          if (members.empty()) members = {self};
        }
      }
      // Metadata reads and presence updates run outside the media sender.
      // Poll at most once per five seconds and publish only changed activity.
      if (!self.empty() && now >= next_activity_poll) {
        next_activity_poll = now + 5000;
        auto game = activity_settings_.enabled() ? platform_running_game()
                                                : std::optional<GameTitle>{};
        if (game && activity_settings_.enabled())
          game->image_url = game_artwork_.image(*game);
        bool enabled;
        {
          std::lock_guard lock(mutex_);
          enabled = activity_settings_.enabled();
          if (!enabled) game.reset();
          activity_game_ = game;
        }
        auto presence = game_activity_presence(enabled, game);
        if (presence.at("activities") != last_activities) {
          send(3, presence);
          last_activities = presence.at("activities");
        }
      }
      if (auto message = ws.receive(now + 20)) {
        if (message->binary)
          throw std::runtime_error(
              "Unexpected compressed account Gateway message");
        auto packet = Json::parse(message->data);
        int op = packet.at("op");
        if (packet.contains("s") && !packet["s"].is_null())
          sequence = packet["s"].get<std::int64_t>();
        auto &d = packet["d"];
        if (op == 10) {
          interval = d.at("heartbeat_interval");
          if (interval < 1000 || interval > 120000)
            throw std::runtime_error("Invalid account heartbeat");
          next_heartbeat = now + interval;
          send(2, {{"token", token},
                   {"capabilities", 16381},
                   {"properties",
                    {{"os", "Linux"},
                     {"browser", "PBS Discord"},
                     {"device", "PS5"}}},
                   {"compress", false},
                   {"presence",
                    {{"status", "online"},
                     {"since", 0},
                     {"activities", Json::array()},
                     {"afk", false}}},
                   {"client_state", {{"guild_versions", Json::object()}}}});
        } else if (op == 11)
          heartbeat_ack = true;
        else if (op == 1)
          send(1, sequence < 0 ? Json(nullptr) : Json(sequence));
        else if (op == 7 || op == 9)
          throw std::runtime_error(
              "Discord requested a fresh account connection");
        else if (op == 0) {
          auto event = packet.value("t", "");
          if (event == "READY") {
            bootstrap(d);
            self = id_of(d.at("user"));
            session = d.at("session_id");
          } else if ((event == "VOICE_STATE_UPDATE" ||
                      event == "VOICE_STATE_UPDATE_BATCH") &&
                     want_call) {
            auto states = event == "VOICE_STATE_UPDATE_BATCH"
                              ? d.at("voice_states")
                              : Json::array({d});
            for (auto &state : states) {
              auto user = id_of(state, "user_id");
              auto cid = id_of(state, "channel_id");
              if (cid == channel) {
                if (std::find(members.begin(), members.end(), user) ==
                    members.end())
                  members.push_back(user);
                if (user == self) {
                  voice_session = state.at("session_id");
                }
              } else if (user == self && !voice_session.empty()) {
                clear(false);
                set_state("ready", "Discord moved or ended the call");
              }
            }
          } else if (event == "VOICE_SERVER_UPDATE" && want_call) {
            bool match = guild.empty() ? (!d.contains("channel_id") ||
                                          id_of(d, "channel_id") == channel)
                                       : id_of(d, "guild_id") == guild;
            if (match && !d.value("endpoint", Json(nullptr)).is_null()) {
              voice_endpoint = d.at("endpoint");
              voice_token = d.at("token");
            }
          } else if ((event == "STREAM_CREATE" || event == "STREAM_UPDATE") &&
                     want_stream) {
            auto expected = guild.empty()
                                ? "call:" + channel + ":" + self
                                : "guild:" + guild + ":" + channel + ":" + self;
            if (d.value("stream_key", "") == expected) {
              stream_key = expected;
              stream_server = d.value("rtc_server_id", stream_server);
              if (d.contains("viewer_ids"))
                for (auto &id : d["viewer_ids"])
                  if (id.is_string())
                    members.push_back(id.get<std::string>());
            }
          } else if (event == "STREAM_SERVER_UPDATE" && want_stream) {
            auto expected = guild.empty()
                                ? "call:" + channel + ":" + self
                                : "guild:" + guild + ":" + channel + ":" + self;
            if (d.value("stream_key", "") == expected &&
                !d.value("endpoint", Json(nullptr)).is_null()) {
              stream_endpoint = d.at("endpoint");
              stream_token = d.at("token");
            }
          } else if (event == "STREAM_DELETE" &&
                     d.value("stream_key", "") == stream_key &&
                     !stream_key.empty()) {
            finish_stream("Discord ended the stream");
          } else if (event == "GUILD_DELETE") {
            const auto id = id_of(d);
            std::lock_guard lock(mutex_);
            channels_.erase(id);
            if (!d.value("unavailable", false))
              guilds_.erase(
                  std::remove_if(guilds_.begin(), guilds_.end(),
                                 [&](const Json &v) { return v["id"] == id; }),
                  guilds_.end());
          } else if (event == "CHANNEL_DELETE") {
            auto id = id_of(d);
            std::lock_guard lock(mutex_);
            const auto guild_id = id_of(d, "guild_id");
            if (!guild_id.empty())
              channels_.erase(guild_id);
            auto &list = dms_;
            list.erase(
                std::remove_if(list.begin(), list.end(),
                               [&](const Json &v) { return v["id"] == id; }),
                list.end());
          }
        }
      }
      if (want_call) {
        bool have_call;
        {
          std::lock_guard lock(mutex_);
          have_call = call_ != nullptr;
        }
        if (!have_call && !voice_session.empty() && !voice_endpoint.empty() &&
            !voice_token.empty()) {
          RtcParams p{guild.empty() ? channel : guild,
                      channel,
                      self,
                      voice_session,
                      voice_token,
                      voice_endpoint,
                      members,
                      false
          };
          auto call = std::make_unique<VoiceSession>(std::move(p), identity_);
          {
            std::lock_guard lock(mutex_);
            call_ = std::move(call);
          }
          have_call = true;
        }
        if (want_stream && have_call && !stream_requested) {
          send(18, {{"type", guild.empty() ? "call" : "guild"},
                    {"guild_id", guild.empty() ? Json(nullptr) : Json(guild)},
                    {"channel_id", channel},
                    {"preferred_region", nullptr}});
          auto key = guild.empty()
                         ? "call:" + channel + ":" + self
                         : "guild:" + guild + ":" + channel + ":" + self;
          send(22, {{"stream_key", key}, {"paused", false}});
          stream_requested = true;
          set_state("negotiating");
        }
        bool have_stream;
        {
          std::lock_guard lock(mutex_);
          have_stream = stream_ != nullptr;
        }
        if (want_stream && !have_stream && !stream_server.empty() &&
            !stream_endpoint.empty() && !stream_token.empty() &&
            !voice_session.empty()) {
          auto rtc = std::stoull(stream_server);
          if (!rtc)
            throw std::runtime_error("Invalid Discord stream allocation");
          RtcParams p{
              stream_server, std::to_string(rtc - 1), self,    voice_session,
              stream_token,  stream_endpoint,         members, true};
          auto stream = std::make_unique<VoiceSession>(std::move(p), identity_);
          std::lock_guard lock(mutex_);
          stream_ = std::move(stream);
        }
        std::string call_error, stream_error;
        bool rtc_connected = false, ready = false;
        {
          std::lock_guard lock(mutex_);
          if (call_) {
            call_error = call_->error();
          }
          if (stream_) {
            auto state = stream_->status();
            rtc_connected = state["connected"];
            ready = state["dave_ready"] &&
                    state["video_packets"].get<std::uint64_t>() > 0;
            stream_error = stream_->error();
          }
        }
        if (!call_error.empty()) {
          clear(true);
          set_state("ready", call_error);
        } else if (!stream_error.empty()) {
          finish_stream(stream_error);
        } else if (ready)
          set_state("streaming");
        else if (!rtc_connected && now > join_deadline) {
          finish_stream("Discord call/stream negotiation timed out");
        }
      }
    }
  } catch (const WebSocketClose &closed) {
    secure_erase(token.data(), token.size());
    if (closed.code() == 4004)
      throw std::runtime_error("Discord rejected account authentication (Gateway close 4004). "
                               "The saved session may be expired or revoked; link the account again.");
    throw std::runtime_error(std::string("Discord account Gateway: ") + closed.what());
  } catch (...) {
    secure_erase(token.data(), token.size());
    throw;
  }
  try {
    if (!last_activities.empty()) send(3, game_activity_presence(false, {}));
    clear(true);
  } catch (...) {
  }
  secure_erase(token.data(), token.size());
  ws.close();
}
} // namespace pbsd
