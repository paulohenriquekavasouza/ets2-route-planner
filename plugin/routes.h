// routes.tsv (made by tools/gen_routes.py from the user's own game files) and the one query the UI
// needs: which cargo can go from a company in city A to a company in city B.
#pragma once
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

struct Named {
  std::string tok, name, parent; // parent: country of a city, city of a branch
};

struct RouteData {
  std::vector<Named> countries, cities, branches; // branches: tok = company, parent = city
  std::set<std::pair<std::string, std::string>> ships, receives; // (company, cargo)
  std::map<std::string, std::string> cargo_names;
  std::map<std::string, int> cargo_mass; // kg, estimated (see tools/gen_routes.py)
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
    if (k == 'N') d.countries.push_back({f[1], f[2], ""});
    else if (k == 'C' && f.size() >= 4) d.cities.push_back({f[1], f[2], f[3]});
    else if (k == 'P' && f.size() >= 4) d.branches.push_back({f[1], f[2], f[3]});
    else if (k == 'O') d.ships.insert({f[1], f[2]});
    else if (k == 'I') d.receives.insert({f[1], f[2]});
    else if (k == 'G') {
      d.cargo_names[f[1]] = f[2];
      if (f.size() >= 4) d.cargo_mass[f[1]] = std::atoi(f[3].c_str());
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

enum SortBy { SORT_CARGO, SORT_HEAVY, SORT_LIGHT, SORT_SRC, SORT_DST };
inline const char* const kSortNames[] = {"Carga (A–Z)", "Mais pesada", "Mais leve", "Empresa de origem", "Empresa de destino"};

// Stable, so ties keep the cargo-name order RouteOptions produced.
inline void SortOptions(const RouteData& d, std::vector<RouteOption>& v, SortBy by) {
  auto key = [&](const RouteOption& o) -> std::string {
    if (by == SORT_SRC) return o.src_name;
    if (by == SORT_DST) return o.dst_name;
    return CargoName(d, o.cargo);
  };
  std::stable_sort(v.begin(), v.end(), [&](const RouteOption& a, const RouteOption& b) {
    if (by == SORT_HEAVY) return CargoMass(d, a.cargo) > CargoMass(d, b.cargo);
    if (by == SORT_LIGHT) return CargoMass(d, a.cargo) < CargoMass(d, b.cargo);
    return key(a) < key(b);
  });
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
