// RoutePlanner — host DLL loaded by SPF-Framework. Owns everything SPF must see exactly once
// (manifest, keybinds, windows, fonts) and forwards the work to core\RoutePlannerCore.dll, which it
// hot-reloads when that file changes on disk (deploy.ps1 replaces it). The core is always loaded
// from a private copy, so the original file is never locked.
#include <SPF_Hooks_API.h>
#include <SPF_KeyBinds_API.h>
#include <SPF_Manifest_API.h>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdio>
#include <mutex>
#include <string>

#include "core_api.h"

namespace {

constexpr int WATCH_EVERY_FRAMES = 30;

const SPF_Core_API* g_core = nullptr;
SPF_Logger_Handle* g_log = nullptr;

std::mutex g_guest_mutex; // serialises every call into the core with (re)loading it
HMODULE g_guest = nullptr;
CoreExports g_ex{};
std::string g_dir;         // this DLL's folder, with a trailing backslash
std::string g_guest_copy;  // the private copy currently loaded
FILETIME g_guest_stamp{};  // write time of the source file that copy was made from
int g_reloads = 0;

void Log(const char* msg) {
  if (g_core && g_log) g_core->logger->Log(g_log, SPF_LOG_INFO, msg);
}

std::string PluginDir() {
  char buf[MAX_PATH] = {};
  HMODULE self = nullptr;
  GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(&PluginDir),
                     &self);
  GetModuleFileNameA(self, buf, MAX_PATH);
  std::string dir(buf);
  dir.erase(dir.find_last_of("\\/") + 1);
  return dir;
}
std::string GuestSource() { return g_dir + "core\\RoutePlannerCore.dll"; }

bool WriteTime(const std::string& path, FILETIME* out) {
  WIN32_FILE_ATTRIBUTE_DATA d;
  if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &d)) return false;
  *out = d.ftLastWriteTime;
  return true;
}

// EXPERIMENT (v4.1.1): a hook on the game's function that draws the route of a map widget,
// bool 0x1014990(map, list*, int64 index, uint32 colour) in 1.61.1.1, so that the core can take the way
// from the truck to the origin out of the list before every draw (done once a frame from Update it was
// seen for a moment each time the game made the list again). The detour lives here because the core is
// unloaded on a hot reload; SPF installs it and owns the trampoline.
// ponytail: g_ex is read without the guest mutex (the draw and the reload are both on the game's main
// thread, as far as seen); a lock here if that ever proves wrong.
using DrawRoute_t = bool (*)(void* map, void* list, int64_t index, uint32_t colour);
DrawRoute_t g_draw_route = nullptr;
bool DrawRouteDetour(void* map, void* list, int64_t index, uint32_t colour) {
  if (const auto before = g_ex.RouteDraw) before(map, list);
  return g_draw_route ? g_draw_route(map, list, index, colour) : false;
}
constexpr const char* kDrawRouteSig = "48 89 54 24 10 57 41 55 48 81 EC B8 00 00 00 4C 8B 52 10 45 8B E9 4C 8B"; // unique in the exe

// One call site for every mouse-block request (see core_api.h).
__declspec(noinline) void SetMouseBlocked(bool blocked) {
  if (g_core && g_core->ui) g_core->ui->UI_SetMouseBlockState(blocked, blocked, false);
}

void UnloadGuest(bool game_calls_ok) {
  if (!g_guest) return;
  if (g_ex.Shutdown) g_ex.Shutdown(game_calls_ok);
  g_ex = {};
  FreeLibrary(g_guest);
  g_guest = nullptr;
  DeleteFileA(g_guest_copy.c_str());
}

