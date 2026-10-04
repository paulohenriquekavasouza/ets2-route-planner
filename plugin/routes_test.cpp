// Run by deploy.ps1. Exits non-zero on failure.
#undef NDEBUG // asserts must run in Release too
#include <cassert>
#include <cstdio>
#include <fstream>

#include "escort.h"
#include "routes.h"

int main() {
  const char* path = "routes_test.tsv";
  std::ofstream(path) << "N\tgermany\tDeutschland\ttraffic.passat_cc.pol_de\nC\tberlin\tBerlin\tgermany\nC\tbremen\tBremen\tgermany\n"
                         "P\ttesco\tTesco\tberlin\nP\tkaarfor\tKaarfor\tbremen\nP\tlisette\tLisette\tbremen\n"
                         "O\ttesco\tapples\nO\ttesco\tbeef\nI\tkaarfor\tapples\nI\tlisette\tbeef\nI\tlisette\tapples\n"
                         "G\tapples\tApples\t23000\nG\tbeef\tBeef\n";
  RouteData d;
  assert(LoadRoutes(path, d));
  assert(d.cities.size() == 2 && d.branches.size() == 3);
  const auto o = RouteOptions(d, "berlin", "bremen");
  assert(o.size() == 3); // apples->kaarfor, apples->lisette, beef->lisette
  assert(o[0].cargo == "apples" && o[2].cargo == "beef" && o[2].dst_company == "lisette");
  assert(RouteOptions(d, "bremen", "berlin").empty());
  const auto any = RouteOptions(d, "bremen", "berlin", true); // nobody in bremen ships anything
  assert(any.size() == 2 && any[0].off_market && any[0].src_company == "kaarfor" && any[0].dst_company == "tesco");
  assert(CargoMass(d, "apples") == 23000 && CargoMass(d, "beef") == 0); // mass column is optional
  assert(d.countries[0].parent == "traffic.passat_cc.pol_de");
  assert(std::abs(escort::Forward(0.25).x + 1.0) < 1e-9 && std::abs(escort::Forward(0).z + 1.0) < 1e-9);
  // trail: straight 40 m north then a point 10 m west; 30 m back from the end is on the first leg
  escort::Trail trail;
  for (int i = 0; i <= 40; ++i) trail.Add({{0, 0, -double(i)}, 0, 0});
  for (int i = 1; i <= 10; ++i) trail.Add({{-double(i), 0, -40}, 0.25, 0});
  escort::Sample at;
  assert(trail.At(30, &at) && std::abs(at.p.x) < 1e-9 && std::abs(at.p.z + 20) < 1e-9 && at.heading == 0);
  assert(trail.At(5, &at) && std::abs(at.p.x + 5) < 1e-9 && at.heading == 0.25 && !trail.At(60, &at));
  trail.Add({{5000, 0, 5000}, 0, 0}); // teleport drops the old trail
  assert(trail.pts.size() == 1);
  float q[4];
  escort::Orientation(0.25, 0, q); // 90 degrees left about +Y
  assert(std::abs(q[0] - 0.70710678f) < 1e-5 && std::abs(q[2] - 0.70710678f) < 1e-5 && q[1] == 0 && q[3] == 0);
  // favourites survive a save/load round trip, and find their option again
  const std::vector<Favorite> favs = {{"berlin", "bremen", "beef", "tesco", "lisette"}, {"berlin", "bremen", "apples", "tesco", "nobody"}};
  assert(SaveFavorites("fav_test.tsv", favs) && LoadFavorites("fav_test.tsv") == favs && LoadFavorites("missing.tsv").empty());
  assert(FindOption(o, favs[0]) == 2 && FindOption(o, favs[1]) == 0 && FindOption(o, {"", "", "gold", "", ""}) == -1);
  double far_m = 0;
  const auto ends = FarthestPair({{0, 0, true}, {30, 40, true}, {1000, 1000, false}, {-3, -4, true}}, &far_m);
  assert(ends.first == 1 && ends.second == 3 && std::abs(far_m - 55.0) < 1e-9 && FarthestPair({{1, 1, true}}, nullptr).first == -1);
  double near_m = 0;
  assert(NearestPoint({{0, 0, true}, {30, 40, true}, {28, 39, false}}, 27, 36, &near_m) == 1 && std::abs(near_m - 5.0) < 1e-9);
  assert(NearestPoint({{1, 1, false}}, 0, 0, nullptr) == -1);
  static_assert(Token("a") == 11 && Token("0_") == 1 + 37 * 38);
  assert(Untoken(Token("gld_frm_grg")) == "gld_frm_grg");
  std::puts("routes_test ok");
  return 0;
}
