// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "types.hpp"
namespace pbsd {
// Rewrite Annex B SPS VUI restrictions before DAVE authentication. Returns
// false for valid, already normalized SPS (or an AU without SPS). Throws
// std::invalid_argument on malformed/bounded-out input; leaves the AU intact.
bool rewrite_sps(Bytes &access_unit);
} // namespace pbsd
