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
  // escort speed: matches the truck at the wanted gap, faster when far, slower when close, never negative
  assert(escort::FollowSpeed(20, escort::GAP) == 20.0f && escort::FollowSpeed(20, 200) == 30.0f);
  assert(escort::FollowSpeed(20, 5) == 14.0f && escort::FollowSpeed(2, 0) == 0.0f);
  assert(std::abs(escort::Forward(0.25).x + 1.0) < 1e-9 && std::abs(escort::Forward(0).z + 1.0) < 1e-9);
  static_assert(Token("a") == 11 && Token("0_") == 1 + 37 * 38);
  assert(Untoken(Token("gld_frm_grg")) == "gld_frm_grg");
  std::puts("routes_test ok");
  return 0;
}
