// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <map>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <utility>

namespace pbsd {
namespace destination_metadata {
using Json = nlohmann::json;
inline std::string id_of(const Json &value, const char *key = "id") {
  const auto it = value.find(key);
  if (it == value.end() || !it->is_string())
    return {};
  auto id = it->get<std::string>();
  if (id.empty() || id.size() > 20 ||
      id.find_first_not_of("0123456789") != std::string::npos)
    return {};
  return id;
}
inline std::string name_of(const Json &value, const std::string &fallback) {
  const auto it = value.find("name");
  return it != value.end() && it->is_string() ? it->get<std::string>() : fallback;
}
inline std::string icon_of(const Json &value) {
  const auto it = value.find("icon");
  if (it == value.end() || !it->is_string())
    return {};
  auto icon = it->get<std::string>();
  const auto start = icon.starts_with("a_") ? 2u : 0u;
  if (icon.size() != start + 32 ||
      icon.find_first_not_of("0123456789abcdef", start) != std::string::npos)
    return {};
  return icon;
}
inline Json position_of(const Json &value) {
  const auto it = value.find("position");
  return it != value.end() && it->is_number_integer() && *it >= 0 &&
                 *it <= 2147483647
             ? *it
             : Json(nullptr);
}
} // namespace destination_metadata

// READY can place guild presentation fields inside properties.
inline nlohmann::json discovered_guild(const nlohmann::json &guild) {
  using namespace destination_metadata;
  const auto id = id_of(guild);
  if (id.empty())
    return nullptr;
  const auto properties = guild.find("properties");
  const bool nested = properties != guild.end() && properties->is_object();
  const auto name = nested ? name_of(*properties, name_of(guild, id))
                           : name_of(guild, id);
  auto icon = nested ? icon_of(*properties) : std::string{};
  if (icon.empty())
    icon = icon_of(guild);
  Json out{{"id", id}, {"name", name}};
  if (!icon.empty())
    out["icon"] = icon;
  return out;
}

// Use only the already fetched guild channel list. Categories describe voice
// choices; category, text and stage entries never become selectable choices.
inline nlohmann::json discovered_voice_channels(const nlohmann::json &remote) {
  using namespace destination_metadata;
  if (!remote.is_array())
    throw std::runtime_error("Invalid Discord channel list");
  std::map<std::string, const Json *> categories;
  for (const auto &channel : remote) {
    const auto id = id_of(channel);
    if (!id.empty() && channel.value("type", Json(nullptr)) == 4)
      categories[id] = &channel;
  }
  Json out = Json::array();
  for (const auto &channel : remote) {
    const auto id = id_of(channel);
    if (id.empty() || channel.value("type", Json(nullptr)) != 2 ||
        out.size() >= 4096)
      continue;
    Json choice{{"id", id}, {"name", name_of(channel, id)}, {"kind", "voice"},
                {"position", position_of(channel)}, {"category_id", nullptr},
                {"category_name", nullptr}, {"category_position", nullptr}};
    const auto category = categories.find(id_of(channel, "parent_id"));
    if (category != categories.end()) {
      choice["category_id"] = category->first;
      choice["category_name"] = name_of(*category->second, category->first);
      choice["category_position"] = position_of(*category->second);
    }
    out.push_back(std::move(choice));
  }
  return out;
}
} // namespace pbsd
