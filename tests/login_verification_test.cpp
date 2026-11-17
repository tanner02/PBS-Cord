// SPDX-License-Identifier: GPL-3.0-or-later
#include "login_verification.hpp"
#include <atomic>
#include <cassert>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace pbsd;

template <typename F> void rejects(F action) {
  bool rejected = false;
  try {
    action();
  } catch (const std::runtime_error &error) {
    rejected = true;
    // Diagnostics must never echo untrusted challenge or solution contents.
    assert(std::string(error.what()).find("private") == std::string::npos);
  }
  assert(rejected);
}

int main() {
  const Json fixture{{"captcha_service", "hcaptcha"},
                     {"captcha_sitekey", "dynamic-sitekey_123"},
                     {"captcha_rqdata", "public challenge data"},
                     {"captcha_rqtoken", "private request token"},
                     {"captcha_session_id", "private session"}};
  LoginVerification state;
  assert(state.status(0).is_null());
  assert(!state.take(0));
  state.issue(fixture, 100);
  const auto visible = state.status(1);
  const std::string id = visible.at("id");
  assert(id.size() == 32);
  assert(visible == Json({{"id", id}, {"service", "hcaptcha"},
                          {"sitekey", "dynamic-sitekey_123"},
                          {"rqdata", "public challenge data"}}));
  assert(visible.dump().find("private") == std::string::npos);
  assert(!state.take(1));
  rejects([&] { state.submit("stale", "private solution", 1); });
  for (const auto &solution : {std::string(), std::string(8193, 'x'),
                               std::string("private\r\ninjection"),
                               std::string("private\0injection", 17),
                               std::string("private\x7f")})
    rejects([&] { state.submit(id, solution, 1); });
  state.submit(id, "private solution", 1);
  assert(state.status(1).is_null());
  rejects([&] { state.submit(id, "duplicate", 1); });
  const auto headers = state.take(2);
  assert(headers && headers->size() == 3);
  assert(headers->at("X-Captcha-Key") == "private solution");
  assert(headers->at("X-Captcha-Rqtoken") == "private request token");
  assert(headers->at("X-Captcha-Session-Id") == "private session");
  assert(!state.take(2));
  assert(state.status(2).is_null());
  rejects([&] { state.submit(id, "replay", 2); });

  state.issue(fixture, 100);
  const std::string expired_id = state.status(99).at("id");
  assert(state.status(100).is_null());
  rejects([&] { state.submit(expired_id, "private solution", 100); });
  assert(!state.take(100));
  state.issue(fixture, 100);
  state.submit(state.status(1).at("id"), "private solution", 99);
  assert(!state.take(100));
  assert(!state.take(1)); // Clock arguments cannot resurrect erased state.

  state.issue(fixture, 100);
  const std::string cancelled = state.status(1).at("id");
  state.clear();
  rejects([&] { state.submit(cancelled, "private solution", 1); });
  assert(!state.take(1));
  state.issue(fixture, 100);
  state.submit(state.status(1).at("id"), "private solution", 1);
  state.clear();
  assert(!state.take(1));

  state.issue(fixture, 100);
  const std::string replaced = state.status(1).at("id");
  state.issue(fixture, 100);
  assert(state.status(1).at("id") != replaced);
  rejects([&] { state.submit(replaced, "private solution", 1); });

  for (const auto *name : {"captcha_service", "captcha_sitekey",
                          "captcha_rqdata", "captcha_rqtoken",
                          "captcha_session_id"}) {
    for (const Json &value : {Json(42), Json(true), Json::array(), Json::object()}) {
      auto invalid = fixture;
      invalid[name] = value;
      rejects([&] { state.issue(invalid, 100); });
      assert(state.status(1).is_null());
    }
  }
  for (const auto &[name, length] :
       {std::pair{"captcha_sitekey", 129}, {"captcha_rqdata", 8193},
        {"captcha_rqtoken", 4097}, {"captcha_session_id", 513}}) {
    auto invalid = fixture;
    invalid[name] = std::string(length, 'x');
    rejects([&] { state.issue(invalid, 100); });
  }
  for (const auto *name : {"captcha_rqtoken", "captcha_session_id"}) {
    for (char control : {'\r', '\n', '\0', '\t', '\x1f', '\x7f'}) {
      auto invalid = fixture;
      invalid[name] = std::string("private") + control + "injection";
      rejects([&] { state.issue(invalid, 100); });
    }
  }
  for (const auto &value : {"", "private.sitekey", "private sitekey", "private\n"}) {
    auto invalid = fixture;
    invalid["captcha_sitekey"] = value;
    rejects([&] { state.issue(invalid, 100); });
  }
  for (const Json &value : {Json("recaptcha"), Json(""), Json(nullptr)}) {
    auto invalid = fixture;
    invalid["captcha_service"] = value;
    rejects([&] { state.issue(invalid, 100); });
  }
  rejects([&] { state.issue(Json::array(), 100); });
  rejects([&] { state.issue(Json::object(), 100); });

  Json minimal{{"captcha_service", "hcaptcha"}, {"captcha_sitekey", "key"},
               {"captcha_rqdata", nullptr}, {"captcha_rqtoken", nullptr},
               {"captcha_session_id", nullptr}};
  state.issue(minimal, 100);
  assert(state.status(1).at("rqdata") == "");
  state.submit(state.status(1).at("id"), std::string(8192, 'x'), 1);
  assert(state.take(1)->size() == 1);
  minimal.erase("captcha_rqdata");
  minimal.erase("captcha_rqtoken");
  minimal.erase("captcha_session_id");
  minimal["captcha_sitekey"] = std::string(128, 'x');
  state.issue(minimal, 100);
  state.submit(state.status(1).at("id"), "solution", 1);
  assert(state.take(1)->size() == 1);

  state.issue(fixture, 100);
  const std::string concurrent_id = state.status(1).at("id");
  std::atomic<int> accepted{};
  std::vector<std::thread> submitters;
  for (int i = 0; i < 8; ++i)
    submitters.emplace_back([&] {
      try {
        state.submit(concurrent_id, "private solution", 1);
        ++accepted;
      } catch (const std::runtime_error &) {
      }
    });
  for (auto &thread : submitters)
    thread.join();
  assert(accepted == 1);
  std::atomic<int> consumed{};
  std::vector<std::thread> consumers;
  for (int i = 0; i < 8; ++i)
    consumers.emplace_back([&] {
      if (state.take(1))
        ++consumed;
    });
  for (auto &thread : consumers)
    thread.join();
  assert(consumed == 1);
  std::cout << "User login verification state passed\n";
}
