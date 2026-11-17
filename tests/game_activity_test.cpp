// SPDX-License-Identifier: GPL-3.0-or-later
#include "game_activity.hpp"
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <sys/stat.h>
#include <unistd.h>

using namespace pbsd;
using Json = nlohmann::json;
namespace fs = std::filesystem;
namespace {
struct Fixture {
  fs::path root;
  Fixture() {
    char path[] = "/tmp/pbsd-game-title-test-XXXXXX";
    const auto created = mkdtemp(path);
    assert(created);
    root = created;
    fs::create_directory(root / "PPSA12345");
    fs::create_directory(root / "CUSA12345");
  }
  ~Fixture() { fs::remove_all(root); }
  void write(const std::string &id, const std::string &name, const std::string &data) {
    std::ofstream output(root / id / name, std::ios::binary | std::ios::trunc);
    output.write(data.data(), data.size());
    assert(output.good());
  }
  std::optional<GameTitle> read(const std::string &id = "PPSA12345",
                              const std::string &locale = "en-US") {
    return read_game_metadata(root.string(), id, locale);
  }
};
void put32(std::string &bytes, std::size_t at, std::uint32_t value) {
  for (unsigned i = 0; i < 4; ++i)
    bytes[at + i] = static_cast<char>(value >> (8 * i));
}
// Independent PSF fixture with two UTF-8 entries and aligned data section.
std::string sfo(const std::string &id, const std::string &name) {
  std::string bytes(68, '\0');
  bytes.replace(0, 4, "\0PSF", 4);
  put32(bytes, 4, 0x101);
  put32(bytes, 8, 52);
  put32(bytes, 12, 68);
  put32(bytes, 16, 2);
  bytes.replace(52, 15, "TITLE_ID\0TITLE\0", 15);
  bytes[22] = bytes[38] = 4;
  bytes[23] = bytes[39] = 2;
  bytes[36] = 9;
  put32(bytes, 24, id.size() + 1);
  put32(bytes, 28, id.size() + 1);
  put32(bytes, 40, name.size() + 1);
  put32(bytes, 44, name.size() + 1);
  put32(bytes, 48, id.size() + 1);
  bytes += id;
  bytes += '\0';
  bytes += name;
  bytes += '\0';
  return bytes;
}
std::string sfo_content(const std::string &id, const std::string &name,
                        const std::string &content) {
  const auto base = sfo(id, name);
  std::string bytes(96, '\0');
  bytes.replace(0, 52, base, 0, 52);
  put32(bytes, 8, 68);
  put32(bytes, 12, 96);
  put32(bytes, 16, 3);
  bytes.replace(68, 26, "TITLE_ID\0TITLE\0CONTENT_ID\0", 26);
  bytes[52] = 15;
  bytes[54] = 4;
  bytes[55] = 2;
  put32(bytes, 56, content.size() + 1);
  put32(bytes, 60, content.size() + 1);
  put32(bytes, 64, base.size() - 68);
  bytes += base.substr(68);
  bytes += content;
  bytes += '\0';
  return bytes;
}
} // namespace

