// SPDX-License-Identifier: GPL-3.0-or-later
#include "game_artwork.hpp"
#include <cassert>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <future>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <sys/stat.h>
#include <vector>
#include <unistd.h>

using namespace pbsd;
using namespace std::chrono_literals;
namespace {
const std::string cover = "https://image.api.playstation.com/vulcan/ap/rnd/202601/1234/ActualGameCover.png?w=440&thumb=false";
const std::string other_cover = "https://image.dl.playstation.net/cdn/UP0106/PPSA12345_00/game-cover.jpg";
GameTitle game(std::string concept_id = "10001234") {
  return {"PPSA12345", "Fixture Game", "UP0106-PPSA12345_00-RESLERI100MASTER", std::move(concept_id), ""};
}
std::string product(const std::string &image, const std::string &sku = "") {
  return "<script type=\"application/ld+json\">{\"@context\":\"https://schema.org\","
      "\"@type\":\"Product\",\"name\":\"Fixture Game\",\"sku\":\"" + sku +
      "\",\"image\":\"" + image + "\"}</script>";
}
std::string concept_page(const GameTitle &title, const std::string &image) {
  return "<!doctype html><html><head>" + product(image) +
      "<script id=\"__NEXT_DATA__\" type=\"application/json\">{\"props\":{\"pageProps\":{"
      "\"conceptId\":\"" + title.concept_id + "\"}}}</script></head></html>";
}

// Every fetch is explicitly released, so no real network or timing assumption
// determines which title is active, queued, replaced, or cached.
struct FetchGate {
  std::mutex mutex;
  std::condition_variable changed;
  std::vector<std::string> calls;
  std::map<std::string, std::string> pages;
  unsigned released{};
  std::string fetch(const std::string &url) {
    std::unique_lock lock(mutex);
    calls.push_back(url);
    const auto ordinal = calls.size();
    changed.notify_all();
    assert(changed.wait_for(lock, 3s, [&] { return released >= ordinal; }));
    return pages.at(url);
  }
  void await(unsigned count) {
    std::unique_lock lock(mutex);
    assert(changed.wait_for(lock, 2s, [&] { return calls.size() >= count; }));
    assert(calls.size() == count);
  }
  void release(unsigned count) {
    std::lock_guard lock(mutex);
    released = count;
    changed.notify_all();
  }
  void no_more_than(unsigned count) {
    std::unique_lock lock(mutex);
    assert(!changed.wait_for(lock, 100ms, [&] { return calls.size() > count; }));
    assert(calls.size() == count);
  }
};
void await_image(GameArtwork &artwork, const GameTitle &title, const std::string &expected) {
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (std::chrono::steady_clock::now() < deadline) {
    const auto result = artwork.image(title);
    assert(result.empty() || result == expected);
    if (result == expected) return;
    std::this_thread::sleep_for(2ms);
  }
  assert(!"artwork lookup did not become cached");
}
} // namespace

