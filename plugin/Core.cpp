// RoutePlannerCore — everything RoutePlanner does (ETS2 1.61.1.1, single player only): the F8 window
// to pick origin, destination and cargo and start that job right away, favourites and the current
// job. The host (Host.cpp) loads this DLL and reloads it whenever the file changes, so nothing here
// may outlive Shutdown().
#include <SPF_GameConsole_API.h>
#include <SPF_Logger_API.h>
#include <SPF_Plugin.h>
#include <SPF_Telemetry_API.h>
#include <SPF_UI_API.h>

#include "core_api.h"

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <map>
#include <set>
#include <string>
#include <thread>

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
bool g_refuel = true;         // fill the tank when the job starts
bool g_morning = true;        // 07:00 and clear weather before the job is created (its deadline counts from then)
int g_start_in = -1;          // frames until the job is created after the console commands (-1 = none)
int g_teleport_in = -1;       // frames until the deferred teleport runs (-1 = none)
int g_hint_in = -1;           // frames until the "job started" message goes to the game's hint box
int g_hint_off_in = -1;       // frames until it is taken down again
std::string g_hint_text;
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
// Drawing. The look follows the game's own panels since the 1.50 UI (Quick Info, Job Market):
// graphite background, slightly lighter cards with a small grey caption, amber for titles, the
// selected tab and the main action, grey for everything secondary.
// =================================================================================================
struct Rgba {
  float r, g, b, a;
};
constexpr Rgba kBg{0.106f, 0.114f, 0.125f, 0.97f};
constexpr Rgba kCard{0.165f, 0.176f, 0.192f, 1.0f};
constexpr Rgba kField{0.235f, 0.247f, 0.267f, 1.0f};
constexpr Rgba kFieldHover{0.300f, 0.314f, 0.337f, 1.0f};
constexpr Rgba kFieldDown{0.200f, 0.212f, 0.231f, 1.0f};
constexpr Rgba kAmber{0.961f, 0.651f, 0.137f, 1.0f};
constexpr Rgba kAmberHover{1.000f, 0.745f, 0.300f, 1.0f};
constexpr Rgba kAmberDown{0.840f, 0.550f, 0.080f, 1.0f};
constexpr Rgba kText{0.910f, 0.910f, 0.910f, 1.0f};
constexpr Rgba kMuted{0.600f, 0.620f, 0.650f, 1.0f};
constexpr Rgba kInk{0.090f, 0.090f, 0.100f, 1.0f}; // text on amber
constexpr Rgba kRed{0.930f, 0.380f, 0.300f, 1.0f};
constexpr Rgba kGreen{0.470f, 0.800f, 0.450f, 1.0f};

constexpr float kW = 620.0f;  // content width: the window sizes itself around it
constexpr float kPad = 14.0f; // inside a card
constexpr float kGap = 10.0f; // between cards and between buttons side by side

uint32_t U32(SPF_UI_API* ui, Rgba c, float alpha = 1.0f) { return ui->UI_ColorConvertFloat4ToU32(c.r, c.g, c.b, c.a * alpha); }

// Colours and metrics for everything drawn in the window; undone when it goes out of scope.
struct Theme {
  SPF_UI_API* ui;
  int colors = 0, vars = 0;
  void Color(SPF_StyleColor i, Rgba c, float alpha = 1.0f) {
    ui->UI_PushStyleColor(i, c.r, c.g, c.b, c.a * alpha);
    ++colors;
  }
  void Var(SPF_StyleVar i, float v) {
    ui->UI_PushStyleVarFloat(i, v);
    ++vars;
  }
  void Var(SPF_StyleVar i, float x, float y) {
    ui->UI_PushStyleVarVec2(i, x, y);
    ++vars;
  }
  explicit Theme(SPF_UI_API* u) : ui(u) {
    Color(SPF_COLOR_TEXT, kText);
    Color(SPF_COLOR_TEXT_DISABLED, kMuted);
    Color(SPF_COLOR_CHILD_BG, kCard, 0.0f);
    Color(SPF_COLOR_POPUP_BG, kCard);
    Color(SPF_COLOR_BORDER, kField);
    Color(SPF_COLOR_FRAME_BG, kField);
    Color(SPF_COLOR_FRAME_BG_HOVERED, kFieldHover);
    Color(SPF_COLOR_FRAME_BG_ACTIVE, kFieldDown);
    Color(SPF_COLOR_BUTTON, kField);
    Color(SPF_COLOR_BUTTON_HOVERED, kFieldHover);
    Color(SPF_COLOR_BUTTON_ACTIVE, kFieldDown);
    Color(SPF_COLOR_HEADER, kAmber, 0.38f);
    Color(SPF_COLOR_HEADER_HOVERED, kAmber, 0.22f);
    Color(SPF_COLOR_HEADER_ACTIVE, kAmber, 0.50f);
    Color(SPF_COLOR_CHECK_MARK, kAmber);
    Color(SPF_COLOR_SEPARATOR, kField);
    Color(SPF_COLOR_SCROLLBAR_BG, kBg, 0.0f);
    Color(SPF_COLOR_SCROLLBAR_GRAB, kFieldHover);
    Color(SPF_COLOR_SCROLLBAR_GRAB_HOVERED, kMuted);
    Color(SPF_COLOR_SCROLLBAR_GRAB_ACTIVE, kAmber);
    Color(SPF_COLOR_TABLE_HEADER_BG, kField);
    Color(SPF_COLOR_TABLE_BORDER_LIGHT, kField, 0.6f);
    Color(SPF_COLOR_TABLE_BORDER_STRONG, kField);
    Color(SPF_COLOR_TABLE_ROW_BG, kBg, 0.35f);
    Color(SPF_COLOR_TABLE_ROW_BG_ALT, kBg, 0.0f);
    Color(SPF_COLOR_NAV_HIGHLIGHT, kAmber);
    Var(SPF_STYLE_VAR_FRAME_ROUNDING, 4.0f);
    Var(SPF_STYLE_VAR_POPUP_ROUNDING, 4.0f);
    Var(SPF_STYLE_VAR_CHILD_ROUNDING, 4.0f);
    Var(SPF_STYLE_VAR_SCROLLBAR_ROUNDING, 4.0f);
    Var(SPF_STYLE_VAR_SCROLLBAR_SIZE, 11.0f);
    Var(SPF_STYLE_VAR_FRAME_BORDERSIZE, 0.0f);
    Var(SPF_STYLE_VAR_FRAME_PADDING, 10.0f, 7.0f);
    Var(SPF_STYLE_VAR_ITEM_SPACING, kGap, 8.0f);
    Var(SPF_STYLE_VAR_CELL_PADDING, 8.0f, 5.0f);
  }
  ~Theme() {
    ui->UI_PopStyleVar(vars);
    ui->UI_PopStyleColor(colors);
  }
  Theme(const Theme&) = delete;
  Theme& operator=(const Theme&) = delete;
};

// A font loaded by the host ("rp_body", "rp_small", "rp_title"); the current one stays if it is not there (yet).
struct FontScope {
  SPF_UI_API* ui;
  bool on;
  FontScope(SPF_UI_API* u, const char* name) : ui(u) {
    const SPF_Font_Handle f = ui->UI_GetFont(name);
    on = f != nullptr;
    if (on) ui->UI_PushFont(f);
  }
  ~FontScope() {
    if (on) ui->UI_PopFont();
  }
  FontScope(const FontScope&) = delete;
  FontScope& operator=(const FontScope&) = delete;
};

void Colored(SPF_UI_API* ui, Rgba c, const char* text) { ui->UI_TextColored(c.r, c.g, c.b, c.a, text); }

// The small grey line over a card or a field.
void Caption(SPF_UI_API* ui, const char* text) {
  FontScope small(ui, "rp_small");
  Colored(ui, kMuted, text);
}

void Wrapped(SPF_UI_API* ui, Rgba c, const std::string& text, float width) {
  ui->UI_PushTextWrapPos(ui->UI_GetCursorPosX() + width);
  Colored(ui, c, text.c_str());
  ui->UI_PopTextWrapPos();
}

// The main action of a screen (and the selected tab): amber with dark text.
bool Primary(SPF_UI_API* ui, const char* label, float w, float h = 0) {
  ui->UI_PushStyleColor(SPF_COLOR_BUTTON, kAmber.r, kAmber.g, kAmber.b, 1);
  ui->UI_PushStyleColor(SPF_COLOR_BUTTON_HOVERED, kAmberHover.r, kAmberHover.g, kAmberHover.b, 1);
  ui->UI_PushStyleColor(SPF_COLOR_BUTTON_ACTIVE, kAmberDown.r, kAmberDown.g, kAmberDown.b, 1);
  ui->UI_PushStyleColor(SPF_COLOR_TEXT, kInk.r, kInk.g, kInk.b, 1);
  const bool pressed = ui->UI_Button(label, w, h);
  ui->UI_PopStyleColor(4);
  return pressed;
}
bool Tab(SPF_UI_API* ui, const char* label, bool active, float w) { return active ? Primary(ui, label, w, 34) : ui->UI_Button(label, w, 34); }

// The game's tick box: amber with a dark tick when on, a grey box when off. (SPF's own checkbox keeps its blue frame.)
bool Check(SPF_UI_API* ui, const char* label, bool* v) {
  const float box = 22.0f, h = ui->UI_GetFrameHeight();
  float x, y, tw, th;
  ui->UI_GetCursorScreenPos(&x, &y);
  ui->UI_CalcTextSize(label, &tw, &th);
  const bool pressed = ui->UI_InvisibleButton(label, box + 8 + tw, h);
  if (pressed) *v = !*v;
  const bool hot = ui->UI_IsItemHovered(SPF_HoveredFlags{});
  const SPF_DrawList_Handle dl = ui->UI_GetWindowDrawList();
  const float by = y + (h - box) / 2;
  ui->UI_DrawList_AddRectFilled(dl, x, by, x + box, by + box, U32(ui, *v ? (hot ? kAmberHover : kAmber) : (hot ? kFieldHover : kField)), 4.0f, SPF_DrawFlags{});
  if (*v) {
    ui->UI_DrawList_AddLine(dl, x + 5, by + 11.5f, x + 9.5f, by + 16, U32(ui, kInk), 2.5f);
    ui->UI_DrawList_AddLine(dl, x + 9.5f, by + 16, x + 17, by + 6.5f, U32(ui, kInk), 2.5f);
  }
  ui->UI_DrawList_AddText(dl, x + box + 8, y + (h - th) / 2, U32(ui, kText), label);
  return pressed;
}

