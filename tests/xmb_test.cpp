// SPDX-License-Identifier: GPL-3.0-or-later
#include "xmb.hpp"
#include <cassert>
#include <iostream>
using namespace pbsd;
int main() {
  auto json = xmb_param_json(xmb_title_id, "http://127.0.0.1:9942/",
                             xmb_title_name);
  assert(json.find("\"titleId\": \"PBSD09943\"") != std::string::npos);
  assert(json.find("\"deeplinkUri\": \"http://127.0.0.1:9942/\"") !=
         std::string::npos);
  assert(json.find("\"titleName\": \"PBS Discord\"") != std::string::npos);
  assert(json.find("\"applicationCategoryType\": 65536") != std::string::npos);
  assert(json.find("\"defaultLanguage\": \"en-US\"") != std::string::npos);
  // Quotes, backslashes and control characters must never break out of the
  // JSON string grammar: the tile content is fully machine-controlled, but
  // the builder is checked here so future callers stay safe too.
  auto hostile = xmb_param_json("X\"Y\\Z\nW", "http://a/b?q=\"\\\"\r\n",
                                "na\"me\\");
  assert(hostile.find("\\\"") != std::string::npos);
  assert(hostile.find("\\\\") != std::string::npos);
  assert(hostile.find("\nW\"") == std::string::npos);
  assert(hostile.find("\r\n\"") == std::string::npos);
  assert(hostile.find("na\"me\\\"") == std::string::npos);
  // The trailing structure stays intact even with hostile inputs.
  assert(hostile.starts_with("{\n"));
  assert(hostile.ends_with("}\n"));
  std::cout << "xmb tests passed\n";
}
