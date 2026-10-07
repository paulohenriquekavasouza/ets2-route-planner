// Run by deploy.ps1. Exits non-zero on failure.
#undef NDEBUG // asserts must run in Release too
#include <cassert>
#include <cstdio>
#include <fstream>

#include "routes.h"

int main() {
  const char* path = "routes_test.tsv";
  std::ofstream(path) << "N\tgermany\tDeutschland\nC\tberlin\tBerlin\tgermany\nC\tbremen\tBremen\tgermany\n"
                         "P\ttesco\tTesco\tberlin\nP\tkaarfor\tKaarfor\tbremen\nP\tlisette\tLisette\tbremen\n"
                         "O\ttesco\tapples\nO\ttesco\tbeef\nI\tkaarfor\tapples\nI\tlisette\tbeef\nI\tlisette\tapples\n"
                         "G\tapples\tApples\t23000\tcurtainside\t11600\nG\tbeef\tBeef\n";
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
  assert(CargoTonnes(d, "apples") == "12-23 t" && CargoTonnes(d, "beef") == "0 t");
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
