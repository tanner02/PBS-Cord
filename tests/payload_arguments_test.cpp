// SPDX-License-Identifier: GPL-3.0-or-later
#include "payload_arguments.hpp"
#include <cassert>
#include <iostream>
#include <string>
#include <vector>
static pbsd::PayloadArguments parse(std::vector<std::string> arguments) {
  std::vector<char *> argv;
  for (auto &argument : arguments) argv.push_back(argument.data());
  return pbsd::parse_payload_arguments(static_cast<int>(argv.size()), argv.data());
}
int main() {
  for (bool program_name : {false, true}) {
    std::vector<std::string> arguments{"--capture", "1440p", "--bitrate", "12000000",
      "--tile-port", "9998", "fps=30", "noaudio"};
    if (program_name) arguments.insert(arguments.begin(), "/data/pbs-discord.elf");
    auto options = parse(arguments);
    assert(options.capture_profile == "1440p");
    assert(options.capture_bitrate == 12000000 && options.tile_port == 9998);
    assert((options.passthrough == std::vector<std::string>{"fps=30", "noaudio"}));
  }
  assert(parse({"--capture", "1080p"}).capture_profile == "1080p");
  auto defaults = parse({"pbs-discord"});
  assert(defaults.capture_profile == "720p" && defaults.capture_bitrate == 4000000);
  assert(parse({}).tile_port == 9878);
  auto legacy = parse({"--data", "/data/old", "--port", "1234", "--address", "127.0.0.1"});
  assert(legacy.passthrough.empty());
  std::cout << "payload arguments: normal and loader argv layouts pass\n";
}
