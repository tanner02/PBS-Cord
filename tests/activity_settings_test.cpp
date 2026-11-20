// SPDX-License-Identifier: GPL-3.0-or-later
#include "activity_settings.hpp"
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <sys/stat.h>

namespace fs = std::filesystem;
using namespace pbsd;
using Json = nlohmann::json;
namespace {
struct Fixture {
  fs::path root;
  Fixture() {
    char path[] = "/tmp/pbsd-activity-settings-test-XXXXXX";
    const auto created = mkdtemp(path);
    assert(created);
    root = created;
  }
  ~Fixture() { fs::remove_all(root); }
};
void write(const fs::path &path, const std::string &value) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(value.data(), value.size());
  assert(out.good());
}
std::string read(const fs::path &path) {
  std::ifstream in(path, std::ios::binary);
  assert(in.good());
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
template <typename F> void rejects(F action) {
  bool failed = false;
  try { action(); } catch (const std::runtime_error &) { failed = true; }
  assert(failed);
}
void no_temporary_files(const fs::path &directory) {
  for (const auto &entry : fs::directory_iterator(directory))
    assert(entry.path().filename().string().find(".activity-settings-") != 0);
}
} // namespace

int main() {
  Fixture fixture;
  const auto directory = fixture.root / "settings";
  const auto path = directory / "activity-settings.json";
  ActivitySettings settings(directory.string());
  assert(!settings.enabled() && !fs::exists(directory));
  settings.set_enabled(false);
  assert(!fs::exists(directory)); // Unchanged default does not create files.
  settings.set_enabled(true);
  assert(settings.enabled());
  assert(Json::parse(read(path)) == Json({{"enabled", true}}));
  assert(ActivitySettings(directory.string()).enabled());
  struct stat directory_stat{}, before{}, after{};
  assert(stat(directory.c_str(), &directory_stat) == 0);
  assert((directory_stat.st_mode & 0777) == 0700);
  assert(stat(path.c_str(), &before) == 0 && (before.st_mode & 0777) == 0600);
  settings.set_enabled(true);
  assert(stat(path.c_str(), &after) == 0);
  assert(before.st_ino == after.st_ino && before.st_size == after.st_size &&
         before.st_mtim.tv_sec == after.st_mtim.tv_sec &&
         before.st_mtim.tv_nsec == after.st_mtim.tv_nsec);
  settings.set_enabled(false);
  assert(!settings.enabled());
  assert(Json::parse(read(path)) == Json({{"enabled", false}}));
  assert(!ActivitySettings(directory.string()).enabled());
  no_temporary_files(directory);

  for (const auto &invalid : {
         std::string(), std::string("{"), std::string("null"), std::string("[]"),
         std::string("true"), std::string("{}"), std::string("{\"enabled\":1}"),
         std::string("{\"enabled\":\"true\"}"), std::string("{\"enabled\":null}"),
         std::string("{\"enabled\":{\"value\":true}}"),
         std::string("{\"enabled\":[true]}"),
         std::string("{\"enabled\":false,\"enabled\":true}"),
         std::string("{\"enabled\":true,\"unexpected\":true}"),
         std::string("{\"enabled\":true}\n{\"enabled\":false}"),
         std::string(4097, ' ')}) {
    write(path, invalid);
    assert(!ActivitySettings(directory.string()).enabled());
  }
  std::string maximum = "{\"enabled\":true}";
  maximum.resize(4096, ' ');
  write(path, maximum);
  assert(ActivitySettings(directory.string()).enabled());
  maximum.push_back(' ');
  write(path, maximum);
  assert(!ActivitySettings(directory.string()).enabled());

  settings.set_enabled(true);
  const auto original = read(path);
  const auto moved = fixture.root / "preserved";
  fs::rename(directory, moved);
  write(directory, "not a directory");
  rejects([&] { settings.set_enabled(false); });
  assert(settings.enabled());
  assert(read(moved / "activity-settings.json") == original);
  fs::remove(directory);
  fs::rename(moved, directory);
  // Force rename failure even under root, independently of permission checks.
  fs::rename(path, directory / "saved.json");
  fs::create_directory(path);
  rejects([&] { settings.set_enabled(false); });
  assert(settings.enabled() && read(directory / "saved.json") == original);
  no_temporary_files(directory);
  fs::remove(path);
  fs::rename(directory / "saved.json", path);
  settings.set_enabled(false);
  assert(!settings.enabled() && !ActivitySettings(directory.string()).enabled());

  const GameTitle game{"PPSA12345", "Game \xf0\x9f\x8e\xae \xe6\x97\xa5\xe6\x9c\xac"};
  const Json empty{{"since", 0}, {"status", "online"},
                   {"activities", Json::array()}, {"afk", false}};
  assert(game_activity_presence(false, std::nullopt) == empty);
  assert(game_activity_presence(false, game) == empty);
  assert(game_activity_presence(true, std::nullopt) == empty);
  auto expected = empty;
  expected["activities"] = Json::array({{{"name", game.name}, {"type", 0}}});
  const auto presence = game_activity_presence(true, game);
  assert(presence == expected && presence.dump().find(game.title_id) == std::string::npos);
  auto with_art = game;
  with_art.image_url = "https://image.api.playstation.com/vulcan/test.png";
  const std::string asset_path = "external/discord_hash/https/image.api.playstation.com/vulcan/test.png";
  const auto asset = discord_game_image(Json::array({{{"external_asset_path", asset_path}}}));
  assert(asset == "mp:" + asset_path);
  expected["activities"][0]["application_id"] = game_activity_application_id;
  expected["activities"][0]["assets"] = {{"large_image", asset}, {"large_text", game.name}};
  assert(game_activity_presence(true, with_art, asset) == expected);
  assert(game_activity_presence(true, with_art) == presence);
  assert(game_activity_presence(false, with_art, asset) == empty);
  assert(discord_game_image(Json::array({{{"external_asset_path", "external/../x"}}})).empty());
  assert(discord_game_image(Json::array({{{"external_asset_path", with_art.image_url}}})).empty());
  assert(discord_game_image(Json::object()).empty());
  std::cout << "Activity opt-in persistence and presence fixtures passed\n";
}
