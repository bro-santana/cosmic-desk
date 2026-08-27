/*
 * This file is part of Moonlight Embedded.
 *
 * Copyright (C) 2015-2017 Iwan Timmer
 *
 * Moonlight is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * Moonlight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Moonlight; if not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "xml.h"

#include <Limelight.h>

#include <stdbool.h>

#define MIN_SUPPORTED_GFE_VERSION 3
#define MAX_SUPPORTED_GFE_VERSION 7

typedef struct _SERVER_DATA {
  char* gpuType;
  bool paired;
  // COSMIC MODIFICATION: true when the host's HTTPS listener answered during
  // the /serverinfo probe (even with HTTP 401 because this client is not yet
  // paired), i.e. the HTTPS port is reachable. Set by load_server_status(); a
  // transport failure (connect refused / timeout) leaves it false.
  bool httpsReachable;
  bool unsupported;
  bool isNvidiaSoftware;
  int currentGame;
  int serverMajorVersion;
  char* gsVersion;
  PDISPLAY_MODE modes;
  // COSMIC MODIFICATION: parsed <CosmicDisplays> list (docs/PROTOCOL.md),
  // populated by gs_init/gs_load_serverinfo. NULL when the host is stock
  // Sunshine (no CosmicDisplays block). Owned by the caller; free each node's
  // name and the nodes themselves.
  PCOSMIC_DISPLAY displays;
  SERVER_INFORMATION serverInfo;
  unsigned short httpPort;
  unsigned short httpsPort;
} SERVER_DATA, *PSERVER_DATA;

int gs_init(PSERVER_DATA server, char* address, unsigned short httpPort, const char *keyDirectory, int logLevel, bool unsupported);
// COSMIC MODIFICATION: refresh-only /serverinfo fetch (plan M5.3). Re-reads the
// host's serverinfo — including the CosmicDisplays block — into an already
// initialized SERVER_DATA (one that gs_init() populated). Tries HTTPS first,
// then HTTP, mirroring load_server_status()'s retry loop. Returns GS_OK on
// success. The caller must have called gs_init() first so the client cert,
// unique id and http/https ports are set up.
int gs_load_serverinfo(PSERVER_DATA server);
int gs_start_app(PSERVER_DATA server, PSTREAM_CONFIGURATION config, int appId, bool sops, bool localaudio, int gamepad_mask);
int gs_applist(PSERVER_DATA server, PAPP_LIST *app_list);
int gs_unpair(PSERVER_DATA server);
int gs_pair(PSERVER_DATA server, char* pin);
int gs_quit_app(PSERVER_DATA server);