int main() {
  alarm(15);
  auto title = game();
  assert(title.content_id.size() == 36);
  assert(public_game_image_url(cover));
  assert(public_game_image_url(other_cover));
  for (const auto &url : {"http://image.api.playstation.com/a.png", "https://example.com/a.png",
                         "https://image.api.playstation.com.evil.test/a.png",
                         "https://image.api.playstation.com@evil.test/a.png",
                         "https://image.api.playstation.com/a b.png", "data:image/png;base64,AA=="})
    assert(!public_game_image_url(url));
  assert(!public_game_image_url("https://image.api.playstation.com/" + std::string(300, 'a')));

  assert(game_store_url(title) == "https://store.playstation.com/en-us/concept/10001234");
  auto regional = game("");
  for (const auto &[region, locale] : std::map<std::string, std::string>{
           {"UP", "en-us"}, {"EP", "en-gb"}, {"HP", "en-hk"}, {"JP", "ja-jp"}}) {
    regional.content_id.replace(0, 2, region);
    assert(game_store_url(regional) == "https://store.playstation.com/" + locale + "/product/" + regional.content_id);
  }
  regional.content_id.replace(0, 2, "XX");
  assert(game_store_url(regional).empty());
  for (const auto &content : {"", "UP0106-PPSA99999_00-RESLERI100MASTER",
                              "UP0106-PPSA12345_00-short", "../PPSA12345"}) {
    auto invalid = game(""); invalid.content_id = content;
    assert(game_store_url(invalid).empty());
  }
  auto invalid = game("not-a-concept"); invalid.content_id.clear();
  assert(game_store_url(invalid).empty());

  assert(parse_game_store_image(concept_page(title, cover), title) == cover);
  auto wrong_concept = title; wrong_concept.concept_id = "99999999";
  assert(parse_game_store_image(concept_page(wrong_concept, cover), title).empty());
  auto sku_title = game("");
  assert(parse_game_store_image(product(other_cover, sku_title.content_id), sku_title) == other_cover);
  assert(parse_game_store_image(product(cover, "UP0106-PPSA99999_00-RESLERI100MASTER"), sku_title).empty());
  assert(parse_game_store_image(concept_page(title, "https://evil.test/game.png"), title).empty());
  assert(parse_game_store_image("<script type='application/ld+json'>{broken}</script>", title).empty());
  assert(parse_game_store_image("<script id='__NEXT_DATA__'>", title).empty());
  assert(parse_game_store_image(std::string(2 * 1024 * 1024 + 1, ' ') + concept_page(title, cover), title).empty());

  // Delisted and disc PS4 titles can have neither a concept nor a usable Store SKU.
  const GameTitle ps4{"CUSA01433", "Rocket League"};
  const std::string tmdb_url = "https://tmdb.np.dl.playstation.net/tmdb2/"
      "CUSA01433_00_74520BBFF3DF8FB578A6772271B6D746F2BF5E9E/CUSA01433_00.json";
  const std::string tmdb_icon = "https://gs2-sec.ww.prod.dl.playstation.net/gs2-sec/appkgo/prod/"
      "CUSA01433_00/7/i_5c5e430a49994f22df5fd81f446ead7b6ae45027af490b415fe4e744a9918e4c/i/icon0.png";
  {
    FetchGate tmdb;
    tmdb.pages[tmdb_url] = nlohmann::json{
        {"npTitleId", "CUSA01433_00"},
        {"icons", {{{"icon", "http" + tmdb_icon.substr(5)}, {"type", "512x512"}}}}}.dump();
    GameArtwork ps4_artwork([&](const auto &url) { return tmdb.fetch(url); });
    assert(public_game_image_url(tmdb_icon));
    assert(ps4_artwork.image(ps4).empty());
    tmdb.await(1);
    tmdb.release(1);
    await_image(ps4_artwork, ps4, tmdb_icon);
    tmdb.no_more_than(1);
  }

  auto a = game("10000001"), b = game("10000002"), c = game("10000003"), d = game("10000004");
  FetchGate gate;
  gate.pages = {{game_store_url(a), concept_page(a, cover)}, {game_store_url(b), "not a store page"},
                {game_store_url(c), concept_page(c, other_cover)}, {game_store_url(d), concept_page(d, cover)}};
  GameArtwork artwork([&](const auto &url) { return gate.fetch(url); });
  assert(artwork.image(a).empty());
  gate.await(1);
  auto immediate = std::async(std::launch::async, [&] { return artwork.image(a); });
  assert(immediate.wait_for(200ms) == std::future_status::ready);
  assert(immediate.get().empty());
  for (int i = 0; i < 20; ++i) assert(artwork.image(a).empty());
  gate.no_more_than(1);
  assert(artwork.image(b).empty());
  assert(artwork.image(c).empty()); // C replaces B while A remains blocked.
  gate.release(1);
  gate.await(2);
  {
    std::lock_guard lock(gate.mutex);
    assert(gate.calls[0] == game_store_url(a) && gate.calls[1] == game_store_url(c));
  }
  // Completed A must not be presented as C's artwork while C is still pending.
  assert(artwork.image(c).empty());
  assert(artwork.image(a) == cover);
  gate.release(2);
  await_image(artwork, c, other_cover);
  for (int i = 0; i < 20; ++i) assert(artwork.image(c) == other_cover);
  gate.no_more_than(2);

  assert(artwork.image(b).empty());
  gate.await(3);
  for (int i = 0; i < 20; ++i) assert(artwork.image(b).empty());
  gate.release(3);
  assert(artwork.image(d).empty());
  gate.await(4); // Reaching D proves B's miss has been cached.
  for (int i = 0; i < 20; ++i) assert(artwork.image(b).empty());
  gate.release(4);
  await_image(artwork, d, cover);
  gate.no_more_than(4);
  assert(artwork.image(GameTitle{}).empty());
  gate.no_more_than(4);

  // A local manifest is an optional source of already resolved public art.
  // All misses below still use fixture callbacks, never the network fetcher.
  char manifest_path[] = "/tmp/pbsd-artwork-manifest-XXXXXX";
  int manifest_fd = mkstemp(manifest_path);
  assert(manifest_fd >= 0);
  close(manifest_fd);
  auto write_manifest = [&](const std::string &data) {
    int fd = open(manifest_path, O_WRONLY | O_TRUNC);
    assert(fd >= 0);
    std::size_t at = 0;
    while (at < data.size()) {
      const auto n = write(fd, data.data() + at, data.size() - at);
      if (n < 0 && errno == EINTR) continue;
      assert(n > 0);
      at += static_cast<std::size_t>(n);
    }
    close(fd);
  };
  using Json = nlohmann::json;
  Json images = {{game_store_url(title), cover}, {ps4.title_id, tmdb_icon}};
  std::vector<GameTitle> regional_titles;
  for (const auto &region : {"UP", "EP", "HP", "JP"}) {
    auto product_title = game("");
    product_title.content_id.replace(0, 2, region);
    regional_titles.push_back(product_title);
    images[game_store_url(product_title)] = other_cover;
  }
  const Json manifest{{"version", 1}, {"images", images}};
  write_manifest(manifest.dump());
  std::atomic<unsigned> manifest_fetches{};
  auto fixture_fetch = [&](const std::string &) {
    ++manifest_fetches;
    return std::string();
  };
  {
    GameArtwork cached(fixture_fetch, manifest_path);
    assert(manifest_fetches == 0);
    for (int i = 0; i < 20; ++i) assert(cached.image(title) == cover);
    assert(cached.image(ps4) == tmdb_icon);
    for (const auto &product_title : regional_titles)
      assert(cached.image(product_title) == other_cover);
    assert(manifest_fetches == 0);
  }
  auto rejected_manifest = [&](const std::string &data) {
    write_manifest(data);
    const auto before = manifest_fetches.load();
    {
      GameArtwork rejected(fixture_fetch, manifest_path);
      assert(manifest_fetches == before); // Construction never starts a fetch.
      assert(rejected.image(title).empty());
    }
    // Destruction may cancel a miss before its asynchronous fetch begins.
    assert(manifest_fetches <= before + 1);
  };
  for (const auto &data : {
         std::string("{"), std::string("null"), std::string("[]"),
         Json({{"version", 2}, {"images", images}}).dump(),
         Json({{"version", "1"}, {"images", images}}).dump(),
         Json({{"version", 1}, {"images", Json::array()}}).dump(),
         std::string(256 * 1024 + 1, ' ')})
    rejected_manifest(data);
  // Enforce the aggregate bound even when every URL is independently valid.
  Json too_many = images;
  for (int i = 0; i < 33; ++i)
    too_many["https://store.playstation.com/en-us/concept/" + std::to_string(20000000 + i)] = cover;
  rejected_manifest(Json({{"version", 1}, {"images", too_many}}).dump());
  for (const Json &bad_image : {
         Json("https://evil.test/art.png"), Json("http://image.api.playstation.com/a.png"),
         Json("https://image.api.playstation.com.evil.test/a.png"),
         Json("https://image.api.playstation.com@evil.test/a.png"),
         Json("https://image.api.playstation.com/a.png\n"),
         Json("data:image/png;base64,AAAA"), Json(nullptr), Json(42), Json::array()})
    rejected_manifest(Json({{"version", 1},
                            {"images", {{game_store_url(title), bad_image}}}}).dump());
  for (const auto &bad_key : {
         "http://store.playstation.com/en-us/concept/10001234",
         "https://store.playstation.com.evil.test/en-us/concept/10001234",
         "https://store.playstation.com@evil.test/en-us/concept/10001234",
         "https://store.playstation.com/de-de/concept/10001234",
         "https://store.playstation.com/en-us/concept/10001234?other=1",
         "https://store.playstation.com/en-us/concept/10001234/",
         "https://store.playstation.com/en-us/concept/1234567890123",
         "https://store.playstation.com/en-us/concept/../10001234",
         "https://store.playstation.com/en-us/product/UP0106-NPXS12345_00-RESLERI100MASTER",
         "https://store.playstation.com/en-us/product/UP0106-PPSA12345_00-short"})
    rejected_manifest(Json({{"version", 1}, {"images", {{bad_key, cover}}}}).dump());
  // Exactly the byte limit remains usable; no unbounded stream read occurs.
  std::string padded_manifest = manifest.dump();
  padded_manifest.resize(256 * 1024, ' ');
  write_manifest(padded_manifest);
  {
    GameArtwork cached(fixture_fetch, manifest_path);
    assert(cached.image(title) == cover);
  }
  Json full_images = Json::object();
  std::vector<GameTitle> seeded_titles;
  for (int i = 0; i < 32; ++i) {
    seeded_titles.push_back(game(std::to_string(31000000 + i)));
    full_images[game_store_url(seeded_titles.back())] = cover;
  }
  write_manifest(Json({{"version", 1}, {"images", full_images}}).dump());
  {
    FetchGate extra_gate;
    const auto extra_title = game("41000000");
    extra_gate.pages[game_store_url(extra_title)] = concept_page(extra_title, other_cover);
    GameArtwork cached([&](const auto &url) { return extra_gate.fetch(url); }, manifest_path);
    for (const auto &seeded : seeded_titles) assert(cached.image(seeded) == cover);
    assert(cached.image(extra_title).empty());
    extra_gate.await(1);
    extra_gate.release(1);
    await_image(cached, extra_title, other_cover); // Proves the async write completed.
    for (const auto &seeded : seeded_titles) assert(cached.image(seeded) == cover);
    extra_gate.no_more_than(1);
  }
  unlink(manifest_path);
  assert(mkfifo(manifest_path, 0600) == 0);
  {
    GameArtwork nonregular(fixture_fetch, manifest_path);
    assert(nonregular.image(title).empty());
  }
  unlink(manifest_path);
  {
    GameArtwork missing(fixture_fetch, manifest_path);
    assert(missing.image(title).empty());
  }
  alarm(0);
}
