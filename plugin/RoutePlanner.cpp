// RoutePlanner — SPF-Framework plugin for ETS2 1.61.1.1. F8 opens a window to pick origin and
// destination cities (filtered by country) and a cargo, and starts that job right away; it also
// shows and cancels the current job. Single player only.
#include <SPF_GameConsole_API.h>
#include <SPF_KeyBinds_API.h>
#include <SPF_Logger_API.h>
#include <SPF_Manifest_API.h>
#include <SPF_Plugin.h>
#include <SPF_Telemetry_API.h>
#include <SPF_UI_API.h>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <set>
#include <string>

#include "escort.h"
#include "game.h"
#include "routes.h"

namespace {

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
bool g_escort = true;         // police car of the origin country following the truck (see escort.h)
bool g_escort_supported = false;
bool g_escort_armed = false;  // a job we started is running
bool g_escort_seen_job = false; // telemetry has reported that job (it lags the start by a few frames)
float g_escort_from_km = 0;   // odometer when the job started
std::string g_escort_model;
escort::Car g_escort_car;
int g_escort_wait = 0;        // frames until the next spawn attempt / until the spawned car is looked up
bool g_escort_spawned = false; // a spawn was requested; find the car when g_escort_wait runs out
escort::Vec g_escort_spawn_at;
int g_escort_log = 0;
std::vector<escort::Car> g_escort_all; // every car we spawned and have not deleted yet
double g_escort_dist = 0, g_escort_ahead = 0;
std::string g_escort_note = "aguardando um serviço iniciado pelo planejador";
bool g_escort_force = false;   // panel: spawn now, ignoring the 500 m rule
SPF_Window_Handle* g_escort_window = nullptr;
std::atomic<bool> g_escort_toggle{false};
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

enum class Pending { None, Start, Cancel, Teleport, Create };
Pending g_pending = Pending::None;

void Log(const std::string& msg) {
  if (g_core && g_log) g_core->logger->Log(g_log, SPF_LOG_INFO, msg.c_str());
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

// The mouse belongs to the window while it is open (cursor shown, camera still). One call site only:
// SPF keys mouse-block requests by return address.
__declspec(noinline) void SetMouseBlocked(SPF_UI_API* ui, bool blocked) { ui->UI_SetMouseBlockState(blocked, blocked, false); }

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
  ui->UI_Checkbox("Escolta policial após 500 m (experimental)", &g_escort);
  ui->UI_EndDisabled();
  ui->UI_BeginDisabled(!can);
  if (ui->UI_Button("Iniciar serviço", -1, 34)) g_pending = Pending::Start;
  ui->UI_EndDisabled();
  if (on_job) ui->UI_TextDisabled("Cancele o serviço atual para iniciar outro.");
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
    DrawCurrentJob(ui);
    ui->UI_SeparatorText("Origem");
    PlaceCombos(ui, "src", g_src);
    ui->UI_SeparatorText("Destino");
    PlaceCombos(ui, "dst", g_dst);
    SPF_JobData jd{};
    if (g_tel) g_core->telemetry->Tel_GetJobData(g_tel, &jd, sizeof jd);
    DrawCargo(ui, jd.on_job);
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
  std::erase_if(g_options, [](const RouteOption& o) { return !game::CargoExists(Token(o.cargo.c_str())); });
  Log("opções " + g_options_for + ": " + std::to_string(g_options.size()));
}

// ---- police escort (escort.h) ----
int RemoveEscortCars();

void ArmEscort() {
  const Named* city = Find(g_data.cities, g_src.city);
  const Named* country = city ? Find(g_data.countries, city->parent) : nullptr;
  g_escort_model = country ? country->parent : "";
  SPF_TruckData td{};
  if (g_tel) g_core->telemetry->Tel_GetTruckData(g_tel, &td, sizeof td);
  g_escort_from_km = td.odometer;
  g_escort_armed = !g_escort_model.empty();
  g_escort_seen_job = false;
  RemoveEscortCars(); // leftovers of a previous job
  g_escort_wait = 0;
  g_escort_note = g_escort_armed ? "aguardando 500 m com o caminhão em movimento" : "sem modelo de polícia para o país de origem";
  Log("escolta: " + (g_escort_armed ? "armada, modelo " + g_escort_model : std::string("sem modelo de polícia para o país de origem")));
}

// Deletes every car we spawned (the one following and the ones we lost on the way).
int RemoveEscortCars() {
  int n = 0;
  for (const auto& c : g_escort_all) n += escort::Remove(c);
  g_escort_all.clear();
  g_escort_car = {};
  g_escort_spawned = false;
  return n;
}

void UpdateEscort() {
  if (!g_tel) return;
  SPF_JobData jd{};
  g_core->telemetry->Tel_GetJobData(g_tel, &jd, sizeof jd);
  if (g_escort_armed) {
    if (jd.on_job) g_escort_seen_job = true;
    else if (g_escort_seen_job) { // delivered or cancelled: the escort goes away with the job
      g_escort_armed = false;
      const int n = RemoveEscortCars();
      g_escort_note = "serviço terminou";
      Log("escolta: serviço terminou, " + std::to_string(n) + " carro(s) excluído(s)");
      return;
    }
  }
  if (!g_escort_supported || g_escort_model.empty()) return;
  if (!g_escort && !g_escort_force && !g_escort_car.ptr) return;
  SPF_TruckData td{};
  g_core->telemetry->Tel_GetTruckData(g_tel, &td, sizeof td);
  const escort::Vec truck{td.world_placement.position.x, td.world_placement.position.y, td.world_placement.position.z};
  const double heading = td.world_placement.orientation.heading;
  char line[256];

  if (g_escort_car.ptr) {
    if (escort::Follow(g_escort_car, truck, heading, td.speed, &g_escort_dist, &g_escort_ahead) == escort::State::Lost) {
      std::snprintf(line, sizeof line, "escolta: perdida (dist %.0f m, à frente %.0f m), carro excluído", g_escort_dist, g_escort_ahead);
      Log(line);
      RemoveEscortCars();
      g_escort_note = "carro perdido; outro em instantes";
      g_escort_wait = 120;
    } else if (++g_escort_log % 600 == 0) {
      std::snprintf(line, sizeof line, "escolta: seguindo, dist %.0f m, caminhão %.0f km/h", g_escort_dist, td.speed * 3.6);
      Log(line);
    }
    return;
  }
  if (g_escort_wait > 0 && --g_escort_wait > 0) return;
  if (g_escort_spawned) { // the car asked for a moment ago should be in the traffic list now
    g_escort_spawned = false;
    g_escort_car = escort::Find(g_escort_model.c_str(), g_escort_spawn_at, 150.0);
    if (g_escort_car.ptr) g_escort_all.push_back(g_escort_car);
    g_escort_note = g_escort_car.ptr ? "seguindo" : "o carro não apareceu no tráfego";
    Log("escolta: " + g_escort_note);
    if (!g_escort_car.ptr) g_escort_wait = 300;
    return;
  }
  const bool due = g_escort_armed && g_escort_seen_job && td.odometer - g_escort_from_km >= 0.5f && td.speed >= 3.0f;
  if (!due && !g_escort_force) {
    if (g_escort_armed) g_escort_note = "aguardando 500 m com o caminhão em movimento";
    return;
  }
  g_escort_force = false;
  const escort::Vec f = escort::Forward(heading);
  g_escort_spawn_at = {truck.x - f.x * escort::SPAWN_BEHIND, truck.y, truck.z - f.z * escort::SPAWN_BEHIND};
  const bool ok = escort::Spawn(g_escort_model.c_str(), g_escort_spawn_at, heading);
  std::snprintf(line, sizeof line, "escolta: spawn %s em [%.1f; %.1f; %.1f]: %s", g_escort_model.c_str(), g_escort_spawn_at.x, g_escort_spawn_at.y,
                g_escort_spawn_at.z, ok ? "ok" : "falhou (motivo no game.log.txt)");
  Log(line);
  g_escort_note = ok ? "carro pedido ao jogo" : "o jogo recusou o spawn aqui; nova tentativa em instantes";
  g_escort_spawned = ok;
  g_escort_wait = ok ? 5 : 300;
}

// The F9 panel: what the escort is doing, and manual control over it.
void DrawEscort(SPF_UI_API* ui, void*) {
  std::lock_guard lock(g_mu);
  DrawCursor(ui);
  SPF_Font_Handle font = ui->UI_GetFont("rp_body");
  if (font) ui->UI_PushFont(font);
  char line[256];
  if (!g_escort_supported) ui->UI_TextColored(0.9f, 0.3f, 0.25f, 1.0f, "Versão do jogo não reconhecida: escolta desligada.");
  ui->UI_Checkbox("Escolta automática (500 m após iniciar o serviço)", &g_escort);
  ui->UI_SeparatorText("Estado");
  ui->UI_TextWrapped(("Situação: " + g_escort_note).c_str());
  ui->UI_Text(("Modelo: " + (g_escort_model.empty() ? std::string("(nenhum; inicie um serviço pelo F8)") : g_escort_model)).c_str());
  std::snprintf(line, sizeof line, "Carros criados e ainda no mundo: %d", static_cast<int>(g_escort_all.size()));
  ui->UI_Text(line);

  ui->UI_SeparatorText("Carro da escolta");
  const escort::Info i = escort::Read(g_escort_car);
  if (!i.valid) {
    ui->UI_TextDisabled("Nenhum carro seguindo agora.");
  } else {
    std::snprintf(line, sizeof line, "%s  (id %u)", i.model, i.id);
    ui->UI_Text(line);
    std::snprintf(line, sizeof line, "Distância: %.0f m   %s", g_escort_dist, g_escort_ahead > 0 ? "(à frente do caminhão)" : "(atrás do caminhão)");
    ui->UI_Text(line);
    std::snprintf(line, sizeof line, "Velocidade: %.0f km/h   Limite dado: %.0f km/h   Alvo da IA: %.0f km/h", i.speed * 3.6, i.limit * 3.6, i.target * 3.6);
    ui->UI_Text(line);
    std::snprintf(line, sizeof line, "Posição: [%.1f; %.1f; %.1f]", i.pos.x, i.pos.y, i.pos.z);
    ui->UI_Text(line);
    std::snprintf(line, sizeof line, "Flags da IA: %016llX%s%s", static_cast<unsigned long long>(i.flags),
                  i.flags & escort::FLAG_DEBUG_PAUSE ? "  [pausado]" : "", i.flags & escort::FLAG_REMOVE ? "  [sendo removido]" : "");
    ui->UI_Text(line);
  }

  ui->UI_SeparatorText("Ações");
  const bool can_spawn = g_escort_supported && !g_escort_model.empty() && !g_escort_spawned;
  ui->UI_BeginDisabled(!can_spawn);
  if (ui->UI_Button(i.valid ? "Trocar por um carro novo" : "Criar carro agora", -1, 0)) {
    RemoveEscortCars();
    g_escort_force = true;
    g_escort_wait = 0;
    g_escort_note = "criando carro a pedido";
  }
  ui->UI_EndDisabled();
  ui->UI_BeginDisabled(g_escort_all.empty());
  if (ui->UI_Button("Excluir carro(s) da escolta", -1, 0)) {
    const int n = RemoveEscortCars();
    g_escort_wait = 600; // give the player ~10 s before the automatic escort tries again
    g_escort_note = std::to_string(n) + " carro(s) excluído(s)";
    Log("escolta: " + g_escort_note + " pelo painel");
  }
  ui->UI_EndDisabled();
  ui->UI_TextDisabled("F9 fecha");
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
  } else if (what == Pending::Teleport) {
    Teleport();
  }
}