// Swap in the core currently on disk. The old core stays if the new file cannot even be copied.
bool ReloadGuest() {
  const std::string src = GuestSource(), live = g_dir + "core\\live\\";
  FILETIME stamp;
  if (!WriteTime(src, &stamp)) {
    Log("núcleo não encontrado: core\\RoutePlannerCore.dll");
    return false;
  }
  CreateDirectoryA(live.c_str(), nullptr);
  char name[64];
  std::snprintf(name, sizeof name, "core-%d.dll", g_reloads + 1);
  const std::string copy = live + name;
  if (!CopyFileA(src.c_str(), copy.c_str(), FALSE)) return false; // mid-write; the watcher retries

  UnloadGuest(true); // a hot reload happens in a normal frame: the old core may tidy up inside the game
  g_guest_stamp = stamp;
  g_guest_copy = copy;
  g_guest = LoadLibraryA(copy.c_str());
  const auto init = g_guest ? reinterpret_cast<Core_Init_Fn>(GetProcAddress(g_guest, "Core_Init")) : nullptr;
  ++g_reloads;
  const CoreApi api{g_core, g_log, g_core->telemetry ? g_core->telemetry->Tel_GetContext(PLUGIN_NAME) : nullptr, g_dir.c_str(), g_reloads, SetMouseBlocked};
  if (!init || !init(&api, &g_ex)) {
    Log("falha ao carregar o núcleo");
    g_ex = {};
    if (g_guest) FreeLibrary(g_guest);
    g_guest = nullptr;
    return false;
  }
  return true;
}

// =================================================================================================
// Forwarders
// =================================================================================================
void OnPlannerKey() {
  std::lock_guard lock(g_guest_mutex);
  if (g_ex.Key) g_ex.Key(CORE_KEY_PLANNER);
}

void OnNativeKey() {
  std::lock_guard lock(g_guest_mutex);
  if (g_ex.Key) g_ex.Key(CORE_KEY_NATIVE);
}

void DrawPlanner(SPF_UI_API* ui, void*) {
  std::lock_guard lock(g_guest_mutex);
  if (g_ex.DrawPlanner) g_ex.DrawPlanner(ui);
  else ui->UI_TextColored(0.89f, 0.27f, 0.24f, 1.0f, "RoutePlanner: núcleo não carregado (core\\RoutePlannerCore.dll)");
}

void OnUpdate() {
  static int frame = 0;
  std::lock_guard lock(g_guest_mutex);
  if (g_core && ++frame % WATCH_EVERY_FRAMES == 0) {
    FILETIME now;
    if (WriteTime(GuestSource(), &now) && CompareFileTime(&now, &g_guest_stamp) != 0) ReloadGuest();
  }
  if (g_ex.Update) g_ex.Update();
}

// =================================================================================================
// Registration and lifecycle
// =================================================================================================
const uint16_t kGlyphs[] = {0x0020, 0x00FF, 0x2026, 0x2026, 0x2190, 0x2192, 0x20AC, 0x20AC, 0};

void OnRegisterUI(SPF_UI_API* ui) {
  // no title bar (the core draws its own header) and sized by its content: the layout has a fixed width
  const auto flags = static_cast<SPF_WindowFlags>(SPF_WINDOW_FLAG_NO_TITLE_BAR | SPF_WINDOW_FLAG_NO_COLLAPSE | SPF_WINDOW_FLAG_NO_SAVED_SETTINGS |
                                                  SPF_WINDOW_FLAG_ALWAYS_AUTO_RESIZE | SPF_WINDOW_FLAG_NO_SCROLLBAR);
  ui->UI_RegisterDrawCallbackWithFlags(PLUGIN_NAME, "Planejador", DrawPlanner, nullptr, flags);
  if (SPF_Window_Handle* w = ui->UI_GetWindowHandle(PLUGIN_NAME, "Planejador")) ui->UI_SetVisibility(w, false);
  static bool fonts_requested = false;
  if (!fonts_requested) {
    fonts_requested = true;
    char win[MAX_PATH] = {};
    GetWindowsDirectoryA(win, MAX_PATH);
    const SPF_Font_Config body{18.0f, false, kGlyphs}, small{14.0f, false, kGlyphs}, title{24.0f, false, kGlyphs};
    ui->UI_LoadFontFromFile("rp_body", (std::string(win) + "\\Fonts\\seguisb.ttf").c_str(), &body);
    ui->UI_LoadFontFromFile("rp_small", (std::string(win) + "\\Fonts\\seguisb.ttf").c_str(), &small);
    ui->UI_LoadFontFromFile("rp_title", (std::string(win) + "\\Fonts\\segoeuib.ttf").c_str(), &title);
  }
}

