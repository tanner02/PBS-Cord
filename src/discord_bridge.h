// SPDX-License-Identifier: GPL-3.0-or-later
// C bridge between the PBS HTTP server and the Discord engine. The PBS
// viewer's Livestream tab calls /discord/* routes; server.c forwards them
// here. The engine initializes lazily in its own thread, so PBS startup
// can never stall on it.
#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
// 0 = engine starting, 1 = ready, -1 = failed (see engine_error below).
// Shutdown is terminal and reports -1 with a shutdown diagnostic.
int pbsd_discord_engine_state(void);
const char *pbsd_discord_engine_error(void);
// Handles one /discord/* route. method is "GET" or "POST", route is the
// path after "/discord/" (e.g. "status", "start"), body is the raw JSON
// request body or NULL. Returns a malloc'd JSON string; release with
// pbsd_discord_free. Never returns NULL.
char *pbsd_discord_api(const char *method, const char *route,
                       const char *body);
void pbsd_discord_free(char *ptr);
// Call after the HTTP workers have joined, before unloading this ELF. Waits
// for initialization and Controller workers; idempotent, including pre-start.
// Later state/API calls report shutdown and never start a replacement engine.
void pbsd_discord_shutdown(void);
#ifdef __cplusplus
}
#endif
