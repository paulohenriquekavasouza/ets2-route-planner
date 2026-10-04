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
  // trail: 40 m north, then 10 m west
  escort::Trail trail;
  for (int i = 0; i <= 40; ++i) trail.Add({{0, 0, -double(i)}, 0});
  for (int i = 1; i <= 10; ++i) trail.Add({{-double(i), 0, -40}, 0.25});
  escort::Sample at;
  assert(trail.At(30, &at) && std::abs(at.p.x) < 1e-9 && std::abs(at.p.z + 20) < 1e-9 && at.heading == 0);
  assert(trail.At(5, &at) && std::abs(at.p.x + 5) < 1e-9 && at.heading == 0.25 && !trail.At(60, &at));
  escort::Projection pr;
  assert(trail.Project({1.5, 0, -20}, &pr) && std::abs(pr.behind - 30) < 1e-9 && std::abs(pr.lateral - 1.5) < 1e-9 && pr.heading == 0);
  // a car 30 m back in our lane facing our way is a good place; the next lane, oncoming, or in front of us is not
  const escort::Vec truck{-10, 0, -40}, north{0, 0, -1}, south{0, 0, 1};
  assert(escort::Locate(trail, truck, 0.25, {1.0, 0, -20}, north).Good(escort::MIN_BEHIND));
  assert(!escort::Locate(trail, truck, 0.25, {4.0, 0, -20}, north).Good(escort::MIN_BEHIND)); // 4 m aside = next lane
  assert(!escort::Locate(trail, truck, 0.25, {1.0, 0, -20}, south).Good(escort::MIN_BEHIND)); // oncoming
  const escort::Place front = escort::Locate(trail, truck, 0.25, {-25, 0, -40}, escort::Forward(0.25));
  assert(!front.on_trail && front.behind < -10 && !front.Good(escort::MIN_BEHIND)); // 15 m ahead of the truck
  // speed: ours at the right distance, faster when far, slower when close, stopped behind a stopped truck, never negative
  assert(escort::WantSpeed(20, 30, 30) == 20.0f && escort::WantSpeed(20, 200, 30) == 32.0f && escort::WantSpeed(20, 10, 30) == 13.0f);
  assert(escort::WantSpeed(-0.0, 31, 30) == 0.0f && escort::WantSpeed(0, 80, 30) == 12.0f && escort::WantSpeed(-3, 0, 30) == 0.0f);
  trail.Add({{5000, 0, 5000}, 0}); // teleport drops the old trail
  assert(trail.pts.size() == 1);
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