void BuildManifest(SPF_Manifest_Builder_Handle* h, const SPF_Manifest_Builder_API* api) {
  api->Info_SetName(h, PLUGIN_NAME);
  api->Info_SetVersion(h, PLUGIN_VERSION);
  api->Info_SetMinFrameworkVersion(h, "1.2.0");
  api->Info_SetAuthor(h, "Paulo");
  api->Info_SetDescriptionLiteral(h, "Planejador de rotas: escolha origem, destino e carga e comece o serviço na hora. Somente single-player.");
  api->Policy_SetAllowUserConfig(h, true);
  api->Policy_AddConfigurableSystem(h, "ui");
  api->Defaults_SetLogging(h, "info", false);
  api->Policy_AddRequiredHook(h, "GameConsole"); // g_set_time / g_set_weather before starting a job
  api->Defaults_AddKeybind(h, "Routes", "toggle", "keyboard", "KEY_F8", "always");
  api->Meta_AddKeybind(h, "Routes", "toggle", "Abrir planejador", "Abre/fecha a janela de rotas.");
  // Home = the game-UI experiment. The action is called "escort" because that is the name Paulo's settings.json
  // already holds for Home (from the escort branch) and SPF does not merge new actions into an existing file.
  api->Defaults_AddKeybind(h, "Routes", "escort", "keyboard", "KEY_HOME", "always");
  api->Meta_AddKeybind(h, "Routes", "escort", "Experimento: janela do jogo", "Abre/fecha a janela de teste feita com a interface do próprio jogo.");
  // name, visible, interactive, x, y, w, h, collapsed, autoscroll
  api->Defaults_AddWindow(h, "Planejador", false, true, 560, 120, 520, 760, false, false);
  api->Meta_AddWindow(h, "Planejador", "Planejador de rotas", "Origem, destino, carga e o serviço atual.");
}

void OnLoad(const SPF_Load_API* load) {
  if (load && load->logger) g_log = load->logger->Log_GetContext(PLUGIN_NAME);
}

void OnActivated(const SPF_Core_API* core) {
  g_core = core;
  g_dir = PluginDir();
  if (core->keybinds)
    if (SPF_KeyBinds_Handle* keys = core->keybinds->Kbind_GetContext(PLUGIN_NAME)) {
      core->keybinds->Kbind_Register(keys, "Routes.toggle", OnPlannerKey);
      core->keybinds->Kbind_Register(keys, "Routes.escort", OnNativeKey);
    }
  if (core->hooks && core->hooks->Hook_Register) {
    SPF_Hook_Handle* const hook = core->hooks->Hook_Register(PLUGIN_NAME, "RoutePlanner_DrawRoute", "Rota no mapa do planejador", reinterpret_cast<void*>(&DrawRouteDetour),
                                                             reinterpret_cast<void**>(&g_draw_route), kDrawRouteSig, true);
    Log(hook && g_draw_route ? "gancho no desenho da rota do mapa: instalado" : "gancho no desenho da rota do mapa: NÃO instalado (o corte da rota fica uma vez por quadro)");
  }
  std::lock_guard lock(g_guest_mutex);
  ReloadGuest();
}

void OnUnload() {
  std::lock_guard lock(g_guest_mutex);
  UnloadGuest(false); // inside the game's "sdk reinit": the core must not call into the game
  g_core = nullptr;
  g_log = nullptr;
}

} // namespace

extern "C" {

SPF_PLUGIN_EXPORT bool SPF_GetManifestAPI(SPF_Manifest_API* out_api) {
  if (!out_api) return false;
  out_api->BuildManifest = BuildManifest;
  return true;
}

SPF_PLUGIN_EXPORT bool SPF_GetPlugin(SPF_Plugin_Exports* exports) {
  if (!exports) return false;
  exports->OnLoad = OnLoad;
  exports->OnActivated = OnActivated;
  exports->OnUnload = OnUnload;
  exports->OnUpdate = OnUpdate;
  exports->OnRegisterUI = OnRegisterUI;
  return true;
}

} // extern "C"
