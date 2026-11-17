// SPDX-License-Identifier: GPL-3.0-or-later
#include "net.hpp"
#include <cassert>
#include <iostream>
#include <string>

int main() {
  using namespace pbsd;
  const std::string host = "c-fra13-2c7452c4.discord.media";
  for (const auto &input : {host, host + "/", "wss://" + host,
                            "wss://" + host + "/", "wss://" + host + "/?v=8"})
    assert(media_gateway_endpoint(input) == "wss://" + host + ":443/?v=8");
  for (const int port : {1, 80, 443, 800, 2087, 8080, 8443, 65535}) {
    const auto authority = host + ":" + std::to_string(port);
    const auto expected = "wss://" + host + ":" +
                          std::to_string(port == 80 ? 443 : port) + "/?v=8";
    assert(media_gateway_endpoint(authority) == expected);
    assert(media_gateway_endpoint("wss://" + authority) == expected);
    assert(media_gateway_endpoint("wss://" + authority + "/") == expected);
    const auto parsed = Url::parse(expected);
    assert(parsed.authority() ==
           host + ((port == 80 || port == 443) ? "" : ":" + std::to_string(port)));
  }
  assert(media_gateway_endpoint("WSS-HOST.DISCORD.MEDIA:2087") ==
         "wss://wss-host.discord.media:2087/?v=8");
  for (const std::string trusted : {"discord.media", "discord.gg", "discord.com",
                                    "voice.discord.gg", "voice.discord.com",
                                    "voice.discordapp.com", "voice.discordapp.net",
                                    "nested.voice.discord.media"}) {
    assert(media_host(trusted));
    assert(Url::parse(media_gateway_endpoint(trusted)).host == trusted);
  }
  for (const std::string invalid : {
           "", "discord.media.evil.example", "evildiscord.media",
           "discord.gg.evil.example", "evildiscord.gg", "evildiscord.com",
           "evildiscordapp.com", "discordapp.net.evil.example", "127.0.0.1:2087",
           "wss://user@voice.discord.media:2087",
           "wss://voice.discord.media@evil.example:2087",
           "ws://voice.discord.media:2087", "http://voice.discord.media",
           "https://voice.discord.media", "voice.discord.media:0",
           "voice.discord.media:-1", "voice.discord.media:65536",
           "voice.discord.media:999999999999999999999999999999",
           "voice.discord.media:", "voice.discord.media:2087junk",
           "voice.discord.media:2087:443", "voice.discord.media: 2087",
           "voice.discord.media\r\n", "voice.discord.media\t",
           "wss://voice.discord.media/other", "wss://voice.discord.media/#fragment"}) {
    bool rejected = false;
    try {
      (void)media_gateway_endpoint(invalid);
    } catch (const std::exception &) {
      rejected = true;
    }
    assert(rejected);
  }
  std::cout << "Media endpoint normalization and trust checks passed\n";
}