void OnToggleKey() { g_toggle = true; }
void OnEscortKey() { g_escort_toggle = true; }

void OnUpdate() {
  if (!g_core || !g_core->ui || !g_window) return;
  SPF_UI_API* ui = g_core->ui;
  if (g_toggle.exchange(false)) ui->UI_SetVisibility(g_window, !ui->UI_IsVisible(g_window));
  if (g_escort_window && g_escort_toggle.exchange(false)) ui->UI_SetVisibility(g_escort_window, !ui->UI_IsVisible(g_escort_window));
  // Esc in SPF can also close them; the mouse is ours while either window is open
  const bool open = ui->UI_IsVisible(g_window) || (g_escort_window && ui->UI_IsVisible(g_escort_window));
  if (open != g_mouse_taken) {
    SetMouseBlocked(ui, open);
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

// =================================================================================================
// Registration and lifecycle
// =================================================================================================
const uint16_t kGlyphs[] = {0x0020, 0x00FF, 0x2026, 0x2026, 0x2190, 0x2192, 0x20AC, 0x20AC, 0};

void OnRegisterUI(SPF_UI_API* ui) {
  const auto flags = static_cast<SPF_WindowFlags>(SPF_WINDOW_FLAG_NO_COLLAPSE | SPF_WINDOW_FLAG_NO_SAVED_SETTINGS);
  ui->UI_RegisterDrawCallbackWithFlags(PLUGIN_NAME, "Planejador", Draw, nullptr, flags);
  g_window = ui->UI_GetWindowHandle(PLUGIN_NAME, "Planejador");
  if (g_window) ui->UI_SetVisibility(g_window, false);
  ui->UI_RegisterDrawCallbackWithFlags(PLUGIN_NAME, "Escolta", DrawEscort, nullptr, flags);
  g_escort_window = ui->UI_GetWindowHandle(PLUGIN_NAME, "Escolta");
  if (g_escort_window) ui->UI_SetVisibility(g_escort_window, false);
  static bool fonts_requested = false;
  if (!fonts_requested) {
    fonts_requested = true;
    char win[MAX_PATH] = {};
    GetWindowsDirectoryA(win, MAX_PATH);
    const SPF_Font_Config body{18.0f, false, kGlyphs};
    ui->UI_LoadFontFromFile("rp_body", (std::string(win) + "\\Fonts\\seguisb.ttf").c_str(), &body);
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
  api->Defaults_AddKeybind(h, "Routes", "escort", "keyboard", "KEY_F9", "always");
  api->Meta_AddKeybind(h, "Routes", "escort", "Painel da escolta", "Abre/fecha o painel do carro de polícia que acompanha o caminhão.");
  // name, visible, interactive, x, y, w, h, collapsed, autoscroll
  api->Defaults_AddWindow(h, "Planejador", false, true, 560, 120, 520, 760, false, false);
  api->Meta_AddWindow(h, "Planejador", "Planejador de rotas", "Origem, destino, carga e o serviço atual.");
  api->Defaults_AddWindow(h, "Escolta", false, true, 24, 120, 500, 430, false, false);
  api->Meta_AddWindow(h, "Escolta", "Escolta policial", "Dados e controle do carro de polícia que acompanha o caminhão.");
}

void OnLoad(const SPF_Load_API* load) {
  if (load && load->logger) g_log = load->logger->Log_GetContext(PLUGIN_NAME);
}

void OnActivated(const SPF_Core_API* core) {
  g_core = core;
  if (core->telemetry) g_tel = core->telemetry->Tel_GetContext(PLUGIN_NAME);
  if (core->keybinds)
    if (SPF_KeyBinds_Handle* keys = core->keybinds->Kbind_GetContext(PLUGIN_NAME)) {
      core->keybinds->Kbind_Register(keys, "Routes.toggle", OnToggleKey);
      core->keybinds->Kbind_Register(keys, "Routes.escort", OnEscortKey);
    }
  g_loaded = LoadRoutes(PluginDir() + "routes.tsv", g_data);
  g_supported = game::Supported();
  g_escort_supported = g_supported && escort::Supported();
  Log(std::string("ativado: ") + std::to_string(g_data.cities.size()) + " cidades, " + std::to_string(g_data.cargo_names.size()) + " cargas; jogo " +
      (g_supported ? "reconhecido" : "NÃO reconhecido") + ". F8 abre.");
}

void OnUnload() {
  RemoveEscortCars(); // the cars only make sense while we steer them
  g_escort_window = nullptr;
  g_escort_armed = false;
  if (g_core && g_core->ui && g_mouse_taken) {
    SetMouseBlocked(g_core->ui, false);
    g_core->ui->UI_SetMouseOverride(false);
  }
  g_mouse_taken = false;
  g_core = nullptr;
  g_log = nullptr;
  g_tel = nullptr;
  g_window = nullptr;
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