// A card: a rounded block with a caption, as tall as what is put in it. The block is painted with the
// height measured on the previous frame (ponytail: one frame late when the content changes height;
// the alternative is draw-list channels or fixed heights).
std::map<std::string, float> g_card_h;
struct Card {
  const char* id;
  float x, y, w;
  float inner() const { return w - 2 * kPad; }
};
Card BeginCard(SPF_UI_API* ui, const char* id, const char* caption, float x, float y, float w) {
  const auto known = g_card_h.find(id);
  if (known != g_card_h.end()) ui->UI_DrawList_AddRectFilled(ui->UI_GetWindowDrawList(), x, y, x + w, y + known->second, U32(ui, kCard), 6.0f, SPF_DrawFlags{});
  ui->UI_SetCursorScreenPos(x + kPad, y + kPad);
  ui->UI_BeginGroup();
  if (caption) Caption(ui, caption);
  return {id, x, y, w};
}
Card BeginCard(SPF_UI_API* ui, const char* id, const char* caption, float w = kW) {
  float x, y;
  ui->UI_GetCursorScreenPos(&x, &y);
  return BeginCard(ui, id, caption, x, y, w);
}
// Returns the card's bottom; the cursor is left under it, at its left edge.
float EndCard(SPF_UI_API* ui, const Card& c) {
  ui->UI_EndGroup();
  float mx, my;
  ui->UI_GetItemRectMax(&mx, &my);
  const float h = my - c.y + kPad;
  g_card_h[c.id] = h;
  ui->UI_SetCursorScreenPos(c.x, c.y + h);
  ui->UI_Dummy(c.w, 0);
  return c.y + h;
}

std::string CompanyLabel(const std::string& tok) {
  for (const auto& b : g_data.branches)
    if (b.tok == tok) return b.name;
  return tok;
}

void SaveFavoritesFile() {
  if (!SaveFavorites(PluginDir() + "favorites.tsv", g_favorites)) Log("não consegui gravar favorites.tsv");
}

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

void AddFavorite(const Favorite& f) {
  const bool dup = std::find(g_favorites.begin(), g_favorites.end(), f) != g_favorites.end();
  if (!dup) {
    g_favorites.push_back(f);
    SaveFavoritesFile();
  }
  g_status = dup ? "Essa rota já está nas favoritas." : "Rota adicionada às favoritas.";
  g_status_error = false;
}

bool CanStart(bool on_job) { return !on_job && g_supported && g_pending == Pending::None && g_start_in < 0; }
bool CargoPicked() { return g_selected >= 0 && g_selected < static_cast<int>(g_options.size()); }

// ---- pieces ----
void DrawHeader(SPF_UI_API* ui) {
  float left, top, wx, wy;
  ui->UI_GetCursorScreenPos(&left, &top);
  ui->UI_GetWindowPos(&wx, &wy);
  {
    FontScope title(ui, "rp_title");
    Colored(ui, kAmber, "PLANEJADOR DE ROTAS");
  }
  float tw, th;
  ui->UI_CalcTextSize("F8 fecha", &tw, &th);
  ui->UI_SameLine(left - wx + kW - tw, -1); // SameLine counts from the window's edge, not from its padding
  ui->UI_TextDisabled("F8 fecha");
  float x, y;
  ui->UI_GetCursorScreenPos(&x, &y);
  ui->UI_DrawList_AddRectFilled(ui->UI_GetWindowDrawList(), x, y, x + kW, y + 2, U32(ui, kAmber, 0.75f), 0, SPF_DrawFlags{});
  ui->UI_Dummy(kW, 4);
}

void DrawTabs(SPF_UI_API* ui) {
  const std::string favs = "FAVORITAS (" + std::to_string(g_favorites.size()) + ")";
  const float w = (kW - kGap) / 2;
  if (Tab(ui, "PLANEJAR", g_view == View::Planner, w)) g_view = View::Planner;
  ui->UI_SameLine(0, -1);
  if (Tab(ui, favs.c_str(), g_view == View::Favorites, w)) g_view = View::Favorites;
}

void DrawCurrentJob(SPF_UI_API* ui, const SPF_JobData& jd) {
  const Card card = BeginCard(ui, "job", "SERVIÇO ATUAL");
  if (!jd.on_job) {
    ui->UI_TextDisabled("Nenhum serviço em andamento.");
    g_confirm_cancel = false;
  } else {
    SPF_JobConstants jc{};
    g_core->telemetry->Tel_GetJobConstants(g_tel, &jc, sizeof jc);
    char line[512];
    std::snprintf(line, sizeof line, "%s  ·  %.1f t", jc.cargo_name, jc.cargo_mass / 1000.0f);
    Colored(ui, kAmber, line);
    std::snprintf(line, sizeof line, "%s, %s  →  %s, %s", jc.source_company, jc.source_city, jc.destination_company, jc.destination_city);
    Wrapped(ui, kText, line, card.inner());
    std::snprintf(line, sizeof line, "%u km planejados  ·  prazo em %uh%02u  ·  € %llu", jc.planned_distance_km, jd.remaining_delivery_minutes / 60,
                  jd.remaining_delivery_minutes % 60, static_cast<unsigned long long>(jc.income));
    ui->UI_TextDisabled(line);
    const float half = (card.inner() - kGap) / 2;
    if (!g_confirm_cancel) {
      if (ui->UI_Button("Ir até a carga (teleporte)", half, 0)) g_pending = Pending::Teleport;
      ui->UI_SameLine(0, -1);
      if (ui->UI_Button("Cancelar serviço", half, 0)) g_confirm_cancel = true;
    } else {
      Wrapped(ui, kAmber, "Cancelar mesmo? O jogo cobra a multa de cancelamento.", card.inner());
      if (Primary(ui, "Sim, cancelar", half)) {
        g_pending = Pending::Cancel;
        g_confirm_cancel = false;
      }
      ui->UI_SameLine(0, -1);
      if (ui->UI_Button("Não, manter", half, 0)) g_confirm_cancel = false;
    }
  }
  EndCard(ui, card);
}

