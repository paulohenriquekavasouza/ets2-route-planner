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
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <map>
#include <set>
#include <string>

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
// laid out like the game's F1 screen: a full panel, a title row, tabs, cards with a bold heading and
// the game's own buttons. A window of ours has no handler class, so:
//   - content changes by writing a new script and opening it again (the game stays paused meanwhile);
//   - a click is the left mouse button released while one of our buttons has the pointer over it
//     (bit 24 of the widget's flags);
//   - there is no text box or scrolling list: long choices (countries, cities, cargo) are pages of buttons.
// The key comes from the host (SPF swallows Home before Windows' key state sees it).
// ponytail: the game keeps every script it has loaded, so each rebuild leaves a few KB behind until the
// game closes. If that ever matters: change widgets in place (needs the game's "set text" call).
// =================================================================================================
void* g_native_window = nullptr;
bool g_native_paused = false; // we paused the game for the window (its cursor only exists while paused)
std::atomic<bool> g_native_toggle{false};

enum class NativePage { Planner, Favorites, Country, City, Cargo };
NativePage g_native_page = NativePage::Planner;
bool g_native_src = true;   // which side the country / city pages fill
int g_native_list_page = 0; // page of a paged list
bool g_native_leave = false; // the action just run needs the game running: close instead of rebuilding
std::vector<std::pair<uint32_t, std::function<void()>>> g_native_buttons; // id -> action, for the window that is open

std::string SiiString(const std::string& s) {
  std::string out;
  for (const char ch : s) {
    if (ch == '"' || ch == '\\') out += '\\';
    out += ch;
  }
  return out;
}

// The game keeps a script it has loaded: the same path (or the same unit names) again shows the old
// content. So every opening gets its own file and names; the previous file is deleted.
int g_native_serial = 0;
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

  std::string Node(const char* kind, const std::string& body, int x, int y, int w, int h, uint32_t id, int layer) {
    const std::string name = unit + ".n" + std::to_string(names.size());
    char tail[320];
    std::snprintf(tail, sizeof tail,
                  " coords_l: %d\n coords_r: %d\n coords_t: %d\n coords_b: %d\n area_l: 1\n area_r: 0\n area_t: 0\n area_b: 1\n id: %u\n layer: %d\n tab: -1\n pointer: -1\n", x,
                  x + w, y, y - h, id, layer);
    kids += std::string(kind) + " : " + name + " {\n" + body + tail + " my_parent: " + group + "\n}\n\n";
    names.push_back(name);
    return name;
  }
  // look = one of the game's text templates (txt.normal.left, txt.emph.left, txt.big.center, ...)
  void Text(const char* look, const std::string& text, int x, int y, int w, int h = 28, int layer = 5) {
    Node("ui::text_common", " value: \"" + SiiString(text) + "\"\n look_template: " + look + "\n text: \"\"\n", x, y, w, h, 0, layer);
  }
  // a flat coloured block (alpha first), like the cards of the F1 screen
  void Block(const char* color, int x, int y, int w, int h, int layer = 2) {
    Node("ui::text_common", std::string(" value: ") + color + "\n look_template: txt.background.flat\n text: \"\"\n", x, y, w, h, 0, layer);
  }
  void Card(const std::string& heading, int x, int y, int w, int h) {
    Block("18FFFFFF", x, y, w, h);
    Text("txt.big.bold.white.center", heading, x, y - 10, w, 30);
  }
  // The game's normal button (30 high), or its tab button tinted with the selection colour (42 high)
  // for the main action, the selected tab and options that are on.
  void Button(const std::string& text, int x, int y, int w, std::function<void()> action, bool accent = false) {
    std::string looks = " n_pml: \"\"\n s_pml: \"\"\n s2_pml: \"\"\n d_pml: \"\"\n p_pml: \"\"\n";
    if (accent) {
      const std::string face =
          "<img src=/material/ui/button/btn_tab.mat right=p4><img src=/material/ui/button/btn_tab.mat width=-4 left=p4 right=p4><img src=/material/ui/button/btn_tab.mat left=p4 "
          "right=p0><ret><align vstyle=center hstyle=center><font face=/font/big_bold.font><color value=@@clr_white@@>" +
          SiiString(text) + "</font></align>";
      const std::string on = "<color value=@@clr_sel@@>" + face;
      looks = " n_pml: \"" + on + "\"\n s_pml: \"" + on + "\"\n s2_pml: \"\"\n d_pml: \"" + on + "\"\n p_pml: \"" + on + "\"\n";
    }
    const uint32_t id = next_id++;
    Node("ui::button_common",
         " value: \"" + SiiString(text) + "\"\n value2: \"\"\n look_template: " + (accent ? "btn.tab" : "btn.normal") + "\n" + looks + " button_type: normal\n", x, y, w,
         accent ? 42 : 30, id, 6);
    g_native_buttons.emplace_back(id, std::move(action));
  }
  // an option that is on or off: accent look when on
  void Toggle(const std::string& text, int x, int y, int w, bool* value) {
    Button(text, x, *value ? y + 6 : y, w, [value] { *value = !*value; }, *value);
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
  if (g_src.city.empty() || g_dst.city.empty()) return "Escolha origem e destino.";
  const std::string key = g_src.city + "|" + g_dst.city + (g_any_cargo ? "|any" : "");
  if (key != g_options_for) {
    g_options = RouteOptions(g_data, g_src.city, g_dst.city, g_any_cargo);
    g_options_for = key;
    g_selected = -1;
    g_cargo_pending = g_supported && !g_options.empty();
  }
  if (g_cargo_pending) FilterUnknownCargo(); // asks the game which cargo it knows
  if (g_options.empty()) return g_any_cargo ? "Uma das cidades não tem empresas." : "Nenhuma carga liga empresas dessas cidades. Ligue \"Qualquer carga\".";
  return nullptr;
}

