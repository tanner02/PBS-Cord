// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "game_activity.hpp"
#include <atomic>
#include <mutex>
#include <nlohmann/json.hpp>

namespace pbsd {
class ActivitySettings {
public:
  explicit ActivitySettings(const std::string &directory);
  bool enabled() const { return enabled_.load(); }
  void set_enabled(bool enabled);

private:
  std::string directory_;
  std::atomic<bool> enabled_{false};
  std::mutex mutex_;
};
nlohmann::json game_activity_presence(bool enabled,
                                      const std::optional<GameTitle> &game);
} // namespace pbsd
