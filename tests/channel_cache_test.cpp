// SPDX-License-Identifier: GPL-3.0-or-later
#include "channel_cache.hpp"
#include "destination_metadata.hpp"
#include <cassert>
#include <iostream>
#include <stdexcept>

using namespace pbsd;
using Json = nlohmann::json;

int main() {
  const std::string icon(32, 'a');
  auto guild = discovered_guild({{"id", "1"}, {"name", "Server"}, {"icon", icon}});
  assert(guild.at("id") == "1" && guild.at("name") == "Server");
  assert(guild.at("icon") == icon);
  guild = discovered_guild({{"id", "1"}, {"properties", {
      {"name", "Nested server"}, {"icon", "a_" + icon}}}});
  assert(guild.at("name") == "Nested server" && guild.at("icon") == "a_" + icon);
  guild = discovered_guild({{"id", "1"}, {"icon", icon},
                            {"properties", {{"name", nullptr}}}});
  assert(guild.at("name") == "1" && guild.at("icon") == icon);
  for (const Json &invalid : {Json(nullptr), Json(4), Json(""),
                             Json("https://example.com/icon.png"),
                             Json(std::string(32, 'g')), Json(icon + "/x")}) {
    guild = discovered_guild({{"id", "1"}, {"icon", invalid}});
    assert(!guild.contains("icon"));
  }
  assert(discovered_guild({{"id", "invalid"}}).is_null());
  // Voice entries precede their category in this REST fixture, and text/stage
  // entries share the parent. Only ordinary voice channels may be returned.
  const auto channels = discovered_voice_channels(Json::array({
      {{"id", "101"}, {"type", 2}, {"name", "Lounge"}, {"parent_id", "10"}, {"position", 3}},
      {{"id", "102"}, {"type", 0}, {"name", "chat"}, {"parent_id", "10"}},
      {{"id", "103"}, {"type", 13}, {"name", "Stage"}, {"parent_id", "10"}},
      {{"id", "104"}, {"type", 2}, {"name", "Uncategorized"}, {"parent_id", nullptr}},
      {{"id", "105"}, {"type", 2}, {"parent_id", "999"}, {"position", "bad"}},
      {{"id", "106"}, {"type", 2}, {"parent_id", "102"}, {"position", -1}},
      {{"id", "bad"}, {"type", 2}}, nullptr,
      {{"id", "10"}, {"type", 4}, {"name", "Hangouts"}, {"position", 2}}}));
  assert(channels.size() == 4);
  assert(channels[0] == Json({{"id", "101"}, {"name", "Lounge"}, {"kind", "voice"},
      {"position", 3}, {"category_id", "10"}, {"category_name", "Hangouts"},
      {"category_position", 2}}));
  for (std::size_t i = 1; i < channels.size(); ++i) {
    assert(channels[i].at("category_id").is_null());
    assert(channels[i].at("category_name").is_null());
    assert(channels[i].at("category_position").is_null());
    assert(channels[i].at("position").is_null());
  }
  assert(channels[2].at("name") == "105");
  assert(discovered_voice_channels(Json::array()).empty());
  bool invalid_list = false;
  try { discovered_voice_channels(Json::object()); }
  catch (const std::runtime_error &) { invalid_list = true; }
  assert(invalid_list);
  ChannelCache cache;
  std::uint64_t now = 1000;
  int requests = 0;
  const Json choices = channels; // Cached metadata survives TTL/refresh paths.
  HttpResponse next{200, {}, choices.dump()};
  // An offline request harness: clock and responses are entirely fixtures.
  auto read = [&](const std::string &guild, bool refresh = false) {
    if (auto cached = cache.lookup(guild, now, refresh))
      return *cached;
    if (cache.cooling_down(now))
      throw std::runtime_error("cooldown");
    ++requests;
    if (next.status == 429) {
      cache.rate_limited(next, now);
      throw std::runtime_error("rate limit");
    }
    if (next.status != 200)
      throw std::runtime_error("failure");
    auto data = Json::parse(next.body);
    cache.success(guild, data, now);
    return data;
  };
  auto rejects = [&](auto action) {
    bool failed = false;
    try { action(); } catch (const std::runtime_error &) { failed = true; }
    assert(failed);
  };
  assert(read("1") == choices && requests == 1);
  for (int i = 0; i < 30; ++i) {
    now += 1999;
    assert(read("1") == choices);
  }
  assert(requests == 1);
  now = 61000; // Success TTL expires at exactly 60 seconds.
  assert(read("1") == choices && requests == 2);
  assert(read("2") == choices && requests == 3); // Per-guild successes.
  assert(read("1", true) == choices && requests == 4);

  next.status = 500;
  rejects([&] { read("1", true); });
  assert(requests == 5 && *cache.previous("1") == choices);
  assert(read("1") == choices && requests == 5);
  now += 60000;
  rejects([&] { read("1"); });
  assert(*cache.previous("1") == choices); // Errors do not replace choices.

  next = {429, {{"retry-after", "2.5"}}, "{\"retry_after\":4.25}"};
  rejects([&] { read("1", true); });
  const auto attempts = requests;
  assert(*cache.previous("1") == choices);
  rejects([&] { read("1"); });
  rejects([&] { read("2", true); }); // Cooldown applies across the route.
  assert(requests == attempts);
  now += 4249;
  rejects([&] { read("1", true); });
  assert(requests == attempts);
  ++now;
  next = {200, {}, choices.dump()};
  assert(read("1", true) == choices && requests == attempts + 1);

  // A longer header is respected too; fractional delays round upward.
  cache.rate_limited({429, {{"retry-after", "3.0001"}},
                      "{\"retry_after\":1}"}, now);
  assert(cache.cooling_down(now + 3000));
  assert(!cache.cooling_down(now + 3001));
  // Fresh successes can still be displayed during cooldown without a request.
  const auto before_cached_read = requests;
  assert(read("1") == choices && requests == before_cached_read);
  rejects([&] { read("1", true); });

  const auto revision = cache.revision();
  cache.clear(); // Account change, bootstrap, linking, and logout all use clear.
  assert(cache.revision() != revision);
  assert(!cache.previous("1") && !cache.previous("2"));
  assert(!cache.cooling_down(now));
  assert(read("1") == choices && requests == before_cached_read + 1);
  cache.erase("1");
  assert(!cache.lookup("1", now) && !cache.previous("1"));

  for (const auto &response : {
         HttpResponse{429, {}, "invalid json"},
         HttpResponse{429, {{"retry-after", "NaN"}}, "{\"retry_after\":-1}"},
         HttpResponse{429, {{"retry-after", "1 garbage"}}, "{}"}}) {
    cache.clear();
    cache.rate_limited(response, now);
    assert(cache.cooling_down(now + 59999));
    assert(!cache.cooling_down(now + 60000));
  }
  cache.clear();
  cache.success("1", Json::array(), now); // A successful empty list is cacheable.
  assert(cache.lookup("1", now)->empty());
  std::cout << "Destination metadata, channel cache TTL and cooldown fixtures passed\n";
}
