// routes.tsv (made by tools/gen_routes.py from the user's own game files) and the one query the UI
// needs: which cargo can go from a company in city A to a company in city B.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

struct Named {
  std::string tok, name, parent; // parent: country of a city, city of a branch, flag code (iso3) of a country
};

struct RouteData {
  std::vector<Named> countries, cities, branches; // branches: tok = company, parent = city
  std::set<std::pair<std::string, std::string>> ships, receives; // (company, cargo)
  std::map<std::string, std::string> cargo_names;
  std::map<std::string, int> cargo_mass; // kg, estimated (see tools/gen_routes.py)
  std::map<std::string, std::string> cargo_icon; // cargo -> name of its picture in the game's /material/ui/cargo_logo
  std::set<std::string> logos;                   // companies the game has a logo for (/material/ui/company/small)
};

struct RouteOption {
  std::string cargo, src_company, src_name, dst_company, dst_name;
  bool off_market = false; // the companies don't normally trade this cargo
};

inline std::vector<std::string> SplitTabs(const std::string& line) {
  std::vector<std::string> out;
  std::stringstream ss(line);
  for (std::string f; std::getline(ss, f, '\t');) out.push_back(f);
  return out;
}

inline bool LoadRoutes(const std::string& path, RouteData& d) {
  std::ifstream in(path);
  if (!in) return false;
  d = {};
  for (std::string line; std::getline(in, line);) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const auto f = SplitTabs(line);
    if (f.size() < 3) continue;
    const char k = f[0][0];
    if (k == 'N') d.countries.push_back({f[1], f[2], f.size() >= 4 ? f[3] : ""});
    else if (k == 'C' && f.size() >= 4) d.cities.push_back({f[1], f[2], f[3]});
    else if (k == 'P' && f.size() >= 4) {
      d.branches.push_back({f[1], f[2], f[3]});
      if (f.size() >= 5 && f[4] == "1") d.logos.insert(f[1]);
    }
    else if (k == 'O') d.ships.insert({f[1], f[2]});
    else if (k == 'I') d.receives.insert({f[1], f[2]});
    else if (k == 'G') {
      d.cargo_names[f[1]] = f[2];
      if (f.size() >= 4) d.cargo_mass[f[1]] = std::atoi(f[3].c_str());
      if (f.size() >= 5) d.cargo_icon[f[1]] = f[4];
    }
  }
  auto by_name = [](const Named& a, const Named& b) { return a.name < b.name; };
  std::sort(d.countries.begin(), d.countries.end(), by_name);
  std::sort(d.cities.begin(), d.cities.end(), by_name);
  return !d.cities.empty();
}

inline std::string CargoName(const RouteData& d, const std::string& cargo) {
  const auto it = d.cargo_names.find(cargo);
  return it == d.cargo_names.end() ? cargo : it->second;
}

// Every (cargo, shipper in src_city, receiver in dst_city), sorted by cargo name. With `any_cargo`,
// every other cargo too, between the first companies of each city (marked off_market).
inline std::vector<RouteOption> RouteOptions(const RouteData& d, const std::string& src_city, const std::string& dst_city, bool any_cargo = false) {
  std::vector<RouteOption> out;
  for (const auto& s : d.branches) {
    if (s.parent != src_city) continue;
    for (auto it = d.ships.lower_bound({s.tok, ""}); it != d.ships.end() && it->first == s.tok; ++it)
      for (const auto& r : d.branches)
        if (r.parent == dst_city && !(r.tok == s.tok && src_city == dst_city) && d.receives.count({r.tok, it->second}))
          out.push_back({it->second, s.tok, s.name, r.tok, r.name});
  }
  if (any_cargo) {
    const Named *s = nullptr, *r = nullptr;
    for (const auto& b : d.branches) {
      if (!s && b.parent == src_city) s = &b;
      if (!r && b.parent == dst_city && (!s || b.tok != s->tok || src_city != dst_city)) r = &b;
    }
    std::set<std::string> have;
    for (const auto& o : out) have.insert(o.cargo);
    if (s && r)
      for (const auto& [cargo, name] : d.cargo_names)
        if (!have.count(cargo)) out.push_back({cargo, s->tok, s->name, r->tok, r->name, true});
  }
  std::sort(out.begin(), out.end(), [&](const RouteOption& a, const RouteOption& b) {
    const auto na = CargoName(d, a.cargo), nb = CargoName(d, b.cargo);
    return na != nb ? na < nb : a.src_name + a.dst_name < b.src_name + b.dst_name;
  });
  return out;
}

