// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>
namespace pbsd {
// XMB web-app tile, following the same registration pattern as the
// console's existing web-app tiles: a fake title directory under
// /user/app with sce_sys/param.json (deeplinkUri) and sce_sys/icon0.png,
// registered through AppInstUtil. The deeplink points at the console's
// own loopback dashboard listener.
inline constexpr const char *xmb_title_id = "PBSD09943";
inline constexpr const char *xmb_title_name = "PBS Discord";
std::string xmb_param_json(const std::string &title_id,
                           const std::string &url,
                           const std::string &name);
#ifdef PBS_PS5
// Stages and registers the tile if it is not already installed. Returns
// true when the tile is registered (or already present) on return. Never
// throws; failures are reported in the returned diagnostic string.
bool xmb_install_tile(const std::string &url, std::string &diagnostic);
#endif
} // namespace pbsd