int main() {
  for (const auto *id : {"PPSA00000", "CUSA12345"}) assert(is_game_title_id(id));
  for (const auto *id : {"", "PPSA1234", "PPSA123456", "ppsa12345", "PPSA12A45",
                          "NPXS40140", "FAKE00000", "ITEM00001", "../PPSA12",
                          "PPSA1234/", "PPSA12345/../"})
    assert(!is_game_title_id(id));
  assert(!is_game_title_id(std::string("PPSA12345\0", 10)));
  assert(!platform_running_game()); // Host builds never query console services.

  Fixture fixture;
  const Json valid{
      {"titleId", "PPSA12345"},
      {"localizedParameters", {{"defaultLanguage", "ja-JP"},
                                {"en-US", {{"titleName", "English Game"}}},
                                {"ja-JP", {{"titleName", "Japanese Game"}}},
                                {"fr-FR", {{"titleName", "French Game"}}}}}};
  auto write_json = [&](const Json &value) {
    fixture.write("PPSA12345", "param.json", value.dump());
  };
  write_json(valid);
  auto title = fixture.read();
  assert(title && title->title_id == "PPSA12345" && title->name == "English Game");
  assert(title->content_id.empty() && title->concept_id.empty() && title->image_url.empty());
  assert(fixture.read("PPSA12345", "fr-FR")->name == "French Game");
  assert(fixture.read("PPSA12345", "de-DE")->name == "Japanese Game");
  auto english_fallback = valid;
  english_fallback["localizedParameters"]["defaultLanguage"] = "missing";
  write_json(english_fallback);
  assert(fixture.read("PPSA12345", "de-DE")->name == "English Game");
  auto invalid_locale = valid;
  invalid_locale["localizedParameters"]["fr-FR"]["titleName"] = 42;
  write_json(invalid_locale);
  assert(fixture.read("PPSA12345", "fr-FR")->name == "Japanese Game");

  const std::string ps5_content = "EP0001-PPSA12345_00-ABCDEFGHIJKLMNOP";
  auto with_identifiers = valid;
  with_identifiers["contentId"] = ps5_content;
  with_identifiers["conceptId"] = "123456789012";
  write_json(with_identifiers);
  title = fixture.read();
  assert(title && title->name == "English Game" && title->content_id == ps5_content &&
         title->concept_id == "123456789012" && title->image_url.empty());
  with_identifiers["contentId"] = "UP1234-PPSA12345_09-aBc0123456789___";
  with_identifiers["conceptId"] = "0";
  write_json(with_identifiers);
  assert(fixture.read()->content_id == with_identifiers["contentId"].get<std::string>());
  assert(fixture.read()->concept_id == "0");
  for (const Json &invalid : {
         Json(nullptr), Json(123), Json(true), Json::array(), Json::object(), Json(""),
         Json("ep0001-PPSA12345_00-ABCDEFGHIJKLMNOP"),
         Json("EP00A1-PPSA12345_00-ABCDEFGHIJKLMNOP"),
         Json("EP0001-PPSA54321_00-ABCDEFGHIJKLMNOP"),
         Json("EP0001-CUSA12345_00-ABCDEFGHIJKLMNOP"),
         Json("EP0001-PPSA12345_0A-ABCDEFGHIJKLMNOP"),
         Json("EP0001-PPSA12345_00-ABCDEFGHIJKLMNO"),
         Json("EP0001-PPSA12345_00-ABCDEFGHIJKLMNOPQ"),
         Json("EP0001-PPSA12345_00-ABCDEFGHIJKLMNO/"),
         Json("EP0001-PPSA12345_00-ABCDEFGHIJKLMNO\n")}) {
    auto metadata = valid;
    metadata["contentId"] = invalid;
    metadata["conceptId"] = "123";
    write_json(metadata);
    const auto parsed = fixture.read();
    assert(parsed && parsed->name == "English Game" && parsed->content_id.empty() &&
           parsed->concept_id == "123" && parsed->image_url.empty());
  }
  for (const Json &invalid : {Json(nullptr), Json(123), Json(true), Json::array(),
                              Json::object(), Json(""), Json("1234567890123"),
                              Json("-1"), Json("1.0"), Json("12 3"), Json("123\n"),
                              Json("\xd9\xa1")}) {
    auto metadata = valid;
    metadata["contentId"] = ps5_content;
    metadata["conceptId"] = invalid;
    write_json(metadata);
    const auto parsed = fixture.read();
    assert(parsed && parsed->name == "English Game" && parsed->concept_id.empty() &&
           parsed->content_id == ps5_content && parsed->image_url.empty());
  }
  for (const Json &id : {Json("PPSA54321"), Json(nullptr), Json(12345)}) {
    auto invalid = valid;
    invalid["titleId"] = id;
    write_json(invalid);
    assert(!fixture.read());
  }
  auto absent_id = valid;
  absent_id.erase("titleId");
  write_json(absent_id);
  assert(!fixture.read());
  for (const Json &localized : {Json(nullptr), Json::array(), Json("invalid"), Json::object()}) {
    auto invalid = valid;
    invalid["localizedParameters"] = localized;
    write_json(invalid);
    assert(!fixture.read());
  }
  auto only_name = [&](const std::string &name) {
    return Json{{"titleId", "PPSA12345"},
                {"localizedParameters", {{"en-US", {{"titleName", name}}}}}};
  };
  for (const auto &name : {std::string(), std::string("   "), std::string("Bad\nGame"),
                           std::string("Bad\rGame"), std::string("Bad\tGame"),
                           std::string("Bad\0Game", 8), std::string("Bad\x7f"),
                           std::string("Bad\xc2\x85")}) {
    write_json(only_name(name));
    assert(!fixture.read());
  }
  const std::string unicode = "\xe6\x97\xa5\xe6\x9c\xac Game \xf0\x9f\x8e\xae";
  write_json(only_name(unicode));
  assert(fixture.read()->name == unicode);
  const std::string emoji = "\xf0\x9f\x8e\xae";
  std::string long_name;
  for (int i = 0; i < 130; ++i) long_name += emoji;
  write_json(only_name(long_name));
  assert(fixture.read()->name == long_name.substr(0, 128 * emoji.size()));
  write_json(only_name(std::string(128, 'x') + "\n"));
  assert(!fixture.read()); // Validate controls even beyond the truncation bound.
  for (const auto &data : {std::string("{"), std::string("null"), std::string("[]"),
                           std::string("{\"titleId\":\"PPSA12345\",\"x\":\"\xc0\xaf\"}"),
                           std::string(1024 * 1024 + 1, ' ')}) {
    fixture.write("PPSA12345", "param.json", data);
    assert(!fixture.read());
  }
  write_json(valid);
  assert(!fixture.read("../PPSA12345"));
  assert(!fixture.read("NPXS40140"));
  assert(!fixture.read("PPSA54321"));

  const auto valid_sfo = sfo("CUSA12345", "PS4 Game");
  fixture.write("CUSA12345", "param.sfo", valid_sfo);
  assert(fixture.read("CUSA12345")->name == "PS4 Game");
  assert(fixture.read("CUSA12345")->content_id.empty());
  fixture.write("CUSA12345", "param.json", "malformed");
  assert(fixture.read("CUSA12345")->name == "PS4 Game");
  const std::string ps4_content = "EP0001-CUSA12345_00-ABCDEFGHIJKLMNOP";
  fixture.write("CUSA12345", "param.sfo", sfo_content("CUSA12345", "PS4 Game", ps4_content));
  const auto ps4_title = fixture.read("CUSA12345");
  assert(ps4_title && ps4_title->name == "PS4 Game" && ps4_title->content_id == ps4_content &&
         ps4_title->concept_id.empty() && ps4_title->image_url.empty());
  for (const auto &invalid : {std::string(), std::string("EP0001-CUSA54321_00-ABCDEFGHIJKLMNOP"),
                              std::string("EP0001-PPSA12345_00-ABCDEFGHIJKLMNOP"),
                              std::string("ep0001-CUSA12345_00-ABCDEFGHIJKLMNOP"),
                              std::string("EP0001-CUSA12345_00-ABCDEFGHIJKLMNO/"),
                              std::string("EP0001-CUSA12345_00-ABCDEFGHIJKLMNO\n"),
                              std::string("EP0001-CUSA12345_00-ABCDEFGHIJKLMNO\0", 36)}) {
    fixture.write("CUSA12345", "param.sfo", sfo_content("CUSA12345", "PS4 Game", invalid));
    const auto parsed = fixture.read("CUSA12345");
    assert(parsed && parsed->name == "PS4 Game" && parsed->content_id.empty());
  }
  auto unsupported_content = sfo_content("CUSA12345", "PS4 Game", ps4_content);
  unsupported_content[55] = 4;
  fixture.write("CUSA12345", "param.sfo", unsupported_content);
  assert(fixture.read("CUSA12345")->name == "PS4 Game");
  assert(fixture.read("CUSA12345")->content_id.empty());
  for (const auto &invalid : {sfo("CUSA54321", "Wrong title"), sfo("CUSA12345", ""),
                              sfo("CUSA12345", "Bad\nGame"),
                              sfo("CUSA12345", std::string("Bad\0Game", 8)),
                              sfo("CUSA12345", "\xed\xa0\x80"),
                              sfo("CUSA12345", "\xf4\x90\x80\x80"),
                              sfo("CUSA12345", "\xc0\xaf"),
                              sfo("CUSA12345", "\xe2\x82")}) {
    fixture.write("CUSA12345", "param.sfo", invalid);
    assert(!fixture.read("CUSA12345"));
  }
  for (std::size_t length = 0; length < valid_sfo.size(); ++length) {
    fixture.write("CUSA12345", "param.sfo", valid_sfo.substr(0, length));
    assert(!fixture.read("CUSA12345"));
  }
  for (std::size_t offset : {8u, 12u, 16u, 24u, 28u, 32u, 40u, 44u, 48u}) {
    auto invalid = valid_sfo;
    put32(invalid, offset, 0xffffffffu);
    fixture.write("CUSA12345", "param.sfo", invalid);
    assert(!fixture.read("CUSA12345"));
  }
  auto bad_format = valid_sfo;
  bad_format[39] = 4;
  fixture.write("CUSA12345", "param.sfo", bad_format);
  assert(!fixture.read("CUSA12345"));
  fixture.write("CUSA12345", "param.sfo", std::string(1024 * 1024 + 1, '\0'));
  assert(!fixture.read("CUSA12345"));
  fs::remove(fixture.root / "PPSA12345" / "param.json");
  assert(mkfifo((fixture.root / "PPSA12345" / "param.json").c_str(), 0600) == 0);
  assert(!fixture.read()); // Non-regular metadata must not block the provider.
  std::cout << "Game activity metadata fixtures passed\n";
}
