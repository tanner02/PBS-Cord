// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>
#include <vector>
namespace pbsd {
struct PayloadArguments {
  std::string capture_profile = "720p";
  unsigned long capture_bitrate = 4000000;
  int tile_port = 9878;
  std::vector<std::string> passthrough;
};
inline PayloadArguments parse_payload_arguments(int argc, char *const *argv) {
  PayloadArguments result;
  // Some loaders supply their first flag as argv[0], without a program name.
  const int first = argc > 0 && argv && argv[0] && argv[0][0] == '-' ? 0 : 1;
  for (int i = first; i < argc; ++i) {
    const std::string argument = argv[i];
    if (argument == "--capture" && i + 1 < argc) result.capture_profile = argv[++i];
    else if (argument == "--bitrate" && i + 1 < argc) result.capture_bitrate = std::stoul(argv[++i]);
    else if (argument == "--tile-port" && i + 1 < argc) result.tile_port = std::stoi(argv[++i]);
    else if (argument == "--data" || argument == "--port" || argument == "--address")
      ++i; // Retain compatibility with old dashboard options.
    else result.passthrough.push_back(argument);
  }
  return result;
}
} // namespace pbsd