std::string Shorten(const std::string& s, size_t max) { // ponytail: counts bytes, an accent may cost one letter
  if (s.size() <= max) return s;
  size_t cut = max - 2;
  while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut; // not in the middle of a UTF-8 character
  return s.substr(0, cut) + "..";
}

// Buttons for a paged grid: `count` items, `cols` x `rows` per page, each made by `item(index, x, y, w)`.
void NativeGrid(NativeUi& ui, int count, int cols, int rows, int row_h, const std::function<void(int, int, int, int)>& item) {
  const int per_page = cols * rows, pages = std::max(1, (count + per_page - 1) / per_page);
  g_native_list_page = std::clamp(g_native_list_page, 0, pages - 1);
  const int left = 110, width = 1220, gap = 8, w = (width - gap * (cols - 1)) / cols, top = 730;
  for (int i = g_native_list_page * per_page, n = 0; i < count && n < per_page; ++i, ++n) item(i, left + (n % cols) * (w + gap), top - (n / cols) * row_h, w);
  if (pages > 1) {
    if (g_native_list_page > 0) ui.Button("< Anterior", 430, 150, 180, [] { --g_native_list_page; });
    ui.Text("txt.normal.center", "Página " + std::to_string(g_native_list_page + 1) + " de " + std::to_string(pages), 620, 150, 200, 30);
    if (g_native_list_page < pages - 1) ui.Button("Próxima >", 830, 150, 180, [] { ++g_native_list_page; });
  }
}

void NativeGoTo(NativePage page) {
  g_native_page = page;
  g_native_list_page = 0;
}