inline int CargoMass(const RouteData& d, const std::string& cargo) {
  const auto it = d.cargo_mass.find(cargo);
  return it == d.cargo_mass.end() ? 0 : it->second;
}

// ---- favourite routes: favorites.tsv next to the DLL, one route per line ----
struct Favorite {
  std::string src_city, dst_city, cargo, src_company, dst_company;
  bool operator==(const Favorite&) const = default;
};

inline std::vector<Favorite> LoadFavorites(const std::string& path) {
  std::vector<Favorite> out;
  std::ifstream in(path);
  for (std::string line; std::getline(in, line);) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const auto f = SplitTabs(line);
    if (f.size() >= 5) out.push_back({f[0], f[1], f[2], f[3], f[4]});
  }
  return out;
}

inline bool SaveFavorites(const std::string& path, const std::vector<Favorite>& favs) {
  std::ofstream out(path, std::ios::trunc);
  for (const auto& f : favs) out << f.src_city << '\t' << f.dst_city << '\t' << f.cargo << '\t' << f.src_company << '\t' << f.dst_company << '\n';
  return static_cast<bool>(out);
}

// Index of the option a favourite refers to: same cargo and companies, else the same cargo, else -1.
inline int FindOption(const std::vector<RouteOption>& options, const Favorite& f) {
  int same_cargo = -1;
  for (int i = 0; i < static_cast<int>(options.size()); ++i) {
    if (options[i].cargo != f.cargo) continue;
    if (options[i].src_company == f.src_company && options[i].dst_company == f.dst_company) return i;
    if (same_cargo < 0) same_cargo = i;
  }
  return same_cargo;
}

// The two points farthest apart (x, z), as indices into `pts`; {-1, -1} with fewer than two points.
// ponytail: O(n^2) over ~400 cities is 80k distance checks, once per click.
struct MapPoint {
  double x = 0, z = 0;
  bool valid = false;
};
inline std::pair<int, int> FarthestPair(const std::vector<MapPoint>& pts, double* dist) {
  std::pair<int, int> best{-1, -1};
  double best_d2 = -1;
  for (int i = 0; i < static_cast<int>(pts.size()); ++i)
    for (int j = i + 1; pts[i].valid && j < static_cast<int>(pts.size()); ++j) {
      if (!pts[j].valid) continue;
      const double dx = pts[i].x - pts[j].x, dz = pts[i].z - pts[j].z, d2 = dx * dx + dz * dz;
      if (d2 > best_d2) best_d2 = d2, best = {i, j};
    }
  if (dist) *dist = best_d2 < 0 ? 0 : std::sqrt(best_d2);
  return best;
}

// Index of the valid point closest to (x, z), or -1; `dist` gets the distance.
inline int NearestPoint(const std::vector<MapPoint>& pts, double x, double z, double* dist) {
  int best = -1;
  double best_d2 = 0;
  for (int i = 0; i < static_cast<int>(pts.size()); ++i) {
    if (!pts[i].valid) continue;
    const double dx = pts[i].x - x, dz = pts[i].z - z, d2 = dx * dx + dz * dz;
    if (best < 0 || d2 < best_d2) best = i, best_d2 = d2;
  }
  if (dist) *dist = std::sqrt(best_d2);
  return best;
}

// SCS token (base 38, first character least significant), as the game's console commands parse it.
constexpr unsigned long long Token(const char* s) {
  unsigned long long value = 0, scale = 1;
  for (; *s; ++s, scale *= 38) {
    const char c = *s;
    const unsigned long long digit = c >= '0' && c <= '9' ? c - '0' + 1 : c >= 'a' && c <= 'z' ? c - 'a' + 11 : 37;
    value += digit * scale;
  }
  return value;
}

inline std::string Untoken(unsigned long long v) {
  std::string s;
  for (; v; v /= 38) {
    const int d = int(v % 38);
    s += d == 0 ? '?' : d <= 10 ? char('0' + d - 1) : d <= 36 ? char('a' + d - 11) : '_';
  }
  return s;
}
