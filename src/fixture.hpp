// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <atomic>
#include <string>
namespace pbsd {
void run_fixture(const std::string &path, int width, int height, int fps,
                 const std::atomic<bool> &quit);
}
