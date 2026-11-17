// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "net.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <nlohmann/json.hpp>

namespace pbsd {
// Access under Controller::mutex_. No clock or network work occurs here.
class ChannelCache {
public:
  using Json = nlohmann::json;
  std::optional<Json> lookup(const std::string &guild, std::uint64_t now,
                             bool refresh = false) const {
    const auto it = entries_.find(guild);
    if (!refresh && it != entries_.end() && now < it->second.expires)
      return std::optional<Json>(std::in_place, it->second.channels);
    return std::nullopt;
  }
  const Json *previous(const std::string &guild) const {
    const auto it = entries_.find(guild);
    return it == entries_.end() ? nullptr : &it->second.channels;
  }
  bool cooling_down(std::uint64_t now) const { return now < retry_at_; }
  void success(const std::string &guild, Json channels, std::uint64_t now) {
    entries_[guild] = {std::move(channels), after(now, 60000)};
  }
  void rate_limited(const HttpResponse &response, std::uint64_t now) {
    double seconds = 0;
    const auto header = response.headers.find("retry-after");
    if (header != response.headers.end()) {
      try {
        std::size_t consumed = 0;
        const double value = std::stod(header->second, &consumed);
        if (consumed == header->second.size() && std::isfinite(value) && value > 0)
          seconds = value;
      } catch (const std::exception &) {
      }
    }
    const auto body = Json::parse(response.body, nullptr, false);
    if (body.is_object() && body.contains("retry_after") &&
        body["retry_after"].is_number()) {
      const double value = body["retry_after"].get<double>();
      if (std::isfinite(value) && value > 0)
        seconds = std::max(seconds, value);
    }
    // Missing or malformed retry hints still prevent repeated polling.
    const auto max = std::numeric_limits<std::uint64_t>::max();
    const double milliseconds = seconds > 0 ? std::ceil(seconds * 1000) : 60000;
    const auto delay = milliseconds >= static_cast<double>(max)
                           ? max : static_cast<std::uint64_t>(milliseconds);
    retry_at_ = std::max(retry_at_, after(now, delay));
  }
  std::uint64_t revision() const { return revision_; }
  void clear() {
    entries_.clear();
    retry_at_ = 0;
    ++revision_;
  }
  void erase(const std::string &guild) {
    entries_.erase(guild);
    ++revision_;
  }

private:
  static std::uint64_t after(std::uint64_t now, std::uint64_t delay) {
    const auto max = std::numeric_limits<std::uint64_t>::max();
    return delay > max - now ? max : now + delay;
  }
  struct Entry { Json channels; std::uint64_t expires; };
  std::map<std::string, Entry> entries_;
  std::uint64_t retry_at_{}, revision_{};
};
} // namespace pbsd
