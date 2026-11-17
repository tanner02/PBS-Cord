// SPDX-License-Identifier: GPL-3.0-or-later
#include "net.hpp"
#include <cassert>
#include <iostream>
#include <string>

int main() {
  using namespace pbsd;
  // Exercise the same payload parser used by receive(), without opening a socket.
  const auto normal = WebSocketClose::from_payload(Bytes{0x03, 0xe8});
  assert(normal.code() == 1000);
  assert(normal.reason().empty());
  assert(std::string(normal.what()) == "WebSocket session ended (code 1000)");

  Bytes payload{0x0f, 0xa4}; // 4004, in network byte order.
  const std::string peer_reason = "Example peer reason";
  payload.insert(payload.end(), peer_reason.begin(), peer_reason.end());
  try {
    throw WebSocketClose::from_payload(payload);
  } catch (const WebSocketClose &close) {
    assert(close.code() == 4004);
    assert(close.reason() == peer_reason);
    assert(std::string(close.what()) ==
           "WebSocket session ended (code 4004: Example peer reason)");
  }

  const auto absent = WebSocketClose::from_payload({});
  assert(!absent.code());
  assert(absent.reason().empty());
  assert(std::string(absent.what()) == "WebSocket session ended (no close code)");

  const auto printable = WebSocketClose::from_payload(
      Bytes{0x03, 0xe8, 'a', '\n', '\r', 0, 0x1b, 0x7f, 0xff, 'z'});
  assert(printable.reason() == "a??????z");
  Bytes longest(125, 'x');
  longest[0] = 0x03;
  longest[1] = 0xe8;
  assert(WebSocketClose::from_payload(longest).reason().size() == 123);

  for (const auto &invalid : {Bytes{0x03}, Bytes(126, 'x')}) {
    bool rejected = false;
    try {
      (void)WebSocketClose::from_payload(invalid);
    } catch (const WebSocketClose &) {
      assert(false && "Malformed payload is a protocol error, not a close event");
    } catch (const std::runtime_error &error) {
      rejected = true;
      assert(std::string(error.what()) == "Invalid WebSocket close payload");
    }
    assert(rejected);
  }
  std::cout << "WebSocket close payload diagnostics passed\n";
}