// ---- pages ----
void NativePlannerPage(NativeUi& ui, const SPF_JobData& jd) {
  const int top = 750, h = 400, w = 400, x1 = 110, x2 = 520, x3 = 930, pad = 30, bw = w - 2 * pad;
  // current job
  ui.Card("SERVIÇO ATUAL", x1, top, w, h);
  if (!jd.on_job) {
    ui.Text("txt.normal.center", "Nenhuma entrega em andamento", x1, top - 190, w);
    g_confirm_cancel = false;
  } else {
    SPF_JobConstants jc{};
    g_core->telemetry->Tel_GetJobConstants(g_tel, &jc, sizeof jc);
    char line[256];
    std::snprintf(line, sizeof line, "%s  (%.1f t)", jc.cargo_name, jc.cargo_mass / 1000.0f);
    ui.Text("txt.emph.left", Shorten(line, 44), x1 + pad, top - 60, bw);
    ui.Text("txt.normal.left", Shorten(std::string("De: ") + jc.source_company + ", " + jc.source_city, 44), x1 + pad, top - 92, bw);
    ui.Text("txt.normal.left", Shorten(std::string("Para: ") + jc.destination_company + ", " + jc.destination_city, 44), x1 + pad, top - 120, bw);
    std::snprintf(line, sizeof line, "%u km  -  prazo em %uh%02u", jc.planned_distance_km, jd.remaining_delivery_minutes / 60, jd.remaining_delivery_minutes % 60);
    ui.Text("txt.normal.left", line, x1 + pad, top - 148, bw);
    if (!g_confirm_cancel) {
      ui.Button("Ir até a carga (teleporte)", x1 + pad, top - h + 100, bw, [] {
        g_pending = Pending::Teleport;
        g_native_leave = true;
      });
      ui.Button("Cancelar serviço", x1 + pad, top - h + 55, bw, [] { g_confirm_cancel = true; });
    } else {
      ui.Text("txt.emph.left", "Cancelar mesmo? O jogo cobra multa.", x1 + pad, top - h + 140, bw);
      ui.Button("Sim, cancelar", x1 + pad, top - h + 100, bw, [] {
        g_pending = Pending::Cancel;
        g_confirm_cancel = false;
        g_native_leave = true;
      });
      ui.Button("Não, manter", x1 + pad, top - h + 55, bw, [] { g_confirm_cancel = false; });
    }
  }
  // route
  ui.Card("ROTA", x2, top, w, h);
  const auto side = [&](const char* title, Side& s, bool src, int y) {
    const Named* country = Find(g_data.countries, s.country);
    const std::string city = CityLabel(s.city);
    ui.Text("txt.emph.left", title, x2 + pad, y, bw);
    ui.Button(Shorten("País: " + (country ? country->name : std::string("todos")), 30), x2 + pad, y - 32, bw, [src] {
      g_native_src = src;
      NativeGoTo(NativePage::Country);
    });
    ui.Button(Shorten("Cidade: " + (city.empty() ? std::string("escolher") : city), 30), x2 + pad, y - 70, bw, [src] {
      g_native_src = src;
      NativeGoTo(NativePage::City);
    });
  };
  side("ORIGEM", g_src, true, top - 55);
  ui.Button("Usar a cidade atual", x2 + pad, top - 163, bw, [] { PickCurrentCity(); });
  side("DESTINO", g_dst, false, top - 215);
  ui.Button("Maior rota possível", x2 + pad, top - 323, bw, [] { PickLongestRoute(); });
  // cargo
  ui.Card("CARGA", x3, top, w, h);
  const char* note = NativeCargoOptions();
  if (CargoPicked()) {
    const RouteOption& o = g_options[g_selected];
    char mass[32];
    std::snprintf(mass, sizeof mass, "~%.0f t", CargoMass(g_data, o.cargo) / 1000.0);
    ui.Text("txt.emph.left", Shorten(CargoName(g_data, o.cargo), 30) + "  (" + mass + ")", x3 + pad, top - 60, bw);
    ui.Text("txt.normal.left", Shorten("De: " + o.src_name, 44), x3 + pad, top - 92, bw);
    ui.Text("txt.normal.left", Shorten("Para: " + o.dst_name, 44), x3 + pad, top - 120, bw);
    if (o.off_market) ui.Text("txt.normal.left", "Fora do mercado dessas empresas", x3 + pad, top - 148, bw);
  } else {
    ui.Text("txt.normal.center", note ? "Sem cargas para listar" : "Nenhuma carga escolhida", x3, top - 100, w);
  }
  if (note) ui.Text("txt.normal.center", Shorten(note, 52), x3, top - 190, w);
  else ui.Button("Escolher carga (" + std::to_string(g_options.size()) + ")", x3 + pad, top - h + 145, bw, [] { NativeGoTo(NativePage::Cargo); });
  ui.Toggle("Qualquer carga", x3 + pad, top - h + 61, bw, &g_any_cargo);
  // options
  const int oy = top - h - 10;
  ui.Card("AO INICIAR O SERVIÇO", x1, oy, 1220, 110);
  const int ow = 290, ox = x1 + 15;
  ui.Toggle("Teleportar até a origem", ox, oy - 56, ow, &g_teleport);
  ui.Toggle("Soltar o freio de mão", ox + 300, oy - 56, ow, &g_release_brake);
  ui.Toggle("7h e tempo limpo", ox + 600, oy - 56, ow, &g_morning);
  ui.Toggle("Abastecer", ox + 900, oy - 56, ow, &g_refuel);
  // actions
  const bool editing = g_editing >= 0 && g_editing < static_cast<int>(g_favorites.size());
  if (CargoPicked()) {
    ui.Button(editing ? "Salvar alterações na favorita" : "Salvar como favorita", x1, 176, 330, [editing] {
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
      ui.Button("INICIAR SERVIÇO", 520, 182, 400, [] {
        g_pending = Pending::Start;
        g_native_leave = true;
      }, true);
  }
  if (!CargoPicked() || !CanStart(jd.on_job))
    ui.Text("txt.normal.center", jd.on_job ? "Cancele o serviço atual para iniciar outro." : "Escolha origem, destino e uma carga para iniciar.", 470, 176, 500, 30);
}

void NativeFavoritesPage(NativeUi& ui, const SPF_JobData& jd) {
  const int x = 110, w = 1220;
  ui.Card("ROTAS FAVORITAS", x, 750, w, 560);
  if (jd.on_job)
    ui.Button("Salvar o serviço atual", x + 30, 700, 300, [] {
      SPF_JobConstants jc{};
      g_core->telemetry->Tel_GetJobConstants(g_tel, &jc, sizeof jc);
      AddFavorite({jc.source_city_id, jc.destination_city_id, jc.cargo_id, jc.source_company_id, jc.destination_company_id});
    });
  if (CargoPicked()) ui.Button("Salvar a rota do planejador", x + 340, 700, 330, [] { AddFavorite(SelectedRoute()); });
  if (g_favorites.empty()) {
    ui.Text("txt.normal.center", "Nenhuma favorita ainda. Em Planejar, escolha a rota e a carga e salve.", x, 480, w);
    return;
  }
  const int rows = 10, count = static_cast<int>(g_favorites.size()), pages = (count + rows - 1) / rows;
  g_native_list_page = std::clamp(g_native_list_page, 0, pages - 1);
  const bool can = CanStart(jd.on_job);
  for (int i = g_native_list_page * rows, n = 0; i < count && n < rows; ++i, ++n) {
    const Favorite& f = g_favorites[i];
    const int y = 655 - n * 44;
    char mass[32];
    std::snprintf(mass, sizeof mass, "~%.0f t", CargoMass(g_data, f.cargo) / 1000.0);
    ui.Text("txt.emph.left", Shorten(CityLabel(f.src_city) + " -> " + CityLabel(f.dst_city), 46), x + 30, y, 420, 30);
    ui.Text("txt.normal.left", Shorten(CargoName(g_data, f.cargo), 36) + "  (" + mass + ")", x + 460, y, 330, 30);
    if (can)
      ui.Button("Iniciar", x + 800, y, 120, [i] {
        if (ApplyRoute(g_favorites[i])) {
          g_pending = Pending::Start; // same path as the planner's button
          g_native_leave = true;
        } else {
          g_status = "Essa favorita não existe mais nos dados do jogo (cidade, empresa ou carga).";
          g_status_error = true;
        }
      });
    ui.Button("Editar", x + 930, y, 120, [i] {
      ApplyRoute(g_favorites[i]);
      g_editing = i;
      NativeGoTo(NativePage::Planner);
    });
    ui.Button("Remover", x + 1060, y, 130, [i] {
      g_favorites.erase(g_favorites.begin() + i);
      SaveFavoritesFile();
      g_editing = -1;
      g_status = "Favorita removida.";
      g_status_error = false;
    });
  }
  if (pages > 1) {
    if (g_native_list_page > 0) ui.Button("< Anterior", 430, 215, 180, [] { --g_native_list_page; });
    ui.Text("txt.normal.center", "Página " + std::to_string(g_native_list_page + 1) + " de " + std::to_string(pages), 620, 215, 200, 30);
    if (g_native_list_page < pages - 1) ui.Button("Próxima >", 830, 215, 180, [] { ++g_native_list_page; });
  }
  if (jd.on_job) ui.Text("txt.normal.center", "Cancele o serviço atual para iniciar outro.", 470, 176, 500, 30);
}

void NativeCountryPage(NativeUi& ui) {
  ui.Text("txt.big.bold.white.center", g_native_src ? "PAÍS DE ORIGEM" : "PAÍS DE DESTINO", 110, 772, 1220, 30);
  const int count = static_cast<int>(g_data.countries.size()) + 1; // "all" first
  NativeGrid(ui, count, 5, 12, 40, [&](int i, int x, int y, int w) {
    Side& side = g_native_src ? g_src : g_dst;
    if (i == 0) {
      ui.Button("Todos os países", x, y, w, [&side] {
        side.country.clear();
        NativeGoTo(NativePage::City);
      }, side.country.empty());
      return;
    }
    const Named& c = g_data.countries[i - 1];
    ui.Button(Shorten(c.name, 22), x, side.country == c.tok ? y + 6 : y, w, [&side, tok = c.tok] {
      side.country = tok;
      const Named* city = Find(g_data.cities, side.city);
      if (city && city->parent != tok) side.city.clear();
      NativeGoTo(NativePage::City); // the city comes next
    }, side.country == c.tok);
  });
  ui.Button("Voltar", 110, 150, 180, [] { NativeGoTo(NativePage::Planner); });
}

void NativeCityPage(NativeUi& ui) {
  Side& side = g_native_src ? g_src : g_dst;
  const Named* country = Find(g_data.countries, side.country);
  ui.Text("txt.big.bold.white.center", std::string(g_native_src ? "CIDADE DE ORIGEM" : "CIDADE DE DESTINO") + (country ? "  -  " + country->name : std::string()), 110, 772, 1220, 30);
  std::vector<const Named*> cities;
  for (const auto& c : g_data.cities)
    if (side.country.empty() || c.parent == side.country) cities.push_back(&c);
  NativeGrid(ui, static_cast<int>(cities.size()), 5, 12, 40, [&](int i, int x, int y, int w) {
    const Named& c = *cities[i];
    ui.Button(Shorten(c.name, 22), x, side.city == c.tok ? y + 6 : y, w, [&side, tok = c.tok, parent = c.parent] {
      side.city = tok;
      side.country = parent;
      NativeGoTo(NativePage::Planner);
    }, side.city == c.tok);
  });
  ui.Button("Voltar", 110, 150, 180, [] { NativeGoTo(NativePage::Planner); });
  ui.Button("Trocar de país", 1150, 150, 180, [] { NativeGoTo(NativePage::Country); });
}

void NativeCargoPage(NativeUi& ui) {
  ui.Text("txt.big.bold.white.center", "CARGA  -  " + CityLabel(g_src.city) + " -> " + CityLabel(g_dst.city), 110, 772, 1220, 30);
  const char* note = NativeCargoOptions();
  if (note) ui.Text("txt.normal.center", note, 110, 480, 1220);
  else
    NativeGrid(ui, static_cast<int>(g_options.size()), 2, 12, 40, [&](int i, int x, int y, int w) {
      const RouteOption& o = g_options[i];
      char mass[32];
      std::snprintf(mass, sizeof mass, "%.0f t", CargoMass(g_data, o.cargo) / 1000.0);
      const std::string label = Shorten(CargoName(g_data, o.cargo), 24) + " (" + mass + ")  " + Shorten(o.src_name, 13) + " -> " + Shorten(o.dst_name, 13) + (o.off_market ? " *" : "");
      ui.Button(label, x, g_selected == i ? y + 6 : y, w, [i] {
        g_selected = i;
        NativeGoTo(NativePage::Planner);
      }, g_selected == i);
    });
  ui.Button("Voltar", 110, 150, 180, [] { NativeGoTo(NativePage::Planner); });
  ui.Text("txt.normal.left", "* fora do mercado dessas empresas", 1030, 150, 300, 30);
}

// Writes the script of the current page. False if the file could not be written.
bool WriteNativeScript() {
  DeleteFileA(NativeScriptPath().c_str());
  ++g_native_serial;
  g_native_buttons.clear();
  NativeUi ui;
  ui.unit = "_nameless.rpl" + std::to_string(g_native_serial); // every name of this opening starts with it
  ui.group = ui.unit + ".grp";
  SPF_JobData jd{};
  if (g_tel) g_core->telemetry->Tel_GetJobData(g_tel, &jd, sizeof jd);
  // the frame every page shares, as in the game's F1 screen
  ui.Text("txt.window.bcg_rect4", "@@clr_bg_main@@", 40, 860, 1360, 820, 1);
  ui.Text("txt.big.left", "@@ui_paused@@", 60, 850, 300, 30);
  ui.Text("txt.big.center", "PLANEJADOR DE ROTAS", 420, 850, 600, 30);
  const bool planner = g_native_page != NativePage::Favorites;
  ui.Button("Planejar", 495, planner ? 816 : 810, 220, [] { NativeGoTo(NativePage::Planner); }, planner);
  ui.Button("Favoritas (" + std::to_string(g_favorites.size()) + ")", 725, planner ? 810 : 816, 220, [] { NativeGoTo(NativePage::Favorites); }, !planner);
  switch (g_native_page) {
    case NativePage::Planner: NativePlannerPage(ui, jd); break;
    case NativePage::Favorites: NativeFavoritesPage(ui, jd); break;
    case NativePage::Country: NativeCountryPage(ui); break;
    case NativePage::City: NativeCityPage(ui); break;
    case NativePage::Cargo: NativeCargoPage(ui); break;
  }
  if (!g_status.empty()) ui.Text(g_status_error ? "txt.emph.left" : "txt.normal.left", Shorten(g_status, 120), 110, 122, 1220, 26);
  ui.Button("Retomar", 620, 92, 200, [] { g_native_leave = true; });
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
  if (g_native_window) game::CloseGameWindow(&g_native_window);
  if (!WriteNativeScript()) {
    Log("janela do jogo: não consegui gravar " + NativeScriptPath());
    return false;
  }
  const int why = game::OpenGameWindow(&g_native_window, "routeplanner", ("/home/routeplanner/" + NativeScriptName()).c_str());
  if (why != 0) Log("janela do jogo: não abriu (motivo " + std::to_string(why) + ")");
  return why == 0;
}

void CloseNative() {
  const bool closed = !g_native_window || game::CloseGameWindow(&g_native_window);
  const bool resumed = g_native_paused && game::PauseForUi(false);
  Log(std::string("janela do jogo: ") + (closed ? "fechada" : "falha ao fechar") + (g_native_paused ? (resumed ? ", jogo retomado" : ", FALHA ao retomar o jogo") : ""));
  g_native_paused = false;
  g_native_buttons.clear();
  DeleteFileA(NativeScriptPath().c_str());
}

void NativeExperiment() {
  static bool was_down = false;
  if (g_native_window) {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    const bool down = pid == GetCurrentProcessId() && (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    const bool released = was_down && !down;
    was_down = down;
    if (released) {
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
    was_down = false;
  }
  if (!g_native_toggle.exchange(false)) return;
  if (g_native_window) {
    CloseNative();
    return;
  }
  g_native_paused = game::PauseForUi(true);
  if (!g_native_paused) Log("janela do jogo: NÃO consegui pausar o jogo (sem cursor)");
  if (g_native_page != NativePage::Favorites) NativeGoTo(NativePage::Planner);
  if (!ShowNative()) CloseNative();
  else Log("janela do jogo: aberta");
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
