// SPDX-License-Identifier: GPL-3.0-or-later
#include "xmb.hpp"
#include <cstdio>
namespace pbsd {
namespace {
// param.json is small and machine-built; escaping the three string inputs
// keeps the JSON valid even if a future caller passes user-influenced text.
void json_string(std::string &out, const std::string &value) {
  for (char c : value) {
    switch (c) {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\n':
    case '\r':
    case '\t':
      out += ' ';
      break;
    default:
      if (static_cast<unsigned char>(c) < 0x20)
        out += ' ';
      else
        out += c;
    }
  }
}
} // namespace
std::string xmb_param_json(const std::string &title_id, const std::string &url,
                           const std::string &name) {
  std::string out;
  out.reserve(256 + url.size() + title_id.size() + name.size());
  out += "{\n  \"applicationCategoryType\": 65536,\n  \"titleId\": \"";
  json_string(out, title_id);
  out += "\",\n  \"deeplinkUri\": \"";
  json_string(out, url);
  out += "\",\n  \"localizedParameters\": {\n    \"defaultLanguage\": "
          "\"en-US\",\n    \"en-US\": {\n      \"titleName\": \"";
  json_string(out, name);
  out += "\"\n    }\n  }\n}\n";
  return out;
}
#ifdef PBS_PS5
#include "assets.hpp"
#include <ps5/kernel.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
// Deliberately NO direct imports of libSceAppInstUtil functions here: its
// exports use decorated NIDs that do not derive from the function names,
// so a name-based import cannot be resolved by the payload rtld and aborts
// the whole ELF before main (observed on hardware). Everything below loads
// and resolves the module at runtime instead, and every failure is only a
// missing tile - never a dead payload.
extern "C" int sceKernelLoadStartModule(const char *, std::size_t,
                                        const void *, std::uint32_t, void *,
                                        std::uint32_t *);
namespace {
int write_file(const std::string &path, const void *data, std::size_t size) {
  int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0755);
  if (fd < 0)
    return -1;
  const auto *bytes = static_cast<const unsigned char *>(data);
  std::size_t at = 0;
  while (at < size) {
    auto written = write(fd, bytes + at, size - at);
    if (written < 0 && errno != EINTR) {
      int saved = errno;
      close(fd);
      errno = saved;
      return -1;
    }
    if (written > 0)
      at += static_cast<std::size_t>(written);
  }
  return close(fd);
}
void hex_code(std::string &out, std::uint32_t value) {
  char code[16];
  std::snprintf(code, sizeof code, "0x%08x", value);
  out += code;
}
// libSceAppInstUtil.sprx cannot be imported statically (decorated NIDs
// abort name-based resolution), so load it into this process first. The
// dlopen route is the ShadowMountPlus pre-12 pattern; the raw
// LoadStartModule call is the same one the payload rtld itself uses for
// DT_NEEDED entries.
bool load_app_inst_util(std::string &diagnostic) {
  uint32_t handle = 0;
  if (kernel_dynlib_handle(-1, "libSceAppInstUtil.sprx", &handle) == 0)
    return true;
  void *dl = dlopen("/system/common/lib/libSceAppInstUtil.sprx", RTLD_LAZY);
  if (dl)
    return true;
  long rc = sceKernelLoadStartModule(
      "/system/common/lib/libSceAppInstUtil.sprx", 0, nullptr, 0, nullptr,
      nullptr);
  if (rc < 0) {
    diagnostic = "load libSceAppInstUtil ";
    hex_code(diagnostic, static_cast<std::uint32_t>(rc));
    return false;
  }
  return kernel_dynlib_handle(-1, "libSceAppInstUtil.sprx", &handle) == 0;
}
using install_fn = int (*)(const char *, const char *, void *);
install_fn resolve_install_title_dir() {
  // Hardware-decorated NID for sceAppInstUtilAppInstallTitleDir, as used
  // by the console's working web-app tiles (patch manager, ftpsrv).
  static const char nid[] = "Wudg3Xe3heE";
  uint32_t handle = 0;
  if (kernel_dynlib_handle(-1, "libSceAppInstUtil.sprx", &handle) != 0)
    return nullptr;
  return reinterpret_cast<install_fn>(kernel_dynlib_resolve(-1, handle, nid));
}
using init_fn = int (*)(void);
init_fn resolve_initialize() {
  // Best effort: name lookup only succeeds where exports are not
  // decorated. A miss here is not fatal.
  void *dl = dlopen(nullptr, RTLD_LAZY);
  if (!dl)
    return nullptr;
  return reinterpret_cast<init_fn>(dlsym(dl, "sceAppInstUtilInitialize"));
}
} // namespace
bool xmb_install_tile(const std::string &url, std::string &diagnostic) {
  // Registration works on firmware 12.00, but the libSceAppInstUtil load
  // can take minutes in uninterruptible kernel time before it completes
  // (observed on hardware: the process ignored SIGKILL while the call was
  // in flight, then finished normally). The caller runs this detached
  // from startup, so a slow or stuck attempt never affects the dashboard,
  // the capture or shutdown.
  const std::string id = xmb_title_id;
  const std::string app_dir = "/user/app/" + id;
  const std::string sys_dir = app_dir + "/sce_sys";
  const std::string appmeta = "/user/appmeta/" + id + "/param.json";
  struct stat st;
  if (stat(appmeta.c_str(), &st) == 0) {
    diagnostic = "already registered";
    return true;
  }
  if (mkdir("/user/app", 0755) < 0 && errno != EEXIST) {
    diagnostic = std::string("mkdir /user/app: ") + std::strerror(errno);
    return false;
  }
  if (mkdir(app_dir.c_str(), 0755) < 0 && errno != EEXIST) {
    diagnostic = std::string("mkdir ") + app_dir + ": " + std::strerror(errno);
    return false;
  }
  if (mkdir(sys_dir.c_str(), 0755) < 0 && errno != EEXIST) {
    diagnostic = std::string("mkdir ") + sys_dir + ": " + std::strerror(errno);
    return false;
  }
  auto json = xmb_param_json(id, url, xmb_title_name);
  if (write_file(sys_dir + "/param.json", json.data(), json.size())) {
    diagnostic = std::string("param.json: ") + std::strerror(errno);
    return false;
  }
  if (write_file(sys_dir + "/icon0.png", asset_icon0_png,
                 static_cast<std::size_t>(asset_icon0_png_len))) {
    diagnostic = std::string("icon0.png: ") + std::strerror(errno);
    return false;
  }
  if (!load_app_inst_util(diagnostic))
    return false;
  auto initialize = resolve_initialize();
  if (initialize) {
    int rc = initialize();
    if (rc != 0) {
      diagnostic = "AppInstUtilInitialize ";
      hex_code(diagnostic, static_cast<std::uint32_t>(rc));
      return false;
    }
  }
  auto install_title_dir = resolve_install_title_dir();
  if (!install_title_dir) {
    diagnostic = "InstallTitleDir NID unresolved";
    return false;
  }
  int result = install_title_dir(id.c_str(), "/user/app/", nullptr);
  if (result != 0 && static_cast<std::uint32_t>(result) != 0x80990002u) {
    diagnostic = "register ";
    hex_code(diagnostic, static_cast<std::uint32_t>(result));
    return false;
  }
  diagnostic = "registered";
  return true;
}
#endif
} // namespace pbsd