void PlaceCombos(SPF_UI_API* ui, const char* id, Side& side, float w) {
  const Named* country = Find(g_data.countries, side.country);
  std::string label = std::string("##country") + id;
  Caption(ui, "PAÍS");
  ui->UI_SetNextItemWidth(w);
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
  Caption(ui, "CIDADE");
  ui->UI_SetNextItemWidth(w);
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

// Origin and destination side by side, with the two shortcuts that fill them in above.
void DrawRoute(SPF_UI_API* ui) {
  const float half = (kW - kGap) / 2;
  ui->UI_BeginDisabled(!g_supported || g_pending != Pending::None);
  if (ui->UI_Button("Origem = cidade atual", half, 0)) g_pending = Pending::CurrentCity; // needs the game: runs in Update
  ui->UI_SameLine(0, -1);
  if (ui->UI_Button("Maior rota possível", half, 0)) g_pending = Pending::Longest;
  ui->UI_EndDisabled();
  float x, y;
  ui->UI_GetCursorScreenPos(&x, &y);
  const Card src = BeginCard(ui, "src", "ORIGEM", x, y, half);
  PlaceCombos(ui, "src", g_src, src.inner());
  const float bottom = EndCard(ui, src);
  const Card dst = BeginCard(ui, "dst", "DESTINO", x + half + kGap, y, half);
  PlaceCombos(ui, "dst", g_dst, dst.inner());
  EndCard(ui, dst);
  ui->UI_SetCursorScreenPos(x, bottom);
  ui->UI_Dummy(kW, 0);
}

void DrawCargo(SPF_UI_API* ui) {
  const Card card = BeginCard(ui, "cargo", "CARGA");
  const char* note = nullptr;
  if (g_src.city.empty() || g_dst.city.empty()) {
    note = "Escolha origem e destino para ver as cargas.";
  } else {
    const std::string key = g_src.city + "|" + g_dst.city + (g_any_cargo ? "|any" : "");
    if (key != g_options_for) {
      g_options = RouteOptions(g_data, g_src.city, g_dst.city, g_any_cargo);
      g_options_for = key;
      g_selected = -1;
      g_cargo_pending = g_supported && !g_options.empty();
    }
    ui->UI_SetNextItemWidth(card.inner() - 190);
    ui->UI_InputTextWithHint("##cargo_filter", "Buscar carga ou empresa…", g_cargo_filter, sizeof g_cargo_filter, SPF_InputTextFlags{});
    ui->UI_SameLine(0, -1);
    Check(ui, "Qualquer carga", &g_any_cargo);
    if (ui->UI_IsItemHovered(SPF_HoveredFlags{})) ui->UI_SetTooltip("Lista todas as cargas, mesmo as que essas empresas não negociam.");
    if (g_cargo_pending) note = "Consultando o jogo…";
    else if (g_options.empty()) note = g_any_cargo ? "Uma das cidades não tem empresas." : "Nenhuma carga liga empresas dessas duas cidades. Marque \"Qualquer carga\".";
  }
  if (note) {
    Wrapped(ui, kMuted, note, card.inner());
    EndCard(ui, card);
    return;
  }
  bool off_market = false;
  const auto flags = static_cast<SPF_TableFlags>(SPF_TABLE_FLAG_ROW_BG | SPF_TABLE_FLAG_SCROLL_Y | SPF_TABLE_FLAG_BORDERS_INNER_H | SPF_TABLE_FLAG_SIZING_STRETCH_PROP);
  if (ui->UI_BeginTable("##cargo", 4, flags, card.inner(), 214, 0)) {
    ui->UI_TableSetupScrollFreeze(0, 1);
    ui->UI_TableSetupColumn("Carga", SPF_TABLE_COLUMN_FLAG_WIDTH_STRETCH, 2.3f, 0);
    ui->UI_TableSetupColumn("Peso", SPF_TABLE_COLUMN_FLAG_WIDTH_FIXED, 58.0f, 0);
    ui->UI_TableSetupColumn("Empresa de origem", SPF_TABLE_COLUMN_FLAG_WIDTH_STRETCH, 1.7f, 0);
    ui->UI_TableSetupColumn("Empresa de destino", SPF_TABLE_COLUMN_FLAG_WIDTH_STRETCH, 1.7f, 0);
    ui->UI_TableHeadersRow();
    for (int i = 0; i < static_cast<int>(g_options.size()); ++i) {
      const auto& o = g_options[i];
      const std::string name = CargoName(g_data, o.cargo) + (o.off_market ? " *" : "");
      if (!Matches(name + " " + o.src_name + " " + o.dst_name, g_cargo_filter)) continue;
      off_market |= o.off_market;
      char mass[24];
      std::snprintf(mass, sizeof mass, "~%.0f t", CargoMass(g_data, o.cargo) / 1000.0);
      ui->UI_TableNextRow(SPF_TableRowFlags{}, 0);
      ui->UI_TableNextColumn();
      if (ui->UI_Selectable((name + "##" + std::to_string(i)).c_str(), g_selected == i, SPF_SELECTABLE_FLAG_SPAN_ALL_COLUMNS, 0, 0)) g_selected = i;
      ui->UI_TableNextColumn();
      ui->UI_TextDisabled(mass);
      ui->UI_TableNextColumn();
      ui->UI_Text(o.src_name.c_str());
      ui->UI_TableNextColumn();
      ui->UI_Text(o.dst_name.c_str());
    }
    ui->UI_EndTable();
  }
  if (off_market) Caption(ui, "* fora do mercado: essas empresas não negociam essa carga normalmente");
  EndCard(ui, card);
}

void DrawStartOptions(SPF_UI_API* ui) {
  const Card card = BeginCard(ui, "opts", "AO INICIAR O SERVIÇO");
  const float second = card.inner() / 2; // SameLine counts from the start of the card's group
  Check(ui, "Teleportar até a empresa de origem", &g_teleport);
  ui->UI_SameLine(second, -1);
  Check(ui, "Soltar o freio de mão", &g_release_brake);
  Check(ui, "7h da manhã e tempo limpo", &g_morning);
  ui->UI_SameLine(second, -1);
  Check(ui, "Abastecer o caminhão", &g_refuel);
  EndCard(ui, card);
}

void DrawPlannerActions(SPF_UI_API* ui, bool on_job) {
  const bool editing = g_editing >= 0 && g_editing < static_cast<int>(g_favorites.size());
  const float side = 210.0f, h = 40.0f;
  ui->UI_BeginDisabled(!CargoPicked());
  if (editing) {
    if (ui->UI_Button("Salvar alterações", side, h)) {
      g_favorites[g_editing] = SelectedRoute();
      SaveFavoritesFile();
      g_status = "Favorita atualizada.";
      g_status_error = false;
      g_editing = -1;
      g_view = View::Favorites;
    }
  } else if (ui->UI_Button("Salvar como favorita", side, h)) {
    AddFavorite(SelectedRoute());
  }
  ui->UI_EndDisabled();
  ui->UI_SameLine(0, -1);
  ui->UI_BeginDisabled(!CargoPicked() || !CanStart(on_job));
  if (Primary(ui, "INICIAR SERVIÇO", kW - side - kGap, h)) g_pending = Pending::Start;
  ui->UI_EndDisabled();
  if (editing && ui->UI_Button("Cancelar edição", side, 0)) {
    g_editing = -1;
    g_view = View::Favorites;
  }
  if (on_job) ui->UI_TextDisabled("Cancele o serviço atual para iniciar outro.");
  else if (!CargoPicked()) ui->UI_TextDisabled("Escolha origem, destino e uma carga.");
}

void DrawFavorites(SPF_UI_API* ui, bool on_job) {
  {
    const Card card = BeginCard(ui, "favsave", "SALVAR COMO FAVORITA");
    const float half = (card.inner() - kGap) / 2;
    ui->UI_BeginDisabled(!on_job);
    if (ui->UI_Button("O serviço atual", half, 0)) { // the job in progress, as the game reports it
      SPF_JobConstants jc{};
      g_core->telemetry->Tel_GetJobConstants(g_tel, &jc, sizeof jc);
      AddFavorite({jc.source_city_id, jc.destination_city_id, jc.cargo_id, jc.source_company_id, jc.destination_company_id});
    }
    ui->UI_EndDisabled();
    ui->UI_SameLine(0, -1);
    ui->UI_BeginDisabled(!CargoPicked());
    if (ui->UI_Button("A rota escolhida em Planejar", half, 0)) AddFavorite(SelectedRoute());
    ui->UI_EndDisabled();
    if (CargoPicked())
      Wrapped(ui, kMuted, "Em Planejar: " + CityLabel(g_src.city) + " → " + CityLabel(g_dst.city) + "  ·  " + CargoName(g_data, g_options[g_selected].cargo), card.inner());
    EndCard(ui, card);
  }
  const Card card = BeginCard(ui, "favlist", "ROTAS FAVORITAS");
  if (g_favorites.empty()) {
    Wrapped(ui, kMuted, "Nenhuma favorita ainda. Em Planejar, escolha origem, destino e carga e use \"Salvar como favorita\".", card.inner());
    EndCard(ui, card);
    return;
  }
  int remove = -1;
  const float bw = 92.0f, text_w = card.inner() - 3 * bw - 3 * kGap - 14;
  // ponytail: fixed-height scrolling list (the window sizes itself to its content); ~5 favourites fit without scrolling
  if (ui->UI_BeginChild("##favs", card.inner(), 400, false, SPF_WindowFlags{})) {
    for (int i = 0; i < static_cast<int>(g_favorites.size()); ++i) {
      const Favorite& f = g_favorites[i];
      const std::string id = "##fav" + std::to_string(i);
      char mass[24];
      std::snprintf(mass, sizeof mass, "~%.0f t", CargoMass(g_data, f.cargo) / 1000.0);
      ui->UI_BeginGroup();
      Wrapped(ui, kText, CityLabel(f.src_city) + "  →  " + CityLabel(f.dst_city), text_w);
      Wrapped(ui, kAmber, CargoName(g_data, f.cargo) + "  ·  " + mass, text_w);
      Wrapped(ui, kMuted, CompanyLabel(f.src_company) + " → " + CompanyLabel(f.dst_company), text_w);
      ui->UI_EndGroup();
      ui->UI_SameLine(text_w + kGap, -1);
      ui->UI_BeginDisabled(!CanStart(on_job));
      if (Primary(ui, ("Iniciar" + id).c_str(), bw)) {
        if (ApplyRoute(f)) g_pending = Pending::Start; // same path as the planner's button
        else {
          g_status = "Essa favorita não existe mais nos dados do jogo (cidade, empresa ou carga).";
          g_status_error = true;
        }
      }
      ui->UI_EndDisabled();
      ui->UI_SameLine(0, -1);
      if (ui->UI_Button(("Editar" + id).c_str(), bw, 0)) {
        ApplyRoute(f);
        g_editing = i;
        g_view = View::Planner;
      }
      ui->UI_SameLine(0, -1);
      if (ui->UI_Button(("Remover" + id).c_str(), bw, 0)) remove = i;
      ui->UI_Separator();
    }
  }
  ui->UI_EndChild();
  if (remove >= 0) {
    g_favorites.erase(g_favorites.begin() + remove);
    SaveFavoritesFile();
    g_editing = -1;
    g_status = "Favorita removida.";
    g_status_error = false;
  }
  EndCard(ui, card);
  if (on_job) ui->UI_TextDisabled("Cancele o serviço atual para iniciar outro.");
}

void DrawCursor(SPF_UI_API* ui) { // SPF only shows a cursor for its own windows
  float mx, my;
  ui->UI_GetMousePos(&mx, &my);
  const SPF_DrawList_Handle fg = ui->UI_GetForegroundDrawList();
  ui->UI_DrawList_AddTriangleFilled(fg, mx, my, mx, my + 19, mx + 13, my + 13, ui->UI_ColorConvertFloat4ToU32(1, 1, 1, 1));
  ui->UI_DrawList_AddTriangle(fg, mx, my, mx, my + 19, mx + 13, my + 13, ui->UI_ColorConvertFloat4ToU32(0, 0, 0, 1), 1.5f);
}

void DrawBody(SPF_UI_API* ui);

void Draw(SPF_UI_API* ui, void*) {
  std::lock_guard lock(g_mu);
  DrawCursor(ui);
  const Theme theme(ui);
  const FontScope body(ui, "rp_body");
  float wx, wy, ww, wh;
  ui->UI_GetWindowPos(&wx, &wy);
  ui->UI_GetWindowSize(&ww, &wh);
  ui->UI_DrawList_AddRectFilled(ui->UI_GetWindowDrawList(), wx, wy, wx + ww, wy + wh, U32(ui, kBg), 8.0f, SPF_DrawFlags{}); // over SPF's own window colour
  float left, top;
  ui->UI_GetCursorScreenPos(&left, &top);
  DrawHeader(ui);
  DrawBody(ui);
  // The window hugs its content (also with a host that still has the old resizable window).
  float ex, ey;
  ui->UI_GetCursorScreenPos(&ex, &ey);
  ui->UI_SetWindowSize(kW + 2 * (left - wx), ey - wy + 4, SPF_COND_ALWAYS);
}

void DrawBody(SPF_UI_API* ui) {
  if (!g_loaded) {
    Wrapped(ui, kRed, "routes.tsv não encontrado ao lado da DLL. Rode o deploy.ps1 (ele gera o arquivo a partir dos dados do jogo).", kW);
    return;
  }
  if (!g_supported) Wrapped(ui, kRed, "Versão do jogo não reconhecida: iniciar e cancelar estão desligados.", kW);
  SPF_JobData jd{};
  if (g_tel) g_core->telemetry->Tel_GetJobData(g_tel, &jd, sizeof jd);
  DrawTabs(ui);
  DrawCurrentJob(ui, jd);
  if (g_view == View::Favorites) {
    DrawFavorites(ui, jd.on_job);
  } else {
    if (g_editing >= 0) Wrapped(ui, kAmber, "Editando uma favorita: mude o que quiser e salve.", kW);
    DrawRoute(ui);
    DrawCargo(ui);
    DrawStartOptions(ui);
    DrawPlannerActions(ui, jd.on_job);
  }
  if (!g_status.empty()) Wrapped(ui, g_status_error ? kRed : kGreen, g_status, kW);
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
    if (ok) { // the game's own message box, once the teleport and its parking brake hint are over
      g_hint_text = "<color value=@@clr_sel@@>Serviço iniciado<br><color value=@@clr_txt@@>" + CargoName(g_data, o.cargo) + "<br>" + CityLabel(g_src.city) + " -> " + CityLabel(g_dst.city) + (g_refuel ? "<br>Tanque cheio" : ""); // the game's font has no arrow glyph
      g_hint_in = 120;
    }
    if (ok && g_refuel) {
      const float before = game::Refuel();
      Log(before < 0 ? std::string("abastecer: caminhão não reconhecido, nada feito") : "abastecido: tinha " + std::to_string(static_cast<int>(before)) + " L, tanque cheio");
    }
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

// =================================================================================================
// EXPERIMENT (Home): the planner as a screen of the game's own UI. F8 stays the ImGui planner.
//
// The game builds its screens from SiiNunit scripts (game.h, OpenGameWindow). Ours is generated here,
// laid out like the game's F1 screen: a full panel, a title row, tabs, cards with a bold heading. It
// uses the game's own pictures: flags (/material/ui/flags/<iso3>), the trailer picture of a cargo
// (/material/ui/cargo_logo/<body>) and company logos (/material/ui/company/small/<company>).
// A window of ours has no handler class, so:
//   - content changes by writing a new script and opening it again (the game stays paused meanwhile);
//   - a click is the left mouse button released while one of our buttons has the pointer over it
//     (bit 24 of the widget's flags);
//   - there is no text box or scrolling list: a place is picked on one page (countries on the left,
//     the cities of the chosen one on the right) and cargo through an index of initials.
// Every button is a plain ui::button with its own faces (normal / pointer over it / pressed), which is
// what lets a selected one be gold. Colours are the game's AABBGGRR.
// The key comes from the host (SPF swallows Home before Windows' key state sees it); Esc closes.
// ponytail: the game keeps every script it has loaded, so each rebuild leaves a few KB behind until the
// game closes. If that ever matters: change widgets in place (needs the game's "set text" call).
// =================================================================================================
void* g_native_window = nullptr;
bool g_native_paused = false; // we paused the game for the window (its cursor only exists while paused)
std::atomic<bool> g_native_toggle{false};

enum class NativePage { Planner, Favorites, Place, Cargo };
NativePage g_native_page = NativePage::Planner;
bool g_native_src = true;     // which side the place page fills
int g_native_list_page = 0;   // page of a paged list
char g_native_letter = 0;     // cargo page: initial shown (0 = all)
std::string g_native_filter;  // cargo page: what was typed (lower case, no accents); empty = no filter
bool g_native_by_weight = false, g_native_reverse = false; // cargo page: order (name A-Z by default; weight = heaviest first)
void* g_native_old = nullptr; // the window being replaced: it stays for a couple of frames so the screen never blinks
int g_native_old_in = 0;      // frames until it is closed
bool g_native_leave = false;  // the action just run needs the game running: close instead of rebuilding
int g_native_pages = 1;       // pages of the list on screen (1 = nothing to turn)
std::vector<std::pair<uint32_t, std::function<void()>>> g_native_buttons; // id -> action, for the window that is open

constexpr const char* kNBtn = "FF483E34";       // the grey-blue of the game's buttons
constexpr const char* kNBtnHover = "FF6B5B4A";
constexpr const char* kNBtnDown = "FF362E26";
constexpr const char* kNGold = "FF0D7FB2";      // the selected tab of the F1 screen
constexpr const char* kNGoldHover = "FF1A96CE";
constexpr const char* kNGoldDown = "FF0A6690";
constexpr const char* kNRow = "14FFFFFF";       // a quiet list row
constexpr const char* kNRowHover = "30FFFFFF";
constexpr const char* kNWhite = "@@clr_white@@";
constexpr const char* kNDim = "@@clr_txt_d@@";
constexpr const char* kNAmber = "@@clr_sel@@";
constexpr const char* kNFont = "/font/normal.font";
constexpr const char* kNFontSmall = "/font/small.font";
constexpr const char* kNFontBold = "/font/big_bold.font";

std::string SiiString(std::string s) {
  // the game's fonts have no arrow, ellipsis, middle dot or euro sign: they would show as "?"
  for (const auto& [from, to] : {std::pair<const char*, const char*>{"\xE2\x86\x92", "-"}, {"\xE2\x80\xA6", "..."}, {"\xC2\xB7", "-"}, {"\xE2\x82\xAC", "EUR"}})
    for (size_t at = 0; (at = s.find(from, at)) != std::string::npos; at += std::strlen(to)) s.replace(at, std::strlen(from), to);
  std::string out;
  for (const char ch : s) {
    if (ch == '"' || ch == '\\') out += '\\';
    if (ch != '<' && ch != '>') out += ch; // would be read as markup
  }
  return out;
}

// ---- the game's text markup, one layer at a time (layers are separated by <ret>) ----
std::string At(int x, int y) { return "<offset hshift=" + std::to_string(x) + " vshift=" + std::to_string(y) + ">"; }
std::string Styled(const std::string& text, const char* font, const char* color) {
  return std::string("<color value=") + color + "><font face=" + font + ">" + SiiString(text) + "</font>";
}
std::string Centered(const std::string& text, const char* font = kNFontBold, const char* color = kNWhite) {
  return "<align hstyle=center vstyle=center>" + Styled(text, font, color) + "</align>";
}
std::string LeftText(const std::string& text, int indent, const char* font = kNFont, const char* color = kNWhite) {
  return "<align vstyle=center>" + At(indent, 0) + Styled(text, font, color) + "</align>";
}
std::string RightText(const std::string& text, const char* font = kNFontSmall, const char* color = kNDim) {
  return "<align hstyle=right vstyle=center>" + Styled(text + "  ", font, color) + "</align>";
}
std::string Fill(const char* color) { return std::string("<img src=/material/ui/white.mat color=") + color + " xscale=stretch yscale=stretch>"; }
// The flag textures are 64x64 with the flag in the middle: the crop the game itself uses.
std::string Flag(const std::string& iso, int w, int h) {
  return iso.empty() ? std::string() : "<img src=/material/ui/flags/" + iso + ".mat left=p2 right=p62 top=p12 bottom=p52 width=" + std::to_string(w) + " height=" + std::to_string(h) + ">";
}
std::string CargoIcon(const std::string& cargo, int size, const char* color = "@@clr_cargo_logo@@") {
  const auto it = g_data.cargo_icon.find(cargo);
  return "<img src=/material/ui/cargo_logo/" + (it == g_data.cargo_icon.end() ? std::string("trailer_generic") : it->second) + ".mat width=" + std::to_string(size) +
         " height=" + std::to_string(size) + " color=" + color + ">";
}
std::string Layers(std::initializer_list<std::string> parts) {
  std::string out;
  for (const auto& p : parts) {
    if (p.empty()) continue;
    if (!out.empty()) out += "<ret>";
    out += p;
  }
  return out;
}

// The game keeps a script it has loaded: the same path (or the same unit names) again shows the old
// content. So every opening gets its own file and names; the previous file is deleted.
// The number must also be new after a hot reload of this DLL (the game's cache outlives it): it starts at the clock.
int g_native_serial = static_cast<int>(GetTickCount64() / 1000 % 1000000) * 100;
std::string NativeScriptName() { return "planner_" + std::to_string(g_native_serial) + ".sii"; }
std::string NativeScriptDir() {
  const char* home = std::getenv("USERPROFILE"); // ponytail: Documents in its default place; ask the shell if someone moved theirs
  return std::string(home ? home : "") + "\\Documents\\Euro Truck Simulator 2\\routeplanner\\";
}
std::string NativeScriptPath() { return NativeScriptDir() + NativeScriptName(); }

// One script being put together. Positions are x, y of the top-left corner, width and height in the
// UI's virtual 1440x900 (the script itself wants left/right/top/bottom with y growing upwards).
struct NativeUi {
  std::string unit, group, kids;
  std::vector<std::string> names;
  uint32_t next_id = 1000;

  void Node(const char* kind, const std::string& body, int x, int y, int w, int h, uint32_t id, int layer) {
    const std::string name = unit + ".n" + std::to_string(names.size());
    char tail[320];
    std::snprintf(tail, sizeof tail,
                  " coords_l: %d\n coords_r: %d\n coords_t: %d\n coords_b: %d\n area_l: 1\n area_r: 0\n area_t: 0\n area_b: 1\n id: %u\n layer: %d\n tab: -1\n pointer: -1\n", x,
                  x + w, y, y - h, id, layer);
    kids += std::string(kind) + " : " + name + " {\n" + body + tail + " my_parent: " + group + "\n}\n\n";
    names.push_back(name);
  }
  // anything drawn that is not a button: `markup` is the game's text markup
  void Draw(const std::string& markup, int x, int y, int w, int h, int layer = 5) { Node("ui::text", " text: \"" + markup + "\"\n", x, y, w, h, 0, layer); }
  void Label(const std::string& text, int x, int y, int w, int h = 28, const char* font = kNFont, const char* color = kNWhite) { Draw(LeftText(text, 0, font, color), x, y, w, h); }
  void Title(const std::string& text, int x, int y, int w, int h = 28, const char* font = kNFont, const char* color = kNWhite) { Draw(Centered(text, font, color), x, y, w, h); }
  // a card of the F1 screen: a slightly lighter block with a bold heading
  void Card(const std::string& heading, int x, int y, int w, int h) {
    Draw(Fill("16FFFFFF"), x, y, w, h, 2);
    Title(heading, x, y - 12, w, 30, kNFontBold);
  }
  // `content` goes over the button's own colour; `selected` makes it gold; `row` is the quiet look of a list line
  void Button(const std::string& content, int x, int y, int w, int h, std::function<void()> action, bool selected = false, bool row = false) {
    const char* n = selected ? kNGold : row ? kNRow : kNBtn;
    const char* s = selected ? kNGoldHover : row ? kNRowHover : kNBtnHover;
    const char* p = selected ? kNGoldDown : kNBtnDown;
    const std::string body = " n_pml: \"" + Fill(n) + "<ret>" + content + "\"\n s_pml: \"" + Fill(s) + "<ret>" + content + "\"\n s2_pml: \"" + Fill(s) + "<ret>" + content +
                             "\"\n d_pml: \"" + Fill(n) + "<ret>" + content + "\"\n p_pml: \"" + Fill(p) + "<ret>" + content + "\"\n button_type: normal\n";
    const uint32_t id = next_id++;
    Node("ui::button", body, x, y, w, h, id, 6);
    g_native_buttons.emplace_back(id, std::move(action));
  }
  void TextButton(const std::string& text, int x, int y, int w, std::function<void()> action, bool selected = false, int h = 32) {
    Button(Centered(text), x, y, w, h, std::move(action), selected);
  }
  // The game's own tick box (/material/ui/button/checkbox_1..4: off, off with the pointer over it, on, on
  // with the pointer over it) with its label; clicking anywhere on the line flips it.
  void Toggle(const std::string& text, int x, int y, int w, bool* value) {
    const auto face = [&](int picture) {
      return " \"" + Layers({At(2, 1) + "<img src=/material/ui/button/checkbox_" + std::to_string(picture) + ".mat left=p1 top=p1>", LeftText(text, 42)}) + "\"\n";
    };
    const int off = *value ? 3 : 1, over = *value ? 4 : 2;
    const std::string body = " n_pml:" + face(off) + " s_pml:" + face(over) + " s2_pml:" + face(over) + " d_pml:" + face(off) + " p_pml:" + face(over) + " button_type: normal\n";
    const uint32_t id = next_id++;
    Node("ui::button", body, x, y, w, 32, id, 6);
    g_native_buttons.emplace_back(id, [value] { *value = !*value; });
  }

  std::string Script() const {
    const std::string wnd = unit + ".wnd";
    std::string group_body = " fitting: false\n my_children: " + std::to_string(names.size()) + "\n";
    for (size_t i = 0; i < names.size(); ++i) group_body += " my_children[" + std::to_string(i) + "]: " + names[i] + "\n";
    const char* full = " coords_l: 0\n coords_r: 1440\n coords_t: 900\n coords_b: 0\n area_l: 0\n area_r: 1440\n area_t: 900\n area_b: 0\n";
    return "SiiNunit\n{\nui::window : " + wnd +
           " {\n window_handler: null\n clip_children: true\n keep_aspect: center\n user_string_data: \"\"\n first_direction_focus_id: 0\n fitting: false\n my_children: 1\n my_children[0]: " +
           group + "\n" + full + " id: 0\n layer: 0\n tab: -1\n pointer: -1\n my_parent: null\n}\n\nui::group : " + group + " {\n" + group_body + full +
           " id: 111\n layer: 0\n tab: -1\n pointer: -1\n my_parent: " + wnd + "\n}\n\n" + kids + "}\n";
  }
};

// ---- what the pages need from the planner ----
// The cargo options for the chosen cities, as the F8 planner computes them. Returns a note when there is nothing to list.
const char* NativeCargoOptions() {
  if (g_src.city.empty() || g_dst.city.empty()) return "Escolha a origem e o destino";
  const std::string key = g_src.city + "|" + g_dst.city + (g_any_cargo ? "|any" : "");
  if (key != g_options_for) {
    g_options = RouteOptions(g_data, g_src.city, g_dst.city, g_any_cargo);
    g_options_for = key;
    g_selected = -1;
    g_cargo_pending = g_supported && !g_options.empty();
  }
  if (g_cargo_pending) FilterUnknownCargo(); // asks the game which cargo it knows
  if (g_options.empty()) return g_any_cargo ? "Uma das cidades não tem empresas" : "Nenhuma carga liga essas cidades (marque \"Qualquer carga\")";
  return nullptr;
}

std::string Shorten(const std::string& s, size_t max) { // ponytail: counts bytes, an accent may cost one letter
  if (s.size() <= max) return s;
  size_t cut = max - 2;
  while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut; // not in the middle of a UTF-8 character
  return s.substr(0, cut) + "..";
}

// The initial of a name for the index, without its accent ("Óleo" -> 'O'); 0 for anything else.
char Initial(const std::string& s) {
  if (s.empty()) return 0;
  const unsigned char a = static_cast<unsigned char>(s[0]);
  if (a < 0x80) return std::isalpha(a) ? static_cast<char>(std::toupper(a)) : 0;
  if (a != 0xC3 || s.size() < 2) return 0;
  const unsigned char b = static_cast<unsigned char>(s[1]) & 0xDF; // upper case
  if (b >= 0x80 && b <= 0x85) return 'A';
  if (b == 0x87) return 'C';
  if (b >= 0x88 && b <= 0x8B) return 'E';
  if (b >= 0x8C && b <= 0x8F) return 'I';
  if (b >= 0x92 && b <= 0x96) return 'O';
  if (b >= 0x99 && b <= 0x9C) return 'U';
  return 0;
}

// Lower case without accents ("Óleo" -> "oleo"), to match what was typed.
std::string Fold(const std::string& s) {
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    const unsigned char a = static_cast<unsigned char>(s[i]);
    if (a < 0x80) {
      out += static_cast<char>(std::tolower(a));
    } else if (a == 0xC3 && i + 1 < s.size()) {
      const char base = Initial(s.substr(i, 2));
      if (base) out += static_cast<char>(std::tolower(base));
      ++i;
    }
  }
  return out;
}

std::string Tonnes(const std::string& cargo) {
  char mass[24];
  std::snprintf(mass, sizeof mass, "%.0f t", CargoMass(g_data, cargo) / 1000.0);
  return mass;
}
std::string Thousands(long long n) { // 12345 -> "12.345"
  std::string s = std::to_string(n);
  for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(i, ".");
  return s;
}
const std::string& CountryFlag(const std::string& country) {
  static const std::string none;
  const Named* c = Find(g_data.countries, country);
  return c ? c->parent : none;
}
const std::string& CityFlag(const std::string& city) {
  static const std::string none;
  const Named* c = Find(g_data.cities, city);
  return c ? CountryFlag(c->parent) : none;
}

void NativeGoTo(NativePage page) {
  if (page == NativePage::Cargo && g_native_page != NativePage::Cargo) g_native_filter.clear(); // a new visit starts with an empty box
  g_native_page = page;
  g_native_list_page = 0;
}

// "< Anterior   Página 2 de 5   Próxima >" on the bottom row, to the right
void NativePager(NativeUi& ui, int pages) {
  g_native_pages = pages;
  if (pages < 2) return;
  if (g_native_list_page > 0) ui.TextButton("Anterior", 930, 96, 130, [] { --g_native_list_page; });
  ui.Title("Página " + std::to_string(g_native_list_page + 1) + " de " + std::to_string(pages), 1065, 96, 150, 32);
  if (g_native_list_page < pages - 1) ui.TextButton("Próxima", 1220, 96, 130, [] { ++g_native_list_page; });
}

// ---- pages ----
constexpr int kCardTop = 760, kCardH = 452, kCardW = 410, kX1 = 90, kX2 = 515, kX3 = 940, kNPad = 22;

// A company line: "DE" / "PARA", its logo when the game has one, and its name.
void NativeCompany(NativeUi& ui, const char* label, const std::string& company, const std::string& name, int x, int y, int w) {
  ui.Label(label, x, y, 50, 30, kNFontSmall, kNAmber);
  const bool logo = g_data.logos.count(company) != 0;
  if (logo) ui.Draw(At(0, 0) + "<img src=/material/ui/company/small/" + company + ".mat width=116 height=29>", x + 52, y, 116, 30);
  ui.Label(Shorten(name, logo ? 22 : 34), x + (logo ? 178 : 52), y, w - (logo ? 178 : 52), 30);
}

// The place (flag, city, country) of one end of the route, as one big button that opens the place page.
void NativePlaceButton(NativeUi& ui, const char* label, Side& side, bool src, int x, int y, int w) {
  ui.Label(label, x, y, w, 24, kNFontSmall, kNAmber);
  const Named* country = Find(g_data.countries, side.country);
  const std::string city = CityLabel(side.city);
  const std::string content =
      city.empty() ? Layers({country ? At(14, 14) + Flag(country->parent, 45, 30) : std::string(), LeftText(country ? "Escolher a cidade" : "Escolher o local", country ? 74 : 18, kNFontBold)})
                   : Layers({At(14, 14) + Flag(CityFlag(side.city), 45, 30), At(74, 6) + Styled(Shorten(city, 24), kNFontBold, kNWhite),
                             At(74, 32) + Styled(country ? country->name : std::string(), kNFontSmall, kNDim)});
  ui.Button(content, x, y - 26, w, 58, [src] {
    g_native_src = src;
    NativeGoTo(NativePage::Place);
  });
}

void NativePlannerPage(NativeUi& ui, const SPF_JobData& jd) {
  const int top = kCardTop, h = kCardH, w = kCardW, bw = w - 2 * kNPad, bottom = top - h;
  // ---- route ----
  ui.Card("ROTA", kX1, top, w, h);
  NativePlaceButton(ui, "ORIGEM", g_src, true, kX1 + kNPad, top - 56, bw);
  ui.TextButton("Usar a cidade atual", kX1 + kNPad, top - 148, bw, [] { PickCurrentCity(); });
  NativePlaceButton(ui, "DESTINO", g_dst, false, kX1 + kNPad, top - 206, bw);
  ui.TextButton("Maior rota possível", kX1 + kNPad, top - 298, bw, [] { PickLongestRoute(); });
  if (!g_src.city.empty() && !g_dst.city.empty()) {
    const int out = static_cast<int>(std::count_if(g_data.branches.begin(), g_data.branches.end(), [](const Named& b) { return b.parent == g_src.city; }));
    const int in = static_cast<int>(std::count_if(g_data.branches.begin(), g_data.branches.end(), [](const Named& b) { return b.parent == g_dst.city; }));
    ui.Title(std::to_string(out) + " empresas na origem, " + std::to_string(in) + " no destino", kX1, top - 360, w, 26, kNFontSmall, kNDim);
  }
  // ---- cargo ----
  ui.Card("CARGA", kX2, top, w, h);
  const char* note = NativeCargoOptions();
  if (CargoPicked()) {
    const RouteOption o = g_options[g_selected];
    ui.Draw("<align hstyle=center vstyle=center>" + CargoIcon(o.cargo, 56) + "</align>", kX2, top - 50, w, 60);
    ui.Title(Shorten(CargoName(g_data, o.cargo), 34), kX2, top - 112, w, 30, kNFontBold, kNAmber);
    ui.Title("~" + Tonnes(o.cargo) + (o.off_market ? "  -  fora do mercado dessas empresas" : ""), kX2, top - 140, w, 24, kNFontSmall, kNDim);
    NativeCompany(ui, "DE", o.src_company, o.src_name, kX2 + kNPad, top - 178, bw);
    NativeCompany(ui, "PARA", o.dst_company, o.dst_name, kX2 + kNPad, top - 214, bw);
    const double km = g_supported ? game::FreightKm(Token(o.src_company.c_str()), Token(g_src.city.c_str()), Token(o.dst_company.c_str()), Token(g_dst.city.c_str())) : -1;
    if (km > 0) {
      ui.Label("Distância estimada", kX2 + kNPad, top - 258, 200, 26, kNFont, kNDim);
      ui.Draw(RightText(Thousands(static_cast<long long>(km)) + " km", kNFont, kNWhite), kX2 + kNPad, top - 258, bw, 26);
      ui.Label("Pagamento estimado", kX2 + kNPad, top - 284, 200, 26, kNFont, kNDim);
      ui.Draw(RightText("EUR " + Thousands(static_cast<long long>(600 + km * 15 * 0.9)), kNFont, kNAmber), kX2 + kNPad, top - 284, bw, 26);
    }
    int same = 0;
    for (const auto& other : g_options) same += other.cargo == o.cargo;
    if (same > 1)
      ui.TextButton("Mudar empresas (" + std::to_string(same) + ")", kX2 + kNPad, bottom + 88, bw, [cargo = o.cargo] {
        for (int n = 1; n <= static_cast<int>(g_options.size()); ++n) { // the next pair of companies carrying the same cargo
          const int i = (g_selected + n) % static_cast<int>(g_options.size());
          if (g_options[i].cargo == cargo) {
            g_selected = i;
            break;
          }
        }
      });
  } else {
    ui.Draw("<align hstyle=center vstyle=center>" + std::string("<img src=/material/ui/cargo_logo/trailer_generic.mat width=56 height=56 color=40FFFFFF>") + "</align>", kX2, top - 150, w, 60);
    ui.Title(note ? note : "Nenhuma carga escolhida", kX2, top - 226, w, 28, kNFont, kNDim);
  }
  // also with nothing to list: "Qualquer carga" lives on the cargo page
  if (!g_src.city.empty() && !g_dst.city.empty()) ui.TextButton(CargoPicked() ? "Trocar a carga" : "Escolher a carga", kX2 + kNPad, bottom + 50, bw, [] { NativeGoTo(NativePage::Cargo); });
  // ---- current job ----
  ui.Card("SERVIÇO ATUAL", kX3, top, w, h);
  if (!jd.on_job) {
    ui.Draw("<align hstyle=center vstyle=center><img src=/material/ui/cargo_logo/all.mat width=56 height=56 color=40FFFFFF></align>", kX3, top - 150, w, 60);
    ui.Title("Nenhuma entrega em andamento", kX3, top - 226, w, 28, kNFont, kNDim);
    g_confirm_cancel = false;
  } else {
    SPF_JobConstants jc{};
    g_core->telemetry->Tel_GetJobConstants(g_tel, &jc, sizeof jc);
    char line[256];
    ui.Draw("<align hstyle=center vstyle=center>" + CargoIcon(jc.cargo_id, 56) + "</align>", kX3, top - 50, w, 60);
    ui.Title(Shorten(jc.cargo_name, 34), kX3, top - 112, w, 30, kNFontBold, kNAmber);
    std::snprintf(line, sizeof line, "%.1f t", jc.cargo_mass / 1000.0f);
    ui.Title(line, kX3, top - 140, w, 24, kNFontSmall, kNDim);
    ui.Label("DE", kX3 + kNPad, top - 178, 50, 30, kNFontSmall, kNAmber);
    ui.Draw(Layers({At(0, 5) + Flag(CityFlag(jc.source_city_id), 30, 20), LeftText(Shorten(std::string(jc.source_city) + "  -  " + jc.source_company, 34), 40)}), kX3 + kNPad + 52, top - 178, bw - 52, 30);
    ui.Label("PARA", kX3 + kNPad, top - 214, 50, 30, kNFontSmall, kNAmber);
    ui.Draw(Layers({At(0, 5) + Flag(CityFlag(jc.destination_city_id), 30, 20), LeftText(Shorten(std::string(jc.destination_city) + "  -  " + jc.destination_company, 34), 40)}), kX3 + kNPad + 52, top - 214, bw - 52,
            30);
    ui.Label("Distância", kX3 + kNPad, top - 258, 200, 26, kNFont, kNDim);
    ui.Draw(RightText(Thousands(jc.planned_distance_km) + " km", kNFont, kNWhite), kX3 + kNPad, top - 258, bw, 26);
    ui.Label("Prazo", kX3 + kNPad, top - 284, 200, 26, kNFont, kNDim);
    std::snprintf(line, sizeof line, "%uh%02u", jd.remaining_delivery_minutes / 60, jd.remaining_delivery_minutes % 60);
    ui.Draw(RightText(line, kNFont, kNWhite), kX3 + kNPad, top - 284, bw, 26);
    ui.Label("Pagamento", kX3 + kNPad, top - 310, 200, 26, kNFont, kNDim);
    ui.Draw(RightText("EUR " + Thousands(static_cast<long long>(jc.income)), kNFont, kNAmber), kX3 + kNPad, top - 310, bw, 26);
    if (!g_confirm_cancel) {
      ui.TextButton("Ir até a carga (teleporte)", kX3 + kNPad, bottom + 88, bw, [] {
        g_pending = Pending::Teleport;
        g_native_leave = true;
      });
      ui.TextButton("Cancelar o serviço", kX3 + kNPad, bottom + 50, bw, [] { g_confirm_cancel = true; });
    } else {
      ui.Title("Cancelar mesmo? O jogo cobra multa.", kX3, bottom + 124, w, 26, kNFont, kNAmber);
      ui.TextButton("Sim, cancelar", kX3 + kNPad, bottom + 88, bw, [] {
        g_pending = Pending::Cancel;
        g_confirm_cancel = false;
        g_native_leave = true;
      });
      ui.TextButton("Não, manter", kX3 + kNPad, bottom + 50, bw, [] { g_confirm_cancel = false; });
    }
  }
  // ---- options, like the strip at the bottom of the F1 screen ----
  const int oy = bottom - 12;
  ui.Card("AO INICIAR O SERVIÇO", kX1, oy, 1260, 96);
  const int ow = 295, ox = kX1 + 22, step = 307;
  ui.Toggle("Teleportar até a origem", ox, oy - 50, ow, &g_teleport);
  ui.Toggle("Soltar o freio de mão", ox + step, oy - 50, ow, &g_release_brake);
  ui.Toggle("7h e tempo limpo", ox + 2 * step, oy - 50, ow, &g_morning);
  ui.Toggle("Abastecer o caminhão", ox + 3 * step, oy - 50, ow, &g_refuel);
  // ---- actions ----
  const bool editing = g_editing >= 0 && g_editing < static_cast<int>(g_favorites.size());
  const int ay = oy - 96 - 12;
  if (CargoPicked()) {
    ui.TextButton(editing ? "Salvar alterações na favorita" : "Salvar como favorita", kX1, ay - 5, 300, [editing] {
      if (editing) {
        g_favorites[g_editing] = SelectedRoute();
        SaveFavoritesFile();
        g_status = "Favorita atualizada.";
        g_status_error = false;
        g_editing = -1;
        NativeGoTo(NativePage::Favorites);
      } else {
        AddFavorite(SelectedRoute());
      }
    });
    if (CanStart(jd.on_job))
      ui.TextButton("INICIAR SERVIÇO", 520, ay, 400, [] {
        g_pending = Pending::Start;
        g_native_leave = true;
      }, true, 42);
  }
  // the planner back to empty, when there is anything in it
  if (!g_src.country.empty() || !g_src.city.empty() || !g_dst.country.empty() || !g_dst.city.empty() || CargoPicked() || editing)
    ui.TextButton("Limpar planejamento", kX3 + kCardW - 300, ay - 5, 300, [] {
      g_src = {};
      g_dst = {};
      g_options.clear();
      g_options_for.clear();
      g_selected = -1;
      g_cargo_pending = false;
      g_any_cargo = false;
      g_cargo_filter[0] = 0;
      g_native_letter = 0;
      g_native_filter.clear();
      g_editing = -1;
      g_status.clear();
    });
  if (!CargoPicked() || !CanStart(jd.on_job))
    ui.Title(jd.on_job ? "Cancele o serviço atual para iniciar outro" : "Escolha a origem, o destino e uma carga para iniciar", 420, ay - 6, 600, 30, kNFont, kNDim);
}

// One page for a place: every country on the left (with its flag), the cities of the chosen one on the right.
void NativePlacePage(NativeUi& ui) {
  Side& side = g_native_src ? g_src : g_dst;
  ui.Title(g_native_src ? "LOCAL DE ORIGEM" : "LOCAL DE DESTINO", 90, 772, 1260, 30, kNFontBold, kNAmber);
  const int top = 738, h = 600;
  ui.Card("PAÍS", kX1, top, 480, h);
  const int rows = 18, cw = 224;
  for (int i = 0; i < static_cast<int>(g_data.countries.size()) && i < 2 * rows; ++i) {
    const Named& c = g_data.countries[i];
    ui.Button(Layers({At(8, 5) + Flag(c.parent, 27, 18), LeftText(Shorten(c.name, 24), 44)}), kX1 + 12 + (i / rows) * (cw + 8), top - 46 - (i % rows) * 30, cw, 28, [&side, tok = c.tok] {
      side.country = tok;
      const Named* city = Find(g_data.cities, side.city);
      if (city && city->parent != tok) side.city.clear();
      g_native_list_page = 0;
    }, side.country == c.tok, true);
  }
  const Named* country = Find(g_data.countries, side.country);
  const int cx = kX1 + 495, cardw = 765;
  ui.Card(country ? "CIDADES  -  " + country->name : std::string("CIDADES"), cx, top, cardw, h);
  if (!country) {
    ui.Title("Escolha um país à esquerda", cx, top - 280, cardw, 30, kNFont, kNDim);
  } else {
    ui.Draw(At(0, 0) + Flag(country->parent, 45, 30), cx + 16, top - 8, 45, 30);
    std::vector<const Named*> cities;
    for (const auto& c : g_data.cities)
      if (c.parent == side.country) cities.push_back(&c);
    const int cols = 4, per_page = cols * rows, pages = std::max(1, (static_cast<int>(cities.size()) + per_page - 1) / per_page), w = 178;
    g_native_list_page = std::clamp(g_native_list_page, 0, pages - 1);
    for (int i = g_native_list_page * per_page, n = 0; i < static_cast<int>(cities.size()) && n < per_page; ++i, ++n) {
      const Named& c = *cities[i];
      const int companies = static_cast<int>(std::count_if(g_data.branches.begin(), g_data.branches.end(), [&](const Named& b) { return b.parent == c.tok; }));
      // filled column by column, so the alphabet reads downwards
      ui.Button(Layers({LeftText(Shorten(c.name, 17), 12), RightText(std::to_string(companies))}), cx + 14 + (n / rows) * (w + 8), top - 46 - (n % rows) * 30, w, 28, [&side, tok = c.tok] {
        side.city = tok;
        NativeGoTo(NativePage::Planner);
      }, side.city == c.tok, true);
    }
    ui.Title("O número ao lado de cada cidade é a quantidade de empresas", cx, top - h + 30, cardw, 24, kNFontSmall, kNDim);
    NativePager(ui, pages);
  }
  ui.TextButton("Voltar", kX1, 96, 160, [] { NativeGoTo(NativePage::Planner); });
  if (g_native_src)
    ui.TextButton("Usar a cidade atual", kX1 + 172, 96, 228, [] {
      PickCurrentCity();
      NativeGoTo(NativePage::Planner);
    });
}

// Cargo: on the left an index of initials, a box that filters by what is typed and the order; on the
// right the cargo (each once; the companies are the first pair that trades it, and the planner offers
// the other pairs). The box is drawn with the game's widgets; the typing comes from our keyboard hook
// (a window of ours has no handler class to own a real input line).
void NativeCargoPage(NativeUi& ui) {
  ui.Title("CARGA DE " + CityLabel(g_src.city) + " PARA " + CityLabel(g_dst.city), 90, 772, 1260, 30, kNFontBold, kNAmber);
  const int top = 738, h = 600, x = kX1 + 20, w = 210;
  ui.Card("ÍNDICE", kX1, top, 250, h);
  const char* note = NativeCargoOptions();
  std::vector<int> items; // one option per cargo, in the options' order (by name)
  std::set<char> letters;
  for (int i = 0; i < static_cast<int>(g_options.size()); ++i) {
    if (i > 0 && g_options[i].cargo == g_options[i - 1].cargo) continue;
    const std::string name = CargoName(g_data, g_options[i].cargo);
    const char initial = Initial(name);
    letters.insert(initial);
    if ((!g_native_letter || initial == g_native_letter) && (g_native_filter.empty() || Fold(name).find(g_native_filter) != std::string::npos)) items.push_back(i);
  }
  if (g_native_by_weight)
    std::stable_sort(items.begin(), items.end(), [](int a, int b) { return CargoMass(g_data, g_options[a].cargo) > CargoMass(g_data, g_options[b].cargo); });
  if (g_native_reverse) std::reverse(items.begin(), items.end());

  ui.TextButton("Todas", x, top - 50, w, [] {
    g_native_letter = 0;
    g_native_list_page = 0;
  }, g_native_letter == 0);
  for (char ch = 'A'; ch <= 'Z'; ++ch) {
    if (!letters.count(ch)) continue; // only the initials there is cargo for
    const int n = ch - 'A';
    ui.TextButton(std::string(1, ch), x + (n % 4) * 54, top - 92 - (n / 4) * 38, 48, [ch] {
      g_native_letter = ch;
      g_native_list_page = 0;
    }, g_native_letter == ch, 32);
  }
  // the filter box
  ui.Label("FILTRAR", x, top - 364, w, 22, kNFontSmall, kNAmber);
  ui.Draw(Layers({Fill("FF1E1A16"), g_native_filter.empty() ? LeftText("Digite para filtrar", 12, kNFont, kNDim) : LeftText(g_native_filter + "_", 12)}), x, top - 388, g_native_filter.empty() ? w : w - 40, 34);
  if (!g_native_filter.empty())
    ui.TextButton("x", x + w - 36, top - 388, 36, [] {
      g_native_filter.clear();
      g_native_list_page = 0;
    }, false, 34);
  // the order: clicking the one in use turns it around
  ui.Label("ORDENAR POR", x, top - 438, w, 22, kNFontSmall, kNAmber);
  const auto order = [](bool by_weight) {
    if (g_native_by_weight == by_weight) g_native_reverse = !g_native_reverse;
    else g_native_by_weight = by_weight, g_native_reverse = false;
    g_native_list_page = 0;
  };
  ui.TextButton("Nome", x, top - 462, 101, [order] { order(false); }, !g_native_by_weight);
  ui.TextButton("Peso", x + 109, top - 462, 101, [order] { order(true); }, g_native_by_weight);
  ui.Title(g_native_by_weight ? (g_native_reverse ? "mais leve primeiro" : "mais pesada primeiro") : (g_native_reverse ? "de Z a A" : "de A a Z"), x, top - 498, w, 22, kNFontSmall, kNDim);
  ui.Toggle("Qualquer carga", x, top - h + 54, w, &g_any_cargo);

  const int cx = kX1 + 265, cardw = 995;
  ui.Card(std::string("CARGAS") + (g_native_letter ? std::string("  -  ") + g_native_letter : std::string()), cx, top, cardw, h);
  if (note) {
    ui.Title(note, cx, top - 280, cardw, 30, kNFont, kNDim);
  } else if (items.empty()) {
    ui.Title("Nenhuma carga com esse filtro", cx, top - 280, cardw, 30, kNFont, kNDim);
  } else {
    const int cols = 3, rows = 17, per_page = cols * rows, pages = std::max(1, (static_cast<int>(items.size()) + per_page - 1) / per_page), bw = 318;
    g_native_list_page = std::clamp(g_native_list_page, 0, pages - 1);
    const std::string chosen = CargoPicked() ? g_options[g_selected].cargo : std::string();
    for (int i = g_native_list_page * per_page, n = 0; i < static_cast<int>(items.size()) && n < per_page; ++i, ++n) {
      const RouteOption& o = g_options[items[i]];
      ui.Button(Layers({At(8, 3) + CargoIcon(o.cargo, 22), LeftText(Shorten(CargoName(g_data, o.cargo), 28), 40, kNFont, o.off_market ? kNDim : kNWhite), RightText(Tonnes(o.cargo))}),
                cx + 12 + (n / rows) * (bw + 8), top - 46 - (n % rows) * 31, bw, 28, [option = items[i]] {
                  g_selected = option;
                  NativeGoTo(NativePage::Planner);
                }, o.cargo == chosen, true);
    }
    ui.Title(g_any_cargo ? "Em cinza: cargas que essas empresas não negociam normalmente" : "Só as cargas que as empresas dessas cidades negociam", cx, top - h + 30, cardw, 24, kNFontSmall, kNDim);
    NativePager(ui, pages);
  }
  ui.TextButton("Voltar", kX1, 96, 160, [] { NativeGoTo(NativePage::Planner); });
}

void NativeFavoritesPage(NativeUi& ui, const SPF_JobData& jd) {
  const int top = kCardTop, h = 610, w = 1260;
  ui.Card("ROTAS FAVORITAS", kX1, top, w, h);
  const int rows = 8, count = static_cast<int>(g_favorites.size()), pages = std::max(1, (count + rows - 1) / rows);
  g_native_list_page = std::clamp(g_native_list_page, 0, pages - 1);
  if (g_favorites.empty()) ui.Title("Nenhuma favorita ainda. Em Planejar, escolha a rota e a carga e use \"Salvar como favorita\".", kX1, top - 280, w, 30, kNFont, kNDim);
  const bool can = CanStart(jd.on_job);
  for (int i = g_native_list_page * rows, n = 0; i < count && n < rows; ++i, ++n) {
    const Favorite& f = g_favorites[i];
    const int y = top - 52 - n * 66, x = kX1 + 16;
    ui.Draw(Fill(kNRow), x, y, w - 32, 58, 3);
    ui.Draw(Layers({At(0, 5) + Flag(CityFlag(f.src_city), 30, 20), LeftText(Shorten(CityLabel(f.src_city), 22), 40, kNFontBold)}), x + 16, y - 4, 250, 30);
    ui.Label("para", x + 16, y - 30, 40, 24, kNFontSmall, kNDim);
    ui.Draw(Layers({At(0, 2) + Flag(CityFlag(f.dst_city), 30, 20), LeftText(Shorten(CityLabel(f.dst_city), 24), 40)}), x + 56, y - 30, 250, 24);
    ui.Draw(Layers({At(0, 2) + CargoIcon(f.cargo, 26), LeftText(Shorten(CargoName(g_data, f.cargo), 30), 36, kNFont, kNAmber)}), x + 330, y - 4, 330, 30);
    ui.Label("~" + Tonnes(f.cargo) + "   " + Shorten(CompanyLabel(f.src_company), 16) + " para " + Shorten(CompanyLabel(f.dst_company), 16), x + 366, y - 30, 440, 24, kNFontSmall, kNDim);
    if (can)
      ui.TextButton("Iniciar", x + 830, y - 13, 130, [i] {
        if (ApplyRoute(g_favorites[i])) {
          g_pending = Pending::Start; // same path as the planner's button
          g_native_leave = true;
        } else {
          g_status = "Essa favorita não existe mais nos dados do jogo (cidade, empresa ou carga).";
          g_status_error = true;
        }
      });
    ui.TextButton("Editar", x + 970, y - 13, 115, [i] {
      ApplyRoute(g_favorites[i]);
      g_editing = i;
      NativeGoTo(NativePage::Planner);
    });
    ui.TextButton("Remover", x + 1095, y - 13, 115, [i] {
      g_favorites.erase(g_favorites.begin() + i);
      SaveFavoritesFile();
      g_editing = -1;
      g_status = "Favorita removida.";
      g_status_error = false;
    });
  }
  if (jd.on_job)
    ui.TextButton("Salvar o serviço atual", kX1, 96, 260, [] {
      SPF_JobConstants jc{};
      g_core->telemetry->Tel_GetJobConstants(g_tel, &jc, sizeof jc);
      AddFavorite({jc.source_city_id, jc.destination_city_id, jc.cargo_id, jc.source_company_id, jc.destination_company_id});
    });
  if (CargoPicked()) ui.TextButton("Salvar a rota do planejador", kX1 + (jd.on_job ? 272 : 0), 96, 290, [] { AddFavorite(SelectedRoute()); });
  NativePager(ui, pages);
}

// ---- the mouse wheel and the keyboard ----
// Neither the game's UI (no handler class) nor SPF's ImGui (its wheel does not arrive while SPF's own
// windows are closed) tells us about the wheel, so while the screen is open a low-level mouse hook
// counts the notches, and on the cargo page a keyboard hook takes what is typed for the filter box
// (letters, digits, space, backspace: those keys are kept from the game meanwhile). Such hooks are
// called on the thread that installed them and need that thread to pump messages, hence a thread of
// ours: it must be gone before this DLL is unloaded (WheelStop).
std::atomic<bool> g_typing{false}; // the cargo page is on screen
std::mutex g_typed_mu;
std::string g_typed; // keys not yet used: lower-case letters, digits, ' ', '\b'

LRESULT CALLBACK KeyProc(int code, WPARAM what, LPARAM data) {
  if (code == HC_ACTION && g_typing && (what == WM_KEYDOWN || what == WM_KEYUP)) {
    const DWORD vk = reinterpret_cast<const KBDLLHOOKSTRUCT*>(data)->vkCode;
    const char ch = vk >= 'A' && vk <= 'Z' ? static_cast<char>(vk + 32) : (vk >= '0' && vk <= '9') || vk == VK_SPACE ? static_cast<char>(vk) : vk == VK_BACK ? '\b' : 0;
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    if (ch && pid == GetCurrentProcessId()) {
      if (what == WM_KEYDOWN) {
        const std::lock_guard lock(g_typed_mu);
        g_typed += ch;
      }
      return 1; // ours: the game does not see it
    }
  }
  return CallNextHookEx(nullptr, code, what, data);
}
std::atomic<int> g_wheel{0};       // notches not yet used: up > 0
std::atomic<DWORD> g_wheel_tid{0}; // the hook thread, once it runs
std::thread g_wheel_thread;

LRESULT CALLBACK WheelProc(int code, WPARAM what, LPARAM data) {
  if (code == HC_ACTION && what == WM_MOUSEWHEEL)
    g_wheel += static_cast<short>(HIWORD(reinterpret_cast<const MSLLHOOKSTRUCT*>(data)->mouseData)) > 0 ? 1 : -1;
  return CallNextHookEx(nullptr, code, what, data);
}
void WheelStart() {
  if (g_wheel_thread.joinable()) return;
  g_wheel = 0;
  g_wheel_tid = 0;
  g_wheel_thread = std::thread([] {
    MSG msg;
    PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE); // makes this thread's message queue, so WM_QUIT can be posted to it
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&WheelProc), &self);
    const HHOOK hook = SetWindowsHookExW(WH_MOUSE_LL, WheelProc, self, 0);
    const HHOOK keys = SetWindowsHookExW(WH_KEYBOARD_LL, KeyProc, self, 0);
    g_wheel_tid = GetCurrentThreadId();
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    }
    if (hook) UnhookWindowsHookEx(hook);
    if (keys) UnhookWindowsHookEx(keys);
  });
}
void WheelStop() {
  if (!g_wheel_thread.joinable()) return;
  while (!g_wheel_tid) Sleep(1); // it starts within a millisecond or two
  PostThreadMessageW(g_wheel_tid, WM_QUIT, 0, 0);
  g_wheel_thread.join();
}

