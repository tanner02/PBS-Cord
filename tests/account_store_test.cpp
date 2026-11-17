// SPDX-License-Identifier: GPL-3.0-or-later
// All credentials and files in this test are synthetic; no network is used.
#include "account_store.hpp"
#include "controller.hpp"
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace {
const std::string token_a = "FAKE-ACCOUNT-SESSION-A-FOR-OFFLINE-TESTS-ONLY";
const std::string token_b = "FAKE-ACCOUNT-SESSION-B-FOR-OFFLINE-TESTS-ONLY";
void check(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}
template <typename F> void rejects(F &&function) {
  bool rejected = false;
  try { function(); } catch (const std::exception &) { rejected = true; }
  check(rejected, "Operation unexpectedly accepted invalid storage");
}
struct Fixture {
  fs::path root, directory;
  Fixture() {
    auto pattern = (fs::temp_directory_path() / "pbsd-account-test-XXXXXX").string();
    std::vector<char> buffer(pattern.begin(), pattern.end()); buffer.push_back('\0');
    auto *created = mkdtemp(buffer.data());
    check(created != nullptr, "Could not create test directory");
    root = created;
    check(root.is_absolute() && root.parent_path() == fs::temp_directory_path() &&
          root.filename().string().starts_with("pbsd-account-test-"), "Invalid test cleanup directory");
    directory = root / "account";
  }
  ~Fixture() { std::error_code error; fs::remove_all(root, error); }
  fs::path key() const { return directory / "account-auto.key"; }
  fs::path vault() const { return directory / "account-auto.vault"; }
};
std::string read_file(const fs::path &path) {
  std::ifstream file(path, std::ios::binary);
  check(file.good(), "Could not read test fixture");
  return std::string(std::istreambuf_iterator<char>(file), {});
}
void write_file(const fs::path &path, const std::string &bytes, mode_t mode = 0600) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  check(file.good(), "Could not create test fixture");
  file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  file.close();
  check(!file.fail() && chmod(path.c_str(), mode) == 0, "Could not write/protect test fixture");
}
mode_t permissions(const fs::path &path) {
  struct stat info{};
  check(lstat(path.c_str(), &info) == 0, "Could not stat test fixture");
  return info.st_mode & 0777;
}
void basic_roundtrip() {
  Fixture fixture;
  pbsd::AccountStore first(fixture.directory.string());
  check(!first.saved() && !first.restore(), "Empty account store must not restore an account");
  first.save(token_a);
  check(first.saved(), "Saved account was not detected");
  pbsd::AccountStore reopened(fixture.directory.string());
  check(reopened.restore() == token_a, "Account did not restore across instances");
  const auto key = read_file(fixture.key()), vault = read_file(fixture.vault());
  check(key.size() == 64, "Device key has incorrect length");
  check(key.find(token_a) == std::string::npos && vault.find(token_a) == std::string::npos,
        "Synthetic session appeared as plaintext on disk");
  check(permissions(fixture.directory) == 0700, "New account directory must have mode 0700");
  check(permissions(fixture.key()) == 0600 && permissions(fixture.vault()) == 0600,
        "Device key and encrypted account must have mode 0600");
  reopened.save(token_b);
  const auto updated = read_file(fixture.vault());
  check(read_file(fixture.key()) == key, "Updating an account replaced its device key");
  check(updated != vault && updated.find(token_b) == std::string::npos, "Updated account was not encrypted");
  check(first.restore() == token_b, "Original instance could not restore the updated account");
  reopened.save(token_b);
  check(read_file(fixture.key()) == key && read_file(fixture.vault()) != updated,
        "Repeated save must retain the key and use fresh encryption material");
}
void existing_directory_permissions() {
  Fixture fixture;
  fs::create_directory(fixture.directory);
  check(chmod(fixture.directory.c_str(), 0755) == 0, "Could not prepare existing directory");
  pbsd::AccountStore store(fixture.directory.string());
  store.save(token_a);
  check(permissions(fixture.directory) == 0700, "Existing account directory must be restricted to 0700");
}
void damaged_key(const std::string &kind) {
  Fixture fixture;
  pbsd::AccountStore store(fixture.directory.string());
  store.save(token_a);
  const auto original_key = read_file(fixture.key()), original_vault = read_file(fixture.vault());
  if (kind == "missing") fs::remove(fixture.key());
  else if (kind == "wrong") {
    auto wrong = original_key; wrong[0] = wrong[0] == 'a' ? 'b' : 'a';
    write_file(fixture.key(), wrong);
  } else if (kind == "corrupt") write_file(fixture.key(), "not-a-device-key");
  else check(chmod(fixture.key().c_str(), 0644) == 0, "Could not change fixture permissions");
  const auto before_key = fs::exists(fixture.key()) ? read_file(fixture.key()) : std::string();
  const auto before_mode = fs::exists(fixture.key()) ? permissions(fixture.key()) : 0;
  rejects([&] { (void)store.restore(); });
  check(read_file(fixture.vault()) == original_vault, "Failed restore modified the encrypted account");
  rejects([&] { store.save(token_b); });
  check(read_file(fixture.vault()) == original_vault, "Invalid key save modified the old account");
  if (kind == "missing") check(!fs::exists(fixture.key()), "Missing key was silently replaced");
  else check(read_file(fixture.key()) == before_key && permissions(fixture.key()) == before_mode,
             "Rejected operation changed the device key or its permissions");
}
void nonregular_keys() {
  for (const auto kind : {"directory", "fifo", "symlink"}) {
    Fixture fixture;
    pbsd::AccountStore store(fixture.directory.string());
    store.save(token_a);
    const auto key = read_file(fixture.key()), vault = read_file(fixture.vault());
    fs::remove(fixture.key());
    const auto outside = fixture.root / "separate-fixture-key";
    if (std::string(kind) == "directory") fs::create_directory(fixture.key());
    else if (std::string(kind) == "fifo") check(mkfifo(fixture.key().c_str(), 0600) == 0, "Could not create fixture FIFO");
    else { write_file(outside, key); fs::create_symlink(outside, fixture.key()); }
    rejects([&] { (void)store.restore(); });
    rejects([&] { store.save(token_b); });
    check(read_file(fixture.vault()) == vault, "Rejected nonregular key changed the vault");
    if (std::string(kind) == "symlink") {
      check(fs::is_symlink(fixture.key()), "Rejected key symlink was replaced");
      check(read_file(outside) == key, "Rejected operation changed the symlink target");
    }
  }
}
void failed_save_preserves_account() {
  Fixture fixture;
  pbsd::AccountStore store(fixture.directory.string());
  store.save(token_a);
  const auto key = read_file(fixture.key()), vault = read_file(fixture.vault());
  // EISDIR forces a write failure even when the test runs as root.
  fs::create_directory(fixture.directory / "account-auto.vault.tmp");
  rejects([&] { store.save(token_b); });
  check(read_file(fixture.key()) == key && read_file(fixture.vault()) == vault,
        "Failed account save changed the previous key/vault");
  check(store.restore() == token_a, "Failed save damaged the previous account");
}
void forget_is_scoped() {
  Fixture fixture;
  pbsd::AccountStore store(fixture.directory.string());
  store.save(token_a);
  const auto legacy = fixture.directory / "account.vault";
  const auto activity = fixture.directory / "activity-settings.json";
  write_file(legacy, "synthetic-legacy-vault-do-not-load");
  write_file(activity, "{\"enabled\":true}\n");
  store.forget();
  store.forget();
  check(!fs::exists(fixture.key()) && !fs::exists(fixture.vault()) && !store.saved() && !store.restore(),
        "Forget did not remove the automatic account pair");
  check(read_file(legacy) == "synthetic-legacy-vault-do-not-load" && read_file(activity) == "{\"enabled\":true}\n",
        "Automatic-account forget modified unrelated legacy/settings files");
}
void empty_controller_status() {
  Fixture fixture;
  pbsd::Controller controller(fixture.directory.string());
  const auto state = controller.api("/api/status", pbsd::Json::object());
  check(state.at("state") == "disconnected" && state.at("account").is_null(),
        "Empty controller should remain disconnected without starting account work");
  check(state.at("auto_login_saved") == false && state.at("vault_saved") == false,
        "Empty controller reported a saved account");

}
void corrupted_store_controller_status() {
  Fixture fixture;
  pbsd::AccountStore store(fixture.directory.string());
  store.save(token_a);
  write_file(fixture.key(), "invalid-fixture-key");
  const auto key = read_file(fixture.key()), vault = read_file(fixture.vault());
  // Invalid local key stops restore before connect_account can start a worker.
  pbsd::Controller controller(fixture.directory.string());
  const auto state = controller.api("/api/status", pbsd::Json::object());
  check(state.at("state") == "disconnected" && state.at("account").is_null(),
        "Failed local restore must leave the controller disconnected");
  check(!state.at("account_save_error").get<std::string>().empty(), "Failed local restore needs a storage diagnostic");
  check(read_file(fixture.key()) == key && read_file(fixture.vault()) == vault,
        "Failed startup restore modified its saved account files");
}
} // namespace

int main() {
  alarm(45); // A regression opening a FIFO in blocking mode must not hang CI.
  unsigned failures = 0;
  for (const auto &[name, function] : std::vector<std::pair<const char *, std::function<void()>>>{
           {"roundtrip/encryption/permissions", basic_roundtrip},
           {"existing directory permissions", existing_directory_permissions},
           {"wrong key rejection", [] { damaged_key("wrong"); }},
           {"missing key rejection", [] { damaged_key("missing"); }},
           {"corrupt key rejection", [] { damaged_key("corrupt"); }},
           {"key permission rejection", [] { damaged_key("permissions"); }},
           {"nonregular/symlink keys", nonregular_keys},
           {"save failure atomicity", failed_save_preserves_account}, {"scoped forget", forget_is_scoped},
           {"empty controller status", empty_controller_status},
           {"corrupt store controller status", corrupted_store_controller_status}}) {
    try { function(); std::cout << "PASS " << name << '\n'; }
    catch (const std::exception &error) { ++failures; std::cerr << "FAIL " << name << ": " << error.what() << '\n'; }
  }
  return failures ? 1 : 0;
}
