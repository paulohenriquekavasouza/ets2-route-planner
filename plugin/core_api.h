// Contract between the host (RoutePlanner.dll: loaded by SPF, owns the manifest, keys, windows and
// fonts, never reloaded while the game runs) and the core (core\RoutePlannerCore.dll: all the logic
// and drawing, reloaded by the host whenever the file changes). Plain C data only: each DLL has its
// own CRT. Same scheme as ets2-police.
#pragma once
#include <SPF_Logger_API.h>
#include <SPF_Plugin.h>
#include <SPF_Telemetry_API.h>
#include <SPF_UI_API.h>

struct CoreApi {
  const SPF_Core_API* core;
  SPF_Logger_Handle* log;
  SPF_Telemetry_Handle* tel;
  const char* plugin_dir; // folder of RoutePlanner.dll (routes.tsv, favorites.tsv), with a trailing backslash
  int reloads;            // how many times the core has been loaded this session (1 = first)
  // SPF keys mouse-block requests by the caller's return address, so the call must come from the
  // host: a request made by a core that was then unloaded could never be withdrawn.
  void (*SetMouseBlocked)(bool blocked);
};

enum CoreKey { CORE_KEY_PLANNER = 0, CORE_KEY_NATIVE = 1 };

struct CoreExports {
  void (*Update)();
  void (*DrawPlanner)(SPF_UI_API* ui);
  void (*Key)(int key);
  // The DLL is about to be unloaded. game_calls_ok = false when the whole framework is going down
  // (the game's "sdk reinit"): calling into the game there took the game down once.
  void (*Shutdown)(bool game_calls_ok);
};

// The core's single export: extern "C" bool Core_Init(const CoreApi*, CoreExports*).
typedef bool (*Core_Init_Fn)(const CoreApi* api, CoreExports* out);
