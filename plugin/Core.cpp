// RoutePlannerCore — everything RoutePlanner does (ETS2 1.61.1.1, single player only): the F8 window
// to pick origin, destination and cargo and start that job right away, favourites, the current job,
// and the escort. The host (Host.cpp) loads this DLL and reloads it whenever the file changes, so
// nothing here may outlive Shutdown().
#include <SPF_GameConsole_API.h>
#include <SPF_Logger_API.h>
#include <SPF_Plugin.h>
#include <SPF_Telemetry_API.h>
#include <SPF_UI_API.h>

#include "core_api.h"

#include <atomic>
#include <cstdio>
#include <mutex>
#include <set>
#include <string>

#include "escort.h"
#include "game.h"
#include "routes.h"

namespace {

CoreApi g_api{};
const SPF_Core_API* g_core = nullptr;
SPF_Logger_Handle* g_log = nullptr;
SPF_Telemetry_Handle* g_tel = nullptr;
SPF_Window_Handle* g_window = nullptr;
std::atomic<bool> g_toggle{false};
bool g_mouse_taken = false;
bool g_supported = false;

RouteData g_data;
bool g_loaded = false;

// UI state
struct Side {
  std::string country; // "" = all
  std::string city;
  char filter[32] = {};
};
Side g_src, g_dst;
char g_cargo_filter[48] = {};
std::vector<RouteOption> g_options;
std::string g_options_for; // "src|dst" the options were computed for
bool g_teleport = true;       // drive-free: put the truck at the source company after starting
bool g_release_brake = true;  // and release the parking brake the teleport engages
bool g_morning = true;        // 07:00 and clear weather before the job is created (its deadline counts from then)
// ---- escort (escort.h): cars carried along the truck's trail ----
// One entry per escort vehicle. Today there is only the police car; another vehicle is one more
// entry with its own distance behind the truck (and, when needed, its own way to pick a model).
struct EscortSlot {
  const char* label;  // shown in the panel
  double gap;         // m behind the truck, along its trail
  std::string model;  // traffic vehicle name, resolved when the escort is called
  escort::Car car;
  escort::Car parked;     // the previous car, stopped where it could no longer follow; deleted when a new one is accepted
  bool requested = false; // spawn asked; look the car up when `wait` runs out
  int wait = 0;           // frames until the next spawn attempt / lookup
  int attempt = 0;        // spawns tried for the current car: each one a little farther back
  int streak = 0;         // tries in a row without an accepted car (ESCORT_MAX_TRIES of them = a long pause)
  int astray = 0;         // frames in a row the car has been off our path or in front of us
  double hint = -1;       // m behind us it was last seen on the trail (keeps it on the right stretch after a U-turn)
  escort::Vec spawn_at;
  escort::Place place;    // where the car is relative to our trail
  float want = 0;         // m/s asked of it
  std::string note = "sem carro";
};
std::vector<EscortSlot> g_escort_slots = {{"Polícia", 30.0}};
// Every try creates (and usually deletes) a vehicle inside the game: never in an endless burst.
constexpr int ESCORT_MAX_TRIES = 10;         // tries in a row for one car...
constexpr int ESCORT_COOLDOWN_FRAMES = 1200; // ...then this long without touching the spawner (~20 s)

bool g_escort = true;           // call the escort by itself 250 m into a job started by the planner
bool g_escort_supported = false;
bool g_escort_active = false;   // the escort should exist right now (called by the job rule or by Home)
bool g_escort_lights = true;    // roof lights on (escort::LightsOn)
int g_escort_lights_failed = 0; // calls that faulted: after a few we stop asking
bool g_escort_armed = false;    // a job we started is running
bool g_escort_seen_job = false; // telemetry has reported that job (it lags the start by a few frames)
// Metres the truck has driven, summed here from its position every frame. The telemetry odometer is
// a float whose steps let a forced curve go the moment the truck left it (log of 2026-10-04: the same
// curve forced again every 0.3 s), before the escort 30 m behind got there.
double g_escort_travel = 0;
double g_escort_from_m = 0;     // g_escort_travel when the job started
std::vector<escort::Car> g_escort_all; // every car we spawned and have not deleted yet
escort::Trail g_escort_trail;   // where the truck has been
// Junction curves the truck drove, forced so the escort takes the same way; released once it is through.
struct ForcedCurve {
  escort::Curve curve;
  double at_m; // g_escort_travel when the truck was last on it
};
std::vector<ForcedCurve> g_escort_forced;
std::string g_escort_note = "Home cria a escolta agora; num serviço iniciado pelo planejador ela vem após 250 m";
int g_escort_log = 0;
SPF_Window_Handle* g_escort_window = nullptr;
std::atomic<bool> g_escort_toggle{false}; // open/close the panel
std::atomic<bool> g_escort_call{false};   // Home: (re)create the escort now
int g_start_in = -1;          // frames until the job is created after the console commands (-1 = none)
int g_teleport_in = -1;       // frames until the deferred teleport runs (-1 = none)
int g_tp_check_in = -1;       // frames until the position after teleporting is logged
bool g_any_cargo = false;     // also list cargo the chosen companies don't normally trade
bool g_cargo_pending = false; // options still to be checked against the game's cargo list
std::mutex g_mu;             // Draw and OnUpdate share the state above
int g_selected = -1;
bool g_confirm_cancel = false;
std::string g_status;
bool g_status_error = false;

enum class Pending { None, Start, Cancel, Teleport, Create, Longest, CurrentCity };

enum class View { Planner, Favorites };
View g_view = View::Planner;
std::vector<Favorite> g_favorites;
int g_editing = -1; // favourite being edited in the planner (-1 = none)
Pending g_pending = Pending::None;

void Log(const std::string& msg) {
  if (g_core && g_log) g_core->logger->Log(g_log, SPF_LOG_INFO, msg.c_str());
}

std::string PluginDir() { return g_api.plugin_dir ? g_api.plugin_dir : ""; } // the host's folder; this DLL runs from core\live\

const Named* Find(const std::vector<Named>& v, const std::string& tok) {
  for (const auto& n : v)
    if (n.tok == tok) return &n;
  return nullptr;
}
std::string CityLabel(const std::string& tok) {
  const Named* c = Find(g_data.cities, tok);
  return c ? c->name : "";
}

// Case-insensitive ASCII substring match (good enough for city and cargo names).
bool Matches(const std::string& text, const char* filter) {
  if (!*filter) return true;
  auto low = [](std::string s) {
    for (auto& ch : s) ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
    return s;
  };
  return low(text).find(low(filter)) != std::string::npos;
}


// =================================================================================================
// Drawing
// =================================================================================================
constexpr float kLabelW = 70.0f;

void PlaceCombos(SPF_UI_API* ui, const char* id, Side& side) {
  const Named* country = Find(g_data.countries, side.country);
  std::string label = std::string("##country") + id;
  ui->UI_AlignTextToFramePadding();
  ui->UI_Text("País");
  ui->UI_SameLine(kLabelW, -1);
  ui->UI_SetNextItemWidth(-1);
  if (ui->UI_BeginCombo(label.c_str(), country ? country->name.c_str() : "Todos os países", SPF_ComboFlags{})) {
    if (ui->UI_Selectable("Todos os países", side.country.empty(), SPF_SelectableFlags{}, 0, 0)) side.country.clear();
    for (const auto& c : g_data.countries)
      if (ui->UI_Selectable((c.name + "##" + c.tok).c_str(), side.country == c.tok, SPF_SelectableFlags{}, 0, 0)) {
        side.country = c.tok;
        const Named* city = Find(g_data.cities, side.city);
        if (city && city->parent != c.tok) side.city.clear();
      }
    ui->UI_EndCombo();
  }
  const std::string city = CityLabel(side.city);
  label = std::string("##city") + id;
  ui->UI_AlignTextToFramePadding();
  ui->UI_Text("Cidade");
  ui->UI_SameLine(kLabelW, -1);
  ui->UI_SetNextItemWidth(-1);
  if (ui->UI_BeginCombo(label.c_str(), city.empty() ? "Escolha a cidade" : city.c_str(), SPF_ComboFlags{})) {
    ui->UI_SetNextItemWidth(-1);
    ui->UI_InputTextWithHint((std::string("##filter") + id).c_str(), "Buscar cidade…", side.filter, sizeof side.filter, SPF_InputTextFlags{});
    for (const auto& c : g_data.cities) {
      if ((!side.country.empty() && c.parent != side.country) || !Matches(c.name, side.filter)) continue;
      const Named* ctry = Find(g_data.countries, c.parent);
      const std::string row = c.name + (side.country.empty() && ctry ? "  (" + ctry->name + ")" : "") + "##" + c.tok;
      if (ui->UI_Selectable(row.c_str(), side.city == c.tok, SPF_SelectableFlags{}, 0, 0)) side.city = c.tok;
    }
    ui->UI_EndCombo();
  }
}

std::string CompanyLabel(const std::string& tok) {
  for (const auto& b : g_data.branches)
    if (b.tok == tok) return b.name;
  return tok;
}

void SaveFavoritesFile() {
  if (!SaveFavorites(PluginDir() + "favorites.tsv", g_favorites)) Log("não consegui gravar favorites.tsv");
}

void AddFavorite(const Favorite& f);

Favorite SelectedRoute() {
  const RouteOption& o = g_options[g_selected];
  return {g_src.city, g_dst.city, o.cargo, o.src_company, o.dst_company};
}

// Puts cities into the planner (country filters follow them) without choosing a cargo.
bool SetCities(const std::string& src_city, const std::string& dst_city) {
  const Named* s = Find(g_data.cities, src_city);
  const Named* d = Find(g_data.cities, dst_city);
  if (!s || !d) return false;
  g_src.country = s->parent, g_src.city = s->tok, g_src.filter[0] = 0;
  g_dst.country = d->parent, g_dst.city = d->tok, g_dst.filter[0] = 0;
  g_cargo_filter[0] = 0;
  return true;
}

// Loads a favourite into the planner with its cargo selected. False if the data no longer has it.
bool ApplyRoute(const Favorite& f) {
  if (!SetCities(f.src_city, f.dst_city)) return false;
  g_any_cargo = true; // the favourite may be an off-market pair
  g_options = RouteOptions(g_data, g_src.city, g_dst.city, true);
  g_options_for = g_src.city + "|" + g_dst.city + "|any";
  g_selected = FindOption(g_options, f);
  if (g_selected >= 0 && g_options[g_selected].cargo != f.cargo) g_selected = -1;
  g_cargo_pending = g_supported && !g_options.empty(); // drops cargo the game doesn't know; keeps the selection
  return g_selected >= 0;
}

void DrawCurrentJob(SPF_UI_API* ui) {
  SPF_JobData jd{};
  if (g_tel) g_core->telemetry->Tel_GetJobData(g_tel, &jd, sizeof jd);
  ui->UI_SeparatorText("Serviço atual");
  if (!jd.on_job) {
    ui->UI_TextDisabled("Nenhum serviço em andamento.");
    g_confirm_cancel = false;
    return;
  }
  SPF_JobConstants jc{};
  g_core->telemetry->Tel_GetJobConstants(g_tel, &jc, sizeof jc);
  char line[512];
  std::snprintf(line, sizeof line, "%s  (%.1f t)", jc.cargo_name, jc.cargo_mass / 1000.0f);
  ui->UI_Text(line);
  std::snprintf(line, sizeof line, "%s, %s  →  %s, %s", jc.source_company, jc.source_city, jc.destination_company, jc.destination_city);
  ui->UI_TextWrapped(line);
  std::snprintf(line, sizeof line, "%u km planejados · prazo em %uh%02u · €%llu", jc.planned_distance_km, jd.remaining_delivery_minutes / 60,
                jd.remaining_delivery_minutes % 60, static_cast<unsigned long long>(jc.income));
  ui->UI_TextDisabled(line);
  if (!g_confirm_cancel) {
    if (ui->UI_Button("Ir até a carga (teleporte)", -1, 0)) {
      g_pending = Pending::Teleport;
    }
    if (ui->UI_Button("Cancelar serviço", -1, 0)) g_confirm_cancel = true;
  } else {
    ui->UI_TextColored(0.95f, 0.65f, 0.2f, 1.0f, "Cancelar mesmo? O jogo cobra a multa de cancelamento.");
    if (ui->UI_Button("Sim, cancelar", 140, 0)) {
      g_pending = Pending::Cancel;
      g_confirm_cancel = false;
    }
    ui->UI_SameLine(0, -1);
    if (ui->UI_Button("Não", 80, 0)) g_confirm_cancel = false;
  }
}

void DrawSaveFavorite(SPF_UI_API* ui);

void DrawCargo(SPF_UI_API* ui, bool on_job) {
  ui->UI_SeparatorText("Carga");
  if (g_src.city.empty() || g_dst.city.empty()) {
    ui->UI_TextDisabled("Escolha origem e destino.");
    return;
  }
  ui->UI_Checkbox("Qualquer carga (mesmo que as empresas não a negociem)", &g_any_cargo);
  const std::string key = g_src.city + "|" + g_dst.city + (g_any_cargo ? "|any" : "");
  if (key != g_options_for) {
    g_options = RouteOptions(g_data, g_src.city, g_dst.city, g_any_cargo);
    g_options_for = key;
    g_selected = -1;
    g_cargo_pending = g_supported && !g_options.empty();
  }
  if (g_cargo_pending) {
    ui->UI_TextDisabled("Consultando o jogo…");
    return;
  }
  if (g_options.empty()) {
    ui->UI_TextDisabled(g_any_cargo ? "Uma das cidades não tem empresas." : "Nenhuma carga liga empresas dessas duas cidades. Marque \"Qualquer carga\".");
    return;
  }
  ui->UI_SetNextItemWidth(-1);
  ui->UI_InputTextWithHint("##cargo_filter", "Buscar carga ou empresa…", g_cargo_filter, sizeof g_cargo_filter, SPF_InputTextFlags{});
  if (ui->UI_BeginListBox("##cargo", -1, 220)) {
    for (int i = 0; i < static_cast<int>(g_options.size()); ++i) {
      const auto& o = g_options[i];
      char mass[24];
      std::snprintf(mass, sizeof mass, "~%.0f t", CargoMass(g_data, o.cargo) / 1000.0);
      const std::string row = CargoName(g_data, o.cargo) + "   ·   " + mass + "   ·   " + o.src_name + " → " + o.dst_name +
                              (o.off_market ? "   (fora do mercado)" : "");
      if (!Matches(row, g_cargo_filter)) continue;
      const bool sel = g_selected == i;
      if (sel) ui->UI_PushStyleColor(SPF_COLOR_HEADER, 0.85f, 0.62f, 0.15f, 0.55f); // SPF's theme leaves selection invisible
      if (ui->UI_Selectable(((sel ? "> " : "   ") + row + "##" + std::to_string(i)).c_str(), sel, SPF_SelectableFlags{}, 0, 0)) g_selected = i;
      if (sel) ui->UI_PopStyleColor(1);
    }
    ui->UI_EndListBox();
  }
  const bool can = g_selected >= 0 && !on_job && g_supported && g_pending == Pending::None && g_start_in < 0;
  ui->UI_Checkbox("Ir até a empresa de origem ao iniciar (teleporte)", &g_teleport);
  ui->UI_Checkbox("Soltar o freio de mão após teleportar", &g_release_brake);
  ui->UI_Checkbox("Antes de iniciar: 7h da manhã e tempo limpo", &g_morning);
  ui->UI_BeginDisabled(!g_escort_supported);
  ui->UI_Checkbox("Escolta policial após 250 m (experimental)", &g_escort);
  ui->UI_EndDisabled();
  ui->UI_BeginDisabled(!can);
  if (ui->UI_Button("Iniciar serviço", -1, 34)) g_pending = Pending::Start;
  ui->UI_EndDisabled();
  if (on_job) ui->UI_TextDisabled("Cancele o serviço atual para iniciar outro.");
  DrawSaveFavorite(ui);
}

// Shown under the cargo list (so origin and destination are set); enabled once a cargo is selected.
void DrawSaveFavorite(SPF_UI_API* ui) {
  const char* missing = g_selected < 0 || g_selected >= static_cast<int>(g_options.size()) ? "Escolha a carga para poder salvar." : nullptr;
  ui->UI_SeparatorText("Favoritas");
  ui->UI_BeginDisabled(missing != nullptr);
  if (g_editing >= 0 && g_editing < static_cast<int>(g_favorites.size())) {
    if (ui->UI_Button("Salvar alterações na favorita", -1, 0)) {
      g_favorites[g_editing] = SelectedRoute();
      SaveFavoritesFile();
      g_status = "Favorita atualizada.";
      g_status_error = false;
      g_editing = -1;
      g_view = View::Favorites;
    }
  } else if (ui->UI_Button("Salvar esta rota como favorita", -1, 0)) {
    AddFavorite(SelectedRoute());
  }
  ui->UI_EndDisabled();
  if (missing) ui->UI_TextDisabled(missing);
  if (g_editing >= 0 && ui->UI_Button("Cancelar edição", -1, 0)) {
    g_editing = -1;
    g_view = View::Favorites;
  }
}

// ---- favourites screen ----
void AddFavorite(const Favorite& f) {
  const bool dup = std::find(g_favorites.begin(), g_favorites.end(), f) != g_favorites.end();
  if (!dup) {
    g_favorites.push_back(f);
    SaveFavoritesFile();
  }
  g_status = dup ? "Essa rota já está nas favoritas." : "Rota adicionada às favoritas.";
  g_status_error = false;
}

void DrawFavorites(SPF_UI_API* ui, bool on_job) {
  ui->UI_SeparatorText("Salvar como favorita");
  ui->UI_BeginDisabled(!on_job);
  if (ui->UI_Button("Salvar o serviço atual", -1, 0)) { // the job in progress, as the game reports it
    SPF_JobConstants jc{};
    g_core->telemetry->Tel_GetJobConstants(g_tel, &jc, sizeof jc);
    AddFavorite({jc.source_city_id, jc.destination_city_id, jc.cargo_id, jc.source_company_id, jc.destination_company_id});
  }
  ui->UI_EndDisabled();
  const bool picked = g_selected >= 0 && g_selected < static_cast<int>(g_options.size());
  ui->UI_BeginDisabled(!picked);
  if (ui->UI_Button("Salvar a rota escolhida em Planejar", -1, 0)) AddFavorite(SelectedRoute());
  ui->UI_EndDisabled();
  if (picked) {
    ui->UI_TextDisabled((CityLabel(g_src.city) + " → " + CityLabel(g_dst.city) + "  ·  " + CargoName(g_data, g_options[g_selected].cargo)).c_str());
  }
  ui->UI_SeparatorText("Rotas favoritas");
  if (g_favorites.empty()) {
    ui->UI_TextWrapped("Nenhuma favorita ainda. Em \"Planejar\", escolha origem, destino e carga e use \"Adicionar esta rota às favoritas\".");
    return;
  }
  const bool can_start = !on_job && g_supported && g_pending == Pending::None && g_start_in < 0;
  int remove = -1;
  for (int i = 0; i < static_cast<int>(g_favorites.size()); ++i) {
    const Favorite& f = g_favorites[i];
    const std::string id = "##fav" + std::to_string(i);
    ui->UI_Text((CityLabel(f.src_city) + "  →  " + CityLabel(f.dst_city)).c_str());
    char mass[24];
    std::snprintf(mass, sizeof mass, "~%.0f t", CargoMass(g_data, f.cargo) / 1000.0);
    ui->UI_TextDisabled((CargoName(g_data, f.cargo) + "  ·  " + mass + "  ·  " + CompanyLabel(f.src_company) + " → " + CompanyLabel(f.dst_company)).c_str());
    ui->UI_BeginDisabled(!can_start);
    if (ui->UI_Button(("Iniciar" + id).c_str(), 150, 0)) {
      if (ApplyRoute(f)) g_pending = Pending::Start; // same path as the planner's button
      else {
        g_status = "Essa favorita não existe mais nos dados do jogo (cidade, empresa ou carga).";
        g_status_error = true;
      }
    }
    ui->UI_EndDisabled();
    ui->UI_SameLine(0, -1);
    if (ui->UI_Button(("Editar" + id).c_str(), 110, 0)) {
      ApplyRoute(f);
      g_editing = i;
      g_view = View::Planner;
    }
    ui->UI_SameLine(0, -1);
    if (ui->UI_Button(("Remover" + id).c_str(), 110, 0)) remove = i;
    ui->UI_Separator();
  }
  if (remove >= 0) {
    g_favorites.erase(g_favorites.begin() + remove);
    SaveFavoritesFile();
    g_editing = -1;
    g_status = "Favorita removida.";
    g_status_error = false;
  }
  if (on_job) ui->UI_TextDisabled("Cancele o serviço atual para iniciar outro.");
}

void DrawTopBar(SPF_UI_API* ui) {
  const std::string favs = "Favoritas (" + std::to_string(g_favorites.size()) + ")";
  const bool planner = g_view == View::Planner;
  if (planner) ui->UI_PushStyleColor(SPF_COLOR_BUTTON, 0.85f, 0.62f, 0.15f, 0.55f);
  if (ui->UI_Button("Planejar", 100, 0)) g_view = View::Planner;
  if (planner) ui->UI_PopStyleColor(1);
  ui->UI_SameLine(0, -1);
  if (!planner) ui->UI_PushStyleColor(SPF_COLOR_BUTTON, 0.85f, 0.62f, 0.15f, 0.55f);
  if (ui->UI_Button(favs.c_str(), 140, 0)) g_view = View::Favorites;
  if (!planner) ui->UI_PopStyleColor(1);
  ui->UI_SameLine(0, -1);
  ui->UI_BeginDisabled(!g_supported || g_pending != Pending::None);
  if (ui->UI_Button("Maior rota", 110, 0)) g_pending = Pending::Longest;
  ui->UI_EndDisabled();
  ui->UI_SameLine(0, -1);
  if (ui->UI_Button("Escolta", 90, 0)) g_escort_toggle = true; // opens/closes the escort panel (handled in OnUpdate)
}

void DrawCursor(SPF_UI_API* ui) { // SPF only shows a cursor for its own windows
  float mx, my;
  ui->UI_GetMousePos(&mx, &my);
  const SPF_DrawList_Handle fg = ui->UI_GetForegroundDrawList();
  ui->UI_DrawList_AddTriangleFilled(fg, mx, my, mx, my + 19, mx + 13, my + 13, ui->UI_ColorConvertFloat4ToU32(1, 1, 1, 1));
  ui->UI_DrawList_AddTriangle(fg, mx, my, mx, my + 19, mx + 13, my + 13, ui->UI_ColorConvertFloat4ToU32(0, 0, 0, 1), 1.5f);
}

void Draw(SPF_UI_API* ui, void*) {
  std::lock_guard lock(g_mu);
  DrawCursor(ui);
  SPF_Font_Handle font = ui->UI_GetFont("rp_body");
  if (font) ui->UI_PushFont(font);
  if (!g_loaded) {
    ui->UI_TextWrapped("routes.tsv não encontrado ao lado da DLL. Rode o deploy.ps1 (ele gera o arquivo a partir dos dados do jogo).");
  } else {
    if (!g_supported) ui->UI_TextColored(0.9f, 0.3f, 0.25f, 1.0f, "Versão do jogo não reconhecida: iniciar/cancelar desligados.");
    DrawTopBar(ui);
    DrawCurrentJob(ui);
    SPF_JobData jd{};
    if (g_tel) g_core->telemetry->Tel_GetJobData(g_tel, &jd, sizeof jd);
    if (g_view == View::Favorites) {
      DrawFavorites(ui, jd.on_job);
    } else {
      if (g_editing >= 0) ui->UI_TextColored(0.95f, 0.75f, 0.3f, 1.0f, "Editando uma favorita: mude o que quiser e salve.");
      ui->UI_SeparatorText("Origem");
      ui->UI_BeginDisabled(!g_supported || g_pending != Pending::None);
      if (ui->UI_Button("Cidade atual", -1, 0)) g_pending = Pending::CurrentCity; // needs the game: runs in OnUpdate
      ui->UI_EndDisabled();
      PlaceCombos(ui, "src", g_src);
      ui->UI_SeparatorText("Destino");
      PlaceCombos(ui, "dst", g_dst);
      DrawCargo(ui, jd.on_job);
    }
  }
  if (!g_status.empty()) {
    ui->UI_Spacing();
    if (g_status_error) ui->UI_TextColored(0.95f, 0.4f, 0.3f, 1.0f, g_status.c_str());
    else ui->UI_TextColored(0.45f, 0.85f, 0.45f, 1.0f, g_status.c_str());
  }
  ui->UI_TextDisabled("F8 fecha");
  if (font) ui->UI_PopFont();
}

// =================================================================================================
// Game actions, run from OnUpdate
// =================================================================================================
// Drop cargo the game doesn't know (mp_job_missing_cargo).
void FilterUnknownCargo() {
  g_cargo_pending = false;
  const bool had = g_selected >= 0 && g_selected < static_cast<int>(g_options.size());
  const Favorite keep = had ? Favorite{g_src.city, g_dst.city, g_options[g_selected].cargo, g_options[g_selected].src_company,
                                       g_options[g_selected].dst_company}
                            : Favorite{};
  std::erase_if(g_options, [](const RouteOption& o) { return !game::CargoExists(Token(o.cargo.c_str())); });
  if (had) {
    g_selected = FindOption(g_options, keep);
    if (g_selected >= 0 && g_options[g_selected].cargo != keep.cargo) g_selected = -1;
  }
  Log("opções " + g_options_for + ": " + std::to_string(g_options.size()));
}

// ---- police escort (escort.h) ----
std::vector<MapPoint> CityPoints();

// Gives the junctions back to the game (everything we forced), or only the ones the escort is past.
void ReleaseCurves(double now_m, double keep_m) {
  std::erase_if(g_escort_forced, [&](const ForcedCurve& f) {
    if (keep_m > 0 && now_m - f.at_m < keep_m) return false;
    escort::ForceCurve(f.curve, false);
    return true;
  });
}

// Deletes every car we spawned (the ones following and the ones we lost on the way).
int RemoveEscortCars() {
  ReleaseCurves(0, 0);
  int n = 0;
  for (const auto& c : g_escort_all) n += escort::Remove(c);
  g_escort_all.clear();
  for (auto& slot : g_escort_slots) {
    slot.car = slot.parked = {};
    slot.requested = false;
    slot.wait = slot.attempt = slot.streak = slot.astray = 0;
    slot.note = "sem carro";
  }
  return n;
}

void ArmEscort() {
  g_escort_from_m = g_escort_travel;
  g_escort_armed = true;
  g_escort_seen_job = false;
  g_escort_active = false;
  RemoveEscortCars(); // leftovers of a previous job
  g_escort_note = "aguardando 250 m com o caminhão em movimento";
  Log("escolta: armada para este serviço");
}

// Police car of the country the truck is in: the country of the nearest city. Asks the game.
std::string PoliceModelHere(const escort::Vec& truck) {
  const int i = NearestPoint(CityPoints(), truck.x, truck.z, nullptr);
  const Named* country = i >= 0 ? Find(g_data.countries, g_data.cities[i].parent) : nullptr;
  return country ? country->parent : "";
}

// Brings the escort in: decides each vehicle's model and lets UpdateEscort spawn them.
void CallEscort(const escort::Vec& truck) {
  RemoveEscortCars();
  const std::string police = PoliceModelHere(truck);
  for (auto& slot : g_escort_slots) {
    slot.model = police; // every slot is a police car for now
    slot.wait = 30;      // the cars just deleted leave first; also keeps a hammered Home key from spawning per press
  }
  g_escort_active = !police.empty();
  g_escort_note = g_escort_active ? "escolta chamada" : "sem modelo de polícia para o país onde o caminhão está";
  Log("escolta: " + g_escort_note + (police.empty() ? "" : " (" + police + ")"));
}

void DropCar(EscortSlot& slot, const std::string& why, int wait) {
  escort::Remove(slot.car);
  std::erase_if(g_escort_all, [&](const escort::Car& c) { return c.ptr == slot.car.ptr; });
  slot.car = {};
  slot.astray = 0;
  slot.note = why;
  slot.wait = wait;
  Log(std::string("escolta (") + slot.label + "): " + why);
}

// One more try without a car. After ESCORT_MAX_TRIES in a row the spawner is left alone for a while.
void EscortTryFailed(EscortSlot& slot, int wait) {
  ++slot.attempt;
  slot.wait = wait;
  if (++slot.streak < ESCORT_MAX_TRIES) return;
  slot.streak = 0;
  slot.wait = ESCORT_COOLDOWN_FRAMES;
  slot.note = std::to_string(ESCORT_MAX_TRIES) + " tentativas sem sucesso; nova rodada em ~20 s";
  Log(std::string("escolta (") + slot.label + "): " + slot.note);
}

void UpdateEscort() {
  if (!g_tel || !g_escort_supported) return;
  // Menus, loading, the map: the world is not being simulated; no spawning, deleting or steering there.
  SPF_GameState gs{};
  g_core->telemetry->Tel_GetGameState(g_tel, &gs, sizeof gs);
  if (gs.paused) return;
  SPF_JobData jd{};
  g_core->telemetry->Tel_GetJobData(g_tel, &jd, sizeof jd);
  SPF_TruckData td{};
  g_core->telemetry->Tel_GetTruckData(g_tel, &td, sizeof td);
  const escort::Vec truck{td.world_placement.position.x, td.world_placement.position.y, td.world_placement.position.z};
  const double heading = td.world_placement.orientation.heading;
  const double dt = std::clamp(static_cast<double>(g_core->ui->UI_GetDeltaTime()), 0.001, 0.1);
  char line[256];
  // no truck in the world yet (all zeros) or garbage: nothing below makes sense
  if (!std::isfinite(truck.x + truck.y + truck.z + heading) || (truck.x == 0 && truck.z == 0)) return;
  static escort::Vec last = truck;
  const double moved = std::hypot(truck.x - last.x, truck.z - last.z);
  if (moved < 50.0) g_escort_travel += moved; // more than that in one frame is a teleport
  last = truck;

  if (g_escort_armed) {
    if (jd.on_job) g_escort_seen_job = true;
    else if (g_escort_seen_job) { // delivered or cancelled: the escort goes away with the job
      g_escort_armed = g_escort_active = false;
      const int n = RemoveEscortCars();
      g_escort_note = "serviço terminou";
      Log("escolta: serviço terminou, " + std::to_string(n) + " carro(s) excluído(s)");
      return;
    }
  }
  if (g_escort_call.exchange(false)) CallEscort(truck); // Home, or the panel's button
  else if (g_escort && g_escort_armed && g_escort_seen_job && !g_escort_active && g_escort_travel - g_escort_from_m >= 250.0 && td.speed >= 3.0f)
    CallEscort(truck); // 250 m into the job, and moving
  g_escort_trail.Add({truck, heading}); // always: a trail already there lets the first car be born in our lane
  if (!g_escort_active) return;

  // Junctions: force the curve the truck is on, so the cars behind take the same exit; release it once
  // the last escort car must be through (its distance behind us plus a margin).
  int why = 0, type = 0;
  const escort::Curve here = escort::CurveAt(truck, &why, &type);
  static int last_why = -1, last_type = -1;
  if (why != last_why || type != last_type) { // diagnostics: what is under the truck, each time it changes
    std::snprintf(line, sizeof line, "escolta: sob o caminhão: etapa %d, tipo de faixa %#x", why, type);
    Log(line);
    last_why = why, last_type = type;
  }
  // A curve without AI access (the turn into a side street the traffic never takes) is only taken as ours
  // after 10 m on it: crossing a junction, such curves are the nearest one for a moment all the time.
  static uint8_t* closed_item = nullptr;
  static double closed_from = 0;
  bool usable = here.item != nullptr;
  if (here.item && !here.ai) {
    if (closed_item != here.item) closed_item = here.item, closed_from = g_escort_travel;
    usable = g_escort_travel - closed_from >= 10.0;
  }
  if (usable) {
    const auto known = std::find_if(g_escort_forced.begin(), g_escort_forced.end(), [&](const ForcedCurve& f) { return f.curve.item == here.item; });
    if (known != g_escort_forced.end()) known->at_m = g_escort_travel; // still on it: the count to its release starts when we leave
    else if (g_escort_forced.size() < 24 && escort::ForceCurve(here, true)) {
      // One forced exit per entry. Where two exits still overlap (the start of a T) the lookup may return
      // the other one first; with both forced the AI picks either (the car went left when we went right).
      // 0x94d090 marks the siblings of a forced curve as blocked (bit 7), which is how we recognise ours:
      // the newest wins, since it is the one we are on after the exits have parted.
      const size_t dropped = std::erase_if(g_escort_forced, [&](const ForcedCurve& f) {
        if (!escort::Blocked(f.curve)) return false;
        escort::ForceCurve(f.curve, false);
        return true;
      });
      if (dropped) { // the old curve left `here` blocked: off (clears every mark of this entry) and on again
        escort::ForceCurve(here, false);
        escort::ForceCurve(here, true);
        Log("escolta: cruzamento, " + std::to_string(dropped) + " curva(s) da mesma entrada solta(s): vale a mais nova");
      }
      g_escort_forced.push_back({here, g_escort_travel});
      // The cars behind chose their exit long ago (the AI plans ~1 km ahead): point their plan at our curve.
      for (const auto& slot : g_escort_slots)
        if (const int r = slot.car.ptr ? escort::Replan(slot.car, here) : 0)
          Log(std::string("escolta (") + slot.label + (r > 0 ? "): ia sair por outro lado; plano refeito para a nossa curva" : "): falha ao refazer o plano"));
      std::snprintf(line, sizeof line, "escolta: cruzamento, curva %p forçada%s (%d ativas)", static_cast<void*>(here.item),
                    here.ai ? "" : " [sem acesso de IA: só pelo plano refeito]", static_cast<int>(g_escort_forced.size()));
      Log(line);
    }
  }
  double farthest = 0;
  for (const auto& slot : g_escort_slots) farthest = std::max(farthest, std::max(slot.gap, slot.car.ptr ? slot.place.behind : 0.0));
  // a slot without a car counts from where the next one is born (up to gap + 60 m back)
  for (const auto& slot : g_escort_slots)
    if (!slot.car.ptr) farthest = std::max(farthest, slot.gap + 60.0);
  ReleaseCurves(g_escort_travel, farthest + 60.0);

  for (auto& slot : g_escort_slots) {
    if (slot.parked.ptr) { // waiting where our path ended for it, lights on, until another car can take over
      if (!escort::Hold(slot.parked, dt)) slot.parked = {};
      else if (g_escort_lights && g_escort_lights_failed < 5) escort::LightsOn(slot.parked, escort::LIGHTS_EMERGENCY);
    }
    if (slot.car.ptr) {
      if (!escort::Steer(slot.car, g_escort_trail, truck, heading, td.speed, slot.gap, dt, slot.hint, &slot.place, &slot.want)) {
        DropCar(slot, "o carro sumiu do tráfego; outro em instantes", 60);
        continue;
      }
      if (slot.place.on_trail) slot.hint = slot.place.behind;
      if (g_escort_lights && g_escort_lights_failed < 5 && !escort::LightsOn(slot.car, escort::LIGHTS_EMERGENCY) && ++g_escort_lights_failed == 5)
        Log("escolta: o jogo recusou ligar as luzes 5 vezes; giroflex desligado nesta sessão");
      // off our path (it took another road), in front of us, or hopelessly far: after 2.5 s like that it is replaced
      const bool astray = !slot.place.on_trail && (slot.place.lateral > 12.0 || slot.place.behind < 0);
      slot.astray = astray || slot.place.behind > 350.0 ? slot.astray + 1 : 0;
      if (astray) escort::Hold(slot.car, dt); // it cannot come the way we went: it stops as far as it got
      if (slot.astray > 150) {
        // It stays there, stopped, while another car is tried behind us. Where the game refuses the spawn
        // ("access not allowed": a yard, a road with no AI lanes) this is the escort, waiting at the entrance.
        if (slot.parked.ptr) {
          escort::Remove(slot.parked);
          std::erase_if(g_escort_all, [&](const escort::Car& c) { return c.ptr == slot.parked.ptr; });
        }
        slot.parked = slot.car;
        slot.car = {};
        slot.astray = 0;
        slot.wait = 30;
        std::snprintf(line, sizeof line, "não pôde seguir (%.0f m atrás, %.0f m de lado): parada ali; outra assim que o jogo permitir", slot.place.behind, slot.place.lateral);
        slot.note = line;
        Log(std::string("escolta (") + slot.label + "): " + line);
        continue;
      }
      slot.note = slot.place.behind > slot.gap + 15 ? "alcançando" : slot.want == 0 ? "parado atrás de você" : "no lugar";
      continue;
    }
    if (slot.wait > 0 && --slot.wait > 0) continue;
    if (slot.requested) { // the car asked for a moment ago should be in the traffic list now: is it where we want it?
      slot.requested = false;
      slot.car = escort::Find(slot.model.c_str(), slot.spawn_at, 150.0);
      escort::Place born;
      if (!slot.car.ptr || !escort::LocateCar(slot.car, g_escort_trail, truck, heading, slot.hint, &born)) {
        slot.car = {};
        slot.note = "o carro não apareceu no tráfego";
        Log(std::string("escolta (") + slot.label + "): " + slot.note);
        EscortTryFailed(slot, 120);
        continue;
      }
      g_escort_all.push_back(slot.car);
      if (!born.Good(escort::MIN_BEHIND)) { // in front of us, another lane, or oncoming: not an escort
        std::snprintf(line, sizeof line, "nasceu no lugar errado (%.0f m atrás, %.0f m de lado, sentido %.1f); tentando mais atrás", born.behind,
                      born.lateral, born.facing);
        DropCar(slot, line, 15);
        EscortTryFailed(slot, 15);
        continue;
      }
      std::snprintf(line, sizeof line, "escolta (%s): carro aceito, %.0f m atrás, %.1f m de lado", slot.label, born.behind, born.lateral);
      Log(line);
      if (slot.parked.ptr) { // the new car took over
        escort::Remove(slot.parked);
        std::erase_if(g_escort_all, [&](const escort::Car& c) { return c.ptr == slot.parked.ptr; });
        slot.parked = {};
      }
      slot.attempt = slot.streak = 0;
      slot.hint = born.on_trail ? born.behind : -1;
      slot.note = "no lugar";
      continue;
    }
    // Ask for the car on the road we just drove, behind its place; each failed try goes 10 m farther back.
    const double behind = slot.gap + 10.0 + 10.0 * (slot.attempt % 6);
    escort::Sample born;
    if (!g_escort_trail.At(behind, &born)) { // no trail that long yet: straight behind the truck
      const escort::Vec f = escort::Forward(heading);
      born = {{truck.x - f.x * behind, truck.y, truck.z - f.z * behind}, heading};
    }
    // AI traffic on our lane around that point makes the game refuse ("no free space") or put the car
    // elsewhere: delete it, and spawn once the game has dropped it.
    // From MIN_BEHIND: nearer than that is our own truck and trailer (which is not AI traffic anyway).
    if (const int cleared = escort::ClearLane(g_escort_trail, truck, heading, escort::MIN_BEHIND, behind + 15.0, g_escort_all.data(), g_escort_all.size())) {
      std::snprintf(line, sizeof line, "escolta (%s): %d veículo(s) do tráfego excluído(s) da faixa, %.0f m atrás", slot.label, cleared, behind);
      Log(line);
      slot.note = "abrindo espaço no tráfego atrás de você";
      slot.wait = 10;
      continue;
    }
    slot.spawn_at = born.p;
    slot.hint = behind; // it is asked for on that stretch of the trail
    const bool ok = escort::Spawn(slot.model.c_str(), slot.spawn_at, born.heading);
    std::snprintf(line, sizeof line, "escolta (%s): spawn %s a %.0f m atrás [%.1f; %.1f; %.1f]: %s", slot.label, slot.model.c_str(), behind,
                  slot.spawn_at.x, slot.spawn_at.y, slot.spawn_at.z, ok ? "ok" : "falhou (motivo no game.log.txt)");
    Log(line);
    slot.note = ok ? "carro pedido ao jogo" : slot.parked.ptr ? "sem acesso aqui: viatura parada onde conseguiu chegar" : "o jogo recusou o spawn aqui; nova tentativa em instantes";
    slot.requested = ok;
    slot.wait = 5;
    if (!ok) EscortTryFailed(slot, 180);
  }
  if (++g_escort_log % 300 == 0) {
    for (const auto& slot : g_escort_slots) {
      if (!slot.car.ptr) continue;
      std::snprintf(line, sizeof line, "escolta (%s): %s, %.0f m atrás (alvo %.0f), %.1f m de lado, %s, pedida %.0f km/h, caminhão %.0f km/h", slot.label,
                    slot.note.c_str(), slot.place.behind, slot.gap, slot.place.lateral, slot.place.on_trail ? "no rastro" : "fora do rastro",
                    slot.want * 3.6, td.speed * 3.6);
      Log(line);
    }
  }
}

// The escort panel: what each escort vehicle is doing, and manual control.
void DrawEscort(SPF_UI_API* ui, void*) {
  std::lock_guard lock(g_mu);
  DrawCursor(ui);
  SPF_Font_Handle font = ui->UI_GetFont("rp_body");
  if (font) ui->UI_PushFont(font);
  char line[256];
  if (!g_escort_supported) ui->UI_TextColored(0.9f, 0.3f, 0.25f, 1.0f, "Versão do jogo não reconhecida: escolta desligada.");
  ui->UI_Checkbox("Chamar sozinha 250 m após iniciar um serviço", &g_escort);
  ui->UI_Checkbox("Giroflex ligado", &g_escort_lights);
  if (g_escort_lights_failed >= 5) ui->UI_TextColored(0.95f, 0.4f, 0.3f, 1.0f, "O jogo recusou ligar as luzes (veja o log).");
  ui->UI_TextWrapped(("Situação: " + g_escort_note).c_str());
  std::snprintf(line, sizeof line, "Carros criados e ainda no mundo: %d", static_cast<int>(g_escort_all.size()));
  ui->UI_Text(line);

  for (const auto& slot : g_escort_slots) {
    ui->UI_SeparatorText(slot.label);
    ui->UI_Text(("Estado: " + slot.note).c_str());
    const escort::Info i = escort::Read(slot.car);
    if (!i.valid) continue;
    std::snprintf(line, sizeof line, "%s  (id %u)", i.model, i.id);
    ui->UI_Text(line);
    std::snprintf(line, sizeof line, "Distância: %.0f m atrás (alvo %.0f m)   De lado: %.1f m (%s)   %s", slot.place.behind, slot.gap, slot.place.lateral,
                  slot.place.side > 0 ? "sua faixa à direita dele" : "sua faixa à esquerda dele", slot.place.on_trail ? "no seu rastro" : "fora do rastro");
    ui->UI_Text(line);
    std::snprintf(line, sizeof line, "Velocidade: %.0f km/h   Pedida: %.0f km/h   Alvo da IA: %.0f km/h", i.speed * 3.6, slot.want * 3.6, i.target * 3.6);
    ui->UI_Text(line);
    std::snprintf(line, sizeof line, "Posição: [%.1f; %.1f; %.1f]", i.pos.x, i.pos.y, i.pos.z);
    ui->UI_Text(line);
    std::snprintf(line, sizeof line, "Flags da IA: %016llX%s%s", static_cast<unsigned long long>(i.flags),
                  i.flags & escort::FLAG_DEBUG_PAUSE ? "  [pausado]" : "", i.flags & escort::FLAG_REMOVE ? "  [sendo removido]" : "");
    ui->UI_Text(line);
  }

  ui->UI_SeparatorText("Ações");
  ui->UI_BeginDisabled(!g_escort_supported);
  if (ui->UI_Button(g_escort_active ? "Trocar a escolta agora (Home)" : "Chamar a escolta agora (Home)", -1, 0)) g_escort_call = true;
  ui->UI_EndDisabled();
  ui->UI_BeginDisabled(g_escort_all.empty() && !g_escort_active);
  if (ui->UI_Button("Dispensar a escolta", -1, 0)) {
    const int n = RemoveEscortCars();
    g_escort_active = false;
    g_escort_note = "escolta dispensada (" + std::to_string(n) + " carro(s) excluído(s))";
    Log("escolta: " + g_escort_note);
  }
  ui->UI_EndDisabled();
  if (font) ui->UI_PopFont();
}

std::string TruckPos() {
  SPF_TruckData td{};
  if (g_tel) g_core->telemetry->Tel_GetTruckData(g_tel, &td, sizeof td);
  char b[96];
  std::snprintf(b, sizeof b, "[%.1f; %.1f; %.1f]", td.world_placement.position.x, td.world_placement.position.y, td.world_placement.position.z);
  return b;
}

bool ParkingBrakeOn() {
  SPF_TruckData td{};
  if (g_tel) g_core->telemetry->Tel_GetTruckData(g_tel, &td, sizeof td);
  return td.parking_brake;
}

void ReleaseBrake(const char* when) {
  const bool was = ParkingBrakeOn();
  const bool ok = game::ReleaseParkingBrake();
  Log(std::string("freio de mão ") + when + ": " + (was ? "puxado" : "solto") + " -> " + (ok ? "soltando" : "falhou"));
}

void Teleport() {
  const std::string before = TruckPos();
  float to[3] = {};
  const bool ok = game::TeleportToTrailerSpot(to);
  char target[96];
  std::snprintf(target, sizeof target, "[%.1f; %.1f; %.1f]", to[0], to[1], to[2]);
  Log(std::string("teleporte para o pátio ") + target + ": " + (ok ? "ok" : "falhou") + ", caminhão em " + before + " -> " + TruckPos());
  g_tp_check_in = 60; // and again a second later, in case the move is applied on a later frame
  if (ok && g_release_brake) ReleaseBrake("logo após o teleporte");
  if (!ok) {
    g_status = "Teleporte falhou (motivo no game.log.txt).";
    g_status_error = true;
  }
}

// Farthest two cities on the map: fills origin and destination and leaves the cargo to the player.
// Where each city of g_data.cities is on the map (same order): the first of its companies the game
// knows. Asks the game, so only from OnUpdate.
std::vector<MapPoint> CityPoints() {
  std::vector<MapPoint> pts(g_data.cities.size());
  for (size_t i = 0; i < g_data.cities.size(); ++i) {
    for (const auto& b : g_data.branches) {
      if (b.parent != g_data.cities[i].tok) continue;
      double c[3];
      if (game::CompanyCenter(Token(b.tok.c_str()), Token(b.parent.c_str()), c)) {
        pts[i] = {c[0], c[2], true};
        break;
      }
    }
  }
  return pts;
}

// Origin = the city nearest to the truck right now.
void PickCurrentCity() {
  SPF_TruckData td{};
  if (g_tel) g_core->telemetry->Tel_GetTruckData(g_tel, &td, sizeof td);
  double metres = 0;
  const int i = NearestPoint(CityPoints(), td.world_placement.position.x, td.world_placement.position.z, &metres);
  if (i < 0) {
    g_status = "Não consegui achar as cidades no jogo (precisa estar com o mapa carregado).";
    g_status_error = true;
    return;
  }
  const Named& city = g_data.cities[i];
  g_src.country = city.parent, g_src.city = city.tok, g_src.filter[0] = 0;
  char msg[200];
  std::snprintf(msg, sizeof msg, "Origem: %s, a cidade mais próxima do caminhão (%.1f km no mapa).", city.name.c_str(), metres / 1000.0);
  g_status = msg;
  g_status_error = false;
  Log(g_status);
}

void PickLongestRoute() {
  const std::vector<MapPoint> pts = CityPoints();
  double metres = 0;
  const auto ends = FarthestPair(pts, &metres);
  if (ends.first < 0 || !SetCities(g_data.cities[ends.first].tok, g_data.cities[ends.second].tok)) {
    g_status = "Não consegui achar as cidades no jogo (precisa estar com o mapa carregado).";
    g_status_error = true;
    return;
  }
  g_any_cargo = true; // every cargo the game knows, so there is always something to pick
  g_editing = -1;
  g_view = View::Planner;
  char msg[256];
  std::snprintf(msg, sizeof msg, "Maior rota: %s → %s, cerca de %.0f km. Escolha a carga e inicie.", CityLabel(g_src.city).c_str(),
                CityLabel(g_dst.city).c_str(), game::MetersToFreightKm(metres));
  g_status = msg;
  g_status_error = false;
  Log(g_status);
}

void RunPending() {
  const Pending what = g_pending;
  g_pending = Pending::None;
  if (what == Pending::Cancel) {
    const bool ok = game::CancelJob();
    g_status = ok ? "Serviço cancelado." : "Não havia serviço para cancelar.";
    g_status_error = !ok;
    Log(g_status);
  } else if (what == Pending::Start) {
    // the game's own console commands; the job is created a few frames later so it sees the new time
    if (g_morning && g_core->console) {
      g_core->console->GCon_ExecuteCommand("g_set_time 7 0");
      g_core->console->GCon_ExecuteCommand("g_set_weather 0");
      Log("antes de iniciar: g_set_time 7 0, g_set_weather 0");
      g_start_in = 5;
    } else {
      g_pending = Pending::Create;
      RunPending();
    }
  } else if (what == Pending::Create && g_selected >= 0 && g_selected < static_cast<int>(g_options.size())) {
    const RouteOption o = g_options[g_selected];
    char err[256] = {};
    int code = -1, tries = 0;
    bool ok = false;
    const double km = game::FreightKm(Token(o.src_company.c_str()), Token(g_src.city.c_str()), Token(o.dst_company.c_str()),
                                      Token(g_dst.city.c_str()));
    Log("distância estimada: " + std::to_string(static_cast<int>(km)) + " km");
    do { // 14 = the trailer spot the game picked is occupied; it picks again on the next try
      ok = game::StartJob(Token(g_src.city.c_str()), Token(g_dst.city.c_str()), Token(o.src_company.c_str()), Token(o.dst_company.c_str()),
                          Token(o.cargo.c_str()), static_cast<float>(km), err, sizeof err, &code);
    } while (!ok && code == 14 && ++tries < 5);
    if (tries) Log("erro 14 (vaga ocupada): " + std::to_string(tries) + " nova(s) tentativa(s)");
    char msg[512];
    std::snprintf(msg, sizeof msg, "start %s %s.%s -> %s.%s%s: %s", o.cargo.c_str(), o.src_company.c_str(), g_src.city.c_str(), o.dst_company.c_str(),
                  g_dst.city.c_str(), o.off_market ? " (fora do mercado)" : "", ok ? "ok" : err);
    Log(msg);
    if (ok && g_teleport) g_teleport_in = 10; // let the new job settle for a few frames first
    if (ok) ArmEscort();
    g_status = ok ? "Serviço iniciado: " + CargoName(g_data, o.cargo) + ", " + CityLabel(g_src.city) + " → " + CityLabel(g_dst.city) : err;
    g_status_error = !ok;
  } else if (what == Pending::Longest) {
    PickLongestRoute();
  } else if (what == Pending::CurrentCity) {
    PickCurrentCity();
  } else if (what == Pending::Teleport) {
    Teleport();
  }
}

void Key(int key) {
  if (key == CORE_KEY_PLANNER) g_toggle = true;
  else if (key == CORE_KEY_ESCORT) g_escort_call = true; // Home: create (or replace) the escort now
}

void Update() {
  if (!g_core || !g_core->ui || !g_window) return;
  SPF_UI_API* ui = g_core->ui;
  if (g_toggle.exchange(false)) ui->UI_SetVisibility(g_window, !ui->UI_IsVisible(g_window));
  if (g_escort_window && g_escort_toggle.exchange(false)) ui->UI_SetVisibility(g_escort_window, !ui->UI_IsVisible(g_escort_window));
  // Esc in SPF can also close them; the mouse is ours (cursor shown, camera still) while either window is open
  const bool open = ui->UI_IsVisible(g_window) || (g_escort_window && ui->UI_IsVisible(g_escort_window));
  if (open != g_mouse_taken) {
    g_api.SetMouseBlocked(open);
    ui->UI_SetMouseOverride(open);
    g_mouse_taken = open;
  }
  std::lock_guard lock(g_mu);
  if (g_cargo_pending) FilterUnknownCargo();
  if (g_pending != Pending::None) RunPending();
  if (g_start_in >= 0 && g_start_in-- == 0) {
    g_pending = Pending::Create;
    RunPending();
  }
  if (g_teleport_in >= 0 && g_teleport_in-- == 0) Teleport();
  UpdateEscort();
  if (g_tp_check_in >= 0 && g_tp_check_in-- == 0) {
    Log("1 s depois do teleporte: caminhão em " + TruckPos());
    if (g_release_brake) ReleaseBrake("1 s depois"); // the game may engage it again once the truck settles
  }
}

void DrawPlanner(SPF_UI_API* ui) { Draw(ui, nullptr); }
void DrawEscortPanel(SPF_UI_API* ui) { DrawEscort(ui, nullptr); }

// The DLL is going away. On a hot reload we tidy up inside the game (escort cars, forced curves); when
// the whole framework unloads (the game's "sdk reinit") calling into the game took it down once
// (MODLOG v2.5.1), so the cars are left to the AI and forced curves stay until the game restarts.
void Shutdown(bool game_calls_ok) {
  std::lock_guard lock(g_mu);
  if (game_calls_ok) RemoveEscortCars();
  g_escort_all.clear();
  g_escort_forced.clear();
  g_escort_active = g_escort_armed = false;
  if (g_core && g_core->ui && g_mouse_taken) {
    g_api.SetMouseBlocked(false);
    g_core->ui->UI_SetMouseOverride(false);
  }
  g_mouse_taken = false;
}

} // namespace

extern "C" __declspec(dllexport) bool Core_Init(const CoreApi* api, CoreExports* out) {
  if (!api || !api->core || !api->core->ui || !api->SetMouseBlocked || !out) return false;
  g_api = *api;
  g_core = api->core;
  g_log = api->log;
  g_tel = api->tel;
  g_window = g_core->ui->UI_GetWindowHandle(PLUGIN_NAME, "Planejador");
  g_escort_window = g_core->ui->UI_GetWindowHandle(PLUGIN_NAME, "Escolta");
  g_loaded = LoadRoutes(PluginDir() + "routes.tsv", g_data);
  g_favorites = LoadFavorites(PluginDir() + "favorites.tsv");
  g_supported = game::Supported();
  g_escort_supported = g_supported && escort::Supported();
  Log("núcleo #" + std::to_string(api->reloads) + " (" __DATE__ " " __TIME__ "): " + std::to_string(g_data.cities.size()) + " cidades, " +
      std::to_string(g_data.cargo_names.size()) + " cargas; jogo " + (g_supported ? "reconhecido" : "NÃO reconhecido") + ". F8 abre, Home chama a escolta.");
  *out = {Update, DrawPlanner, DrawEscortPanel, Key, Shutdown};
  return true;
}