// Writes the script of the current page. False if the file could not be written.
bool WriteNativeScript() {
  DeleteFileA(NativeScriptPath().c_str());
  ++g_native_serial;
  g_native_buttons.clear();
  g_native_pages = 1;
  NativeUi ui;
  ui.unit = "_nameless.rpl" + std::to_string(g_native_serial); // every name of this opening starts with it
  ui.group = ui.unit + ".grp";
  SPF_JobData jd{};
  if (g_tel) g_core->telemetry->Tel_GetJobData(g_tel, &jd, sizeof jd);
  // the frame every page shares, as in the game's F1 screen
  ui.Node("ui::text_common", " value: \"@@clr_bg_main@@\"\n look_template: txt.window.bcg_rect4\n text: \"\"\n", 40, 860, 1360, 820, 0, 1);
  ui.Node("ui::text_common", " value: \"@@ui_paused@@\"\n look_template: txt.big.left\n text: \"\"\n", 60, 850, 300, 30, 0, 5);
  ui.Node("ui::text_common", " value: \"PLANEJADOR DE ROTAS\"\n look_template: txt.big.center\n text: \"\"\n", 420, 850, 600, 30, 0, 5);
  const bool sub_page = g_native_page == NativePage::Place || g_native_page == NativePage::Cargo;
  if (!sub_page) {
    const bool planner = g_native_page == NativePage::Planner;
    ui.TextButton("Planejar", 495, 816, 220, [] { NativeGoTo(NativePage::Planner); }, planner, 42);
    ui.TextButton("Favoritas (" + std::to_string(g_favorites.size()) + ")", 725, 816, 220, [] { NativeGoTo(NativePage::Favorites); }, !planner, 42);
  }
  switch (g_native_page) {
    case NativePage::Planner: NativePlannerPage(ui, jd); break;
    case NativePage::Favorites: NativeFavoritesPage(ui, jd); break;
    case NativePage::Place: NativePlacePage(ui); break;
    case NativePage::Cargo: NativeCargoPage(ui); break;
  }
  if (!sub_page) {
    if (!g_status.empty()) ui.Title(Shorten(g_status, 130), 90, 132, 1260, 24, kNFontSmall, g_status_error ? kNAmber : kNDim);
    ui.TextButton("Retomar", 620, 96, 200, [] { g_native_leave = true; });
  }
  CreateDirectoryA(NativeScriptDir().c_str(), nullptr);
  const std::string script = ui.Script();
  FILE* f = nullptr;
  if (fopen_s(&f, NativeScriptPath().c_str(), "wb") != 0 || !f) return false;
  const bool ok = std::fwrite(script.data(), 1, script.size(), f) == script.size();
  std::fclose(f);
  return ok;
}

