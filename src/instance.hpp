// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <thread>
namespace pbsd {
// Ownership covers capture cleanup; the local control file survives network loss.
class PayloadInstance {
public:
  PayloadInstance(std::string directory, std::function<void()> request_stop);
  ~PayloadInstance();
  PayloadInstance(const PayloadInstance &) = delete;
  PayloadInstance &operator=(const PayloadInstance &) = delete;
  bool acquire(std::string &diagnostic,
               std::chrono::milliseconds timeout = std::chrono::seconds(45));
  bool restart_requested() const { return restart_.load(); }
private:
  std::string directory_, control_path_;
  std::function<void()> request_stop_;
  int lock_ = -1, control_fd_ = -1;
  bool owner_ = false;
  std::atomic<bool> closing_{false}, restart_{false};
  std::thread control_;
  void serve();
};
} // namespace pbsd