// Shows the current page (again). The game stays paused while the window is swapped.
bool ShowNative() {
  // The new window goes up before the old one comes down (four frames later, in NativeExperiment): closing
  // first left a frame with nothing on screen, a blink at every click. Each window has its own name,
  // because showing a window takes down any other with the same name.
  void* const old = g_native_window;
  g_native_window = nullptr;
  int why = -1;
  if (!WriteNativeScript()) Log("janela do jogo: não consegui gravar " + NativeScriptPath());
  else if ((why = game::OpenGameWindow(&g_native_window, ("rpl" + std::to_string(g_native_serial)).c_str(), ("/home/routeplanner/" + NativeScriptName()).c_str())) != 0)
    Log("janela do jogo: não abriu (motivo " + std::to_string(why) + ")");
  g_typing = why == 0 && g_native_page == NativePage::Cargo;
  if (why != 0) {
    g_native_window = old; // nothing new: the caller closes what there is
    return false;
  }
  if (g_native_old) game::CloseGameWindow(&g_native_old); // two rebuilds in a row
  g_native_old = old;
  g_native_old_in = 4;
  return true;
}

void CloseNative() {
  g_typing = false;
  if (g_native_old) game::CloseGameWindow(&g_native_old);
  const bool closed = !g_native_window || game::CloseGameWindow(&g_native_window);
  const bool resumed = g_native_paused && game::PauseForUi(false);
  Log(std::string("janela do jogo: ") + (closed ? "fechada" : "falha ao fechar") + (g_native_paused ? (resumed ? ", jogo retomado" : ", FALHA ao retomar o jogo") : ""));
  g_native_paused = false;
  g_native_buttons.clear();
  DeleteFileA(NativeScriptPath().c_str());
  WheelStop();
}

void NativeExperiment() {
  static bool was_down = false, esc_was_down = false;
  if (g_native_old && --g_native_old_in <= 0) game::CloseGameWindow(&g_native_old);
  if (g_native_window) {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    const bool ours = pid == GetCurrentProcessId();
    const bool down = ours && (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0, esc = ours && (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
    const bool released = was_down && !down, esc_pressed = esc && !esc_was_down;
    was_down = down, esc_was_down = esc;
    const int wheel = ours ? g_wheel.exchange(0) : (g_wheel = 0, 0); // down = next page
    const int turned = std::clamp(g_native_list_page + (wheel < 0 ? 1 : wheel > 0 ? -1 : 0), 0, std::max(g_native_pages, 1) - 1);
    std::string typed;
    if (g_typing) {
      const std::lock_guard lock(g_typed_mu);
      typed.swap(g_typed);
    }
    if (esc_pressed) {
      CloseNative();
    } else if (!typed.empty()) {
      for (const char ch : typed) {
        if (ch == '\b') {
          if (!g_native_filter.empty()) g_native_filter.pop_back();
        } else if (g_native_filter.size() < 20) {
          g_native_filter += ch;
        }
      }
      g_native_list_page = 0;
      if (!ShowNative()) CloseNative();
    } else if (turned != g_native_list_page) {
      g_native_list_page = turned;
      if (!ShowNative()) CloseNative();
    } else if (released) {
      std::function<void()> action;
      for (const auto& [id, act] : g_native_buttons)
        if (game::WidgetFlags(g_native_window, id) & (1u << 24)) action = act; // copied: running it replaces the list
      if (action) {
        g_native_leave = false;
        action();
        if (g_native_leave || !ShowNative()) CloseNative();
      }
    }
  } else {
    was_down = esc_was_down = false;
  }
  if (!g_native_toggle.exchange(false)) return;
  if (g_native_window) {
    CloseNative();
    return;
  }
  g_native_paused = game::PauseForUi(true);
  if (!g_native_paused) Log("janela do jogo: NÃO consegui pausar o jogo (sem cursor)");
  if (g_native_page != NativePage::Favorites) NativeGoTo(NativePage::Planner);
  if (!ShowNative()) {
    CloseNative();
  } else {
    WheelStart();
    Log("janela do jogo: aberta");
  }
}

void Key(int key) {
  if (key == CORE_KEY_PLANNER) g_toggle = true;
  else if (key == CORE_KEY_NATIVE) g_native_toggle = true;
}

void Update() {
  if (!g_core || !g_core->ui || !g_window) return;
  SPF_UI_API* ui = g_core->ui;
  if (g_toggle.exchange(false)) ui->UI_SetVisibility(g_window, !ui->UI_IsVisible(g_window));
  const bool open = ui->UI_IsVisible(g_window); // Esc in SPF can also close it; the mouse is ours while it is open
  if (open != g_mouse_taken) {
    g_api.SetMouseBlocked(open);
    ui->UI_SetMouseOverride(open);
    g_mouse_taken = open;
  }
  std::lock_guard lock(g_mu);
  NativeExperiment();
  if (g_cargo_pending) FilterUnknownCargo();
  if (g_pending != Pending::None) RunPending();
  if (g_start_in >= 0 && g_start_in-- == 0) {
    g_pending = Pending::Create;
    RunPending();
  }
  if (g_teleport_in >= 0 && g_teleport_in-- == 0) Teleport();
  if (g_hint_in >= 0 && g_hint_in-- == 0) {
    static int tries = 0, last = -1;
    const int why = game::ShowHint(g_hint_text.c_str());
    if (why != last) Log("caixa de aviso do jogo: " + (why == 0 ? std::string("mensagem enviada") : "ainda não (motivo " + std::to_string(why) + ")"));
    last = why;
    if (why == 0) g_hint_off_in = 600, tries = 0, last = -1; // ~10 s
    else if (++tries < 40) g_hint_in = 30;                   // the teleport fades the screen: keep trying for ~20 s
    else tries = 0, last = -1;
  }
  if (g_hint_off_in >= 0 && g_hint_off_in-- == 0)
    Log(std::string("caixa de aviso do jogo: ") + (game::HideHint() ? "mensagem retirada" : "falha ao retirar"));
  if (g_tp_check_in >= 0 && g_tp_check_in-- == 0) {
    Log("1 s depois do teleporte: caminhão em " + TruckPos());
    if (g_release_brake) ReleaseBrake("1 s depois"); // the game may engage it again once the truck settles
  }
}

void DrawPlanner(SPF_UI_API* ui) { Draw(ui, nullptr); }

// The DLL is going away: give the mouse back. Nothing here calls into the game.
void Shutdown(bool game_calls_ok) {
  std::lock_guard lock(g_mu);
  WheelStop(); // always: its thread runs code of this DLL
  if (game_calls_ok && g_native_window) CloseNative(); // on a framework unload it stays open (and paused): no game calls there
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
  g_loaded = LoadRoutes(PluginDir() + "routes.tsv", g_data);
  g_favorites = LoadFavorites(PluginDir() + "favorites.tsv");
  g_supported = game::Supported();
  Log("núcleo #" + std::to_string(api->reloads) + " (" __DATE__ " " __TIME__ "): " + std::to_string(g_data.cities.size()) + " cidades, " +
      std::to_string(g_data.cargo_names.size()) + " cargas; jogo " + (g_supported ? "reconhecido" : "NÃO reconhecido") + ". F8 abre.");
  *out = {Update, DrawPlanner, Key, Shutdown};
  return true;
}
