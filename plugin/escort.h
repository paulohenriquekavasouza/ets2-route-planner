// Police escort: spawns the country's police car behind the truck through the game's own traffic
// spawner and keeps it there. Traffic layout from the ets2-police project (exe 1.61.1.1):
//   traffic manager = [exe+0x36ae728]; vehicle array data [mgr+0xf8], count [mgr+0x100], 16-byte
//   elements whose first qword is a traffic_ai_vehicle_t*.
//   vehicle: +0x28 f32 x,y,z + i16 sector x,z (world = local + sector*512), +0x420 id (u32),
//   +0x430 speed limit (f32 m/s, the game does not recompute it), +0x518 model name (char*).
// Spawning = the handler behind the console command `spawn vehicle <name>` (0x3f9df0), which builds
// a placement from the camera and calls 0x566960(traffic_mgr, args*, placement*). We call that
// directly with a placement behind the truck. The car is an ordinary AI car afterwards: it follows
// its lane and keeps its gap to the vehicle ahead (us); we only steer its speed limit.
#pragma once
#include "game.h"

#include <cmath>
#include <deque>

namespace escort {

constexpr uintptr_t TRAFFIC = 0x36ae728;
constexpr uintptr_t SPAWN = 0x566960; // bool (traffic_mgr*, array_t<string_dyn_t>* args, placement*)
constexpr unsigned char kSpawnSig[10] = {0x40, 0x55, 0x53, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8d};
// How the game deletes an AI car (0x923951, "Removing AI crashed into debug-paused vehicle"):
// 0xace9e0(vehicle+0x80), then flags (+0x4b8) |= bit 24. The traffic update drops it afterwards.
constexpr uintptr_t DETACH = 0xace9e0;
constexpr unsigned char kDetachSig[10] = {0x40, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xf9, 0x48};
constexpr uint64_t FLAG_REMOVE = 1ull << 24;
// Junctions: the traffic editor's "Force navigation" (0xcd1b00). Position -> map item (0x6c6510) ->
// its traffic object (0x6d60f0) -> nearest lane/curve (virtual +0x80). 0x94d090(curve, on) sets bit 6
// of curve+0x74 ("forced") and bit 7 on its sibling curves (same entry, other exits), so every AI car
// that reaches that entry takes the forced curve. Plain road lanes report type 0x500000 and cannot
// be forced; junction curves report 0x600000. The editor then calls 0x9452a0(traffic object) to
// rebuild it, which reads the map editor's own object ([exe+0x36ae738], null in the game) and
// faults: never call it. The flags alone are what the AI obeys (confirmed in game, MODLOG v2.6.4).
constexpr uintptr_t MAP_ITEM_AT = 0x6c6510, TRAFFIC_OBJECT = 0x6d60f0, FORCE_CURVE = 0x94d090;
constexpr unsigned char kJunctionSigs[3][10] = {
    {0x4c, 0x8b, 0xdc, 0x48, 0x81, 0xec, 0xc8, 0x00, 0x00, 0x00},
    {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10},
    {0x48, 0x89, 0x5c, 0x24, 0x18, 0x48, 0x89, 0x7c, 0x24, 0x20},
};
constexpr int LANE_ROAD = 0x500000;
constexpr uint64_t AI_ACCESS = 0xffffffffffull;
// `spawn vehicle` creates the car with bit 63 (debug_pause) set: it stands still until cleared.
constexpr uint64_t FLAG_DEBUG_PAUSE = 1ull << 63;

constexpr double MIN_BEHIND = 18.0;   // m: a truck with its trailer is ~17 m long, so nearer than this is on top of us

struct Vec {
  double x = 0, y = 0, z = 0;
};

struct Car {
  uint8_t* ptr = nullptr;
  uint32_t id = 0;
  float saved_limit = 0;
};

inline bool Supported() {
  const uintptr_t junction[3] = {MAP_ITEM_AT, TRAFFIC_OBJECT, FORCE_CURVE};
  for (int i = 0; i < 3; ++i)
    if (std::memcmp(game::At<const void*>(junction[i]), kJunctionSigs[i], 10) != 0) return false;
  return std::memcmp(game::At<const void*>(SPAWN), kSpawnSig, sizeof kSpawnSig) == 0 &&
         std::memcmp(game::At<const void*>(DETACH), kDetachSig, sizeof kDetachSig) == 0;
}

// Unit forward vector for an SCS heading (0..1; 0 = north = -Z, 0.25 = west = -X).
inline Vec Forward(double heading) {
  const double a = heading * 6.283185307179586;
  return {-std::sin(a), 0, -std::cos(a)};
}

inline Vec Position(const uint8_t* v) {
  const float* p = reinterpret_cast<const float*>(v + 0x28);
  const int16_t* s = reinterpret_cast<const int16_t*>(v + 0x34);
  return {p[0] + s[0] * 512.0, p[1], p[2] + s[1] * 512.0};
}

// Spawns `model` (e.g. "traffic.passat_2014.pol_no") on the road nearest to `at`, facing `heading`.
inline bool Spawn(const char* model, const Vec& at, double heading) {
  struct Str {
    uintptr_t vtbl;
    const char* data;
    size_t size, capacity;
  };
  struct Placement {
    float x, y, z;
    int16_t sx, sz;
    float q[4]; // w, x, y, z
  };
  __try {
    uint8_t* const mgr = *game::At<uint8_t**>(TRAFFIC);
    if (!mgr) return false;
    const uintptr_t vt = game::Base() + game::STRING_VTBL;
    Str argv[3] = {{vt, "spawn", 5, 0}, {vt, "vehicle", 7, 0}, {vt, model, std::strlen(model), 0}};
    alignas(16) uint8_t args[0x40] = {};
    *reinterpret_cast<Str**>(args + 0x18) = argv;
    *reinterpret_cast<uint64_t*>(args + 0x20) = 3;
    Placement pl{};
    pl.sx = static_cast<int16_t>(std::floor(at.x / 512.0));
    pl.sz = static_cast<int16_t>(std::floor(at.z / 512.0));
    pl.x = static_cast<float>(at.x - pl.sx * 512.0);
    pl.y = static_cast<float>(at.y);
    pl.z = static_cast<float>(at.z - pl.sz * 512.0);
    const double half = heading * 6.283185307179586 / 2; // rotation about +Y takes -Z to Forward(heading)
    pl.q[0] = static_cast<float>(std::cos(half));
    pl.q[2] = static_cast<float>(std::sin(half));
    return game::At<bool (*)(void*, void*, void*)>(SPAWN)(mgr, args, &pl);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// The traffic vehicle called `model` closest to `around` (within `radius`), or an empty Car.
inline Car Find(const char* model, const Vec& around, double radius) {
  Car best;
  __try {
    const uint8_t* const mgr = *game::At<uint8_t**>(TRAFFIC);
    if (!mgr) return best;
    const uint8_t* data = game::Ptr(mgr, 0xf8);
    const uint64_t n = *reinterpret_cast<const uint64_t*>(mgr + 0x100);
    double best_d = radius * radius;
    for (uint64_t i = 0; data && i < n && i < 4096; ++i) {
      uint8_t* v = game::Ptr(data + i * 16, 0);
      const char* name = v ? *reinterpret_cast<const char* const*>(v + 0x518) : nullptr;
      if (!name || std::strcmp(name, model) != 0) continue;
      const Vec p = Position(v);
      const double d = (p.x - around.x) * (p.x - around.x) + (p.z - around.z) * (p.z - around.z);
      if (d < best_d) {
        best_d = d;
        best.ptr = v;
        best.id = *reinterpret_cast<const uint32_t*>(v + 0x420);
        best.saved_limit = *reinterpret_cast<const float*>(v + 0x430);
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    best = {};
  }
  return best;
}

inline bool StillThere(const Car& c) {
  __try {
    const uint8_t* const mgr = *game::At<uint8_t**>(TRAFFIC);
    if (!mgr || !c.ptr) return false;
    const uint8_t* data = game::Ptr(mgr, 0xf8);
    const uint64_t n = *reinterpret_cast<const uint64_t*>(mgr + 0x100);
    for (uint64_t i = 0; data && i < n && i < 4096; ++i)
      if (game::Ptr(data + i * 16, 0) == c.ptr) return *reinterpret_cast<const uint32_t*>(c.ptr + 0x420) == c.id;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  return false;
}

// What the panel shows. Speed: near the player the car has a physics body ([v+0x238], active when
// byte +0x1c & 1) whose +0x70 is the real speed; otherwise +0x434 (target speed) is the speed.
struct Info {
  bool valid = false;
  char model[64] = {};
  uint32_t id = 0;
  float speed = 0, limit = 0, target = 0;
  uint64_t flags = 0;
  Vec pos;
};

inline Info Read(const Car& c) {
  Info i;
  if (!StillThere(c)) return i;
  __try {
    const char* name = *reinterpret_cast<const char* const*>(c.ptr + 0x518);
    if (name) strncpy_s(i.model, name, _TRUNCATE);
    i.id = c.id;
    i.limit = *reinterpret_cast<const float*>(c.ptr + 0x430);
    i.target = *reinterpret_cast<const float*>(c.ptr + 0x434);
    i.flags = *reinterpret_cast<const uint64_t*>(c.ptr + 0x4b8);
    const uint8_t* phys = game::Ptr(c.ptr, 0x238);
    i.speed = phys && (phys[0x1c] & 1) ? *reinterpret_cast<const float*>(phys + 0x70) : i.target;
    i.pos = Position(c.ptr);
    i.valid = true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    i.valid = false;
  }
  return i;
}

// Deletes the car from the world the way the game does. Returns false if it was already gone.
inline bool Remove(const Car& c) {
  if (!StillThere(c)) return false;
  __try {
    uint64_t* flags = reinterpret_cast<uint64_t*>(c.ptr + 0x4b8);
    if (*flags & FLAG_REMOVE) return true;
    game::At<void (*)(void*)>(DETACH)(c.ptr + 0x80);
    *flags |= FLAG_REMOVE;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// ---- junctions ----------------------------------------------------------------------------------
struct Curve {
  uint8_t* item = nullptr;  // the junction curve under a position (never a plain road lane)
  uint8_t* owner = nullptr; // the traffic object (prefab) it belongs to
};

// The forceable curve at `p`: the one a vehicle standing there is driving. Empty on plain road.
// `why` (optional) gets how far the lookup went: 1 no traffic manager, 2 no map item there, 3 the item has
// no traffic object, 4 no lane found, 5 a plain road lane, 6 no AI access, 7 a forceable curve, 9 exception;
// `type` gets the lane's type id.
inline Curve CurveAt(const Vec& p, int* why = nullptr, int* type = nullptr) {
  struct Pos {
    float x, y, z;
    int16_t sx, sz;
    float q[4];
  };
  struct Found {
    uint8_t* item;
    float dist;
  };
  using ItemAtFn = void* (*)(void*, bool, float, bool);
  using NearestFn = bool (*)(void*, Found*, Pos*, uint32_t);
  int stage = 1, kind = 0;
  Curve out;
  __try {
    uint8_t* const mgr = *game::At<uint8_t**>(TRAFFIC);
    Pos pos{};
    pos.sx = static_cast<int16_t>(std::floor(p.x / 512.0));
    pos.sz = static_cast<int16_t>(std::floor(p.z / 512.0));
    pos.x = static_cast<float>(p.x - pos.sx * 512.0);
    pos.y = static_cast<float>(p.y);
    pos.z = static_cast<float>(p.z - pos.sz * 512.0);
    pos.q[0] = 1.0f;
    Found found{nullptr, -1.0f};
    void* map_item = nullptr;
    uint8_t* owner = nullptr;
    if (mgr) {
      stage = 2;
      map_item = game::At<ItemAtFn>(MAP_ITEM_AT)(&pos, false, 8.0f, false);
      if (!map_item) map_item = game::At<ItemAtFn>(MAP_ITEM_AT)(&pos, false, 20.0f, true);
    }
    if (map_item) {
      stage = 3;
      owner = game::At<uint8_t* (*)(void*, void*)>(TRAFFIC_OBJECT)(mgr, map_item);
    }
    if (owner) {
      stage = 4;
      if (!(*reinterpret_cast<NearestFn**>(owner))[16](owner, &found, &pos, 0x8000)) found.item = nullptr;
    }
    if (found.item) {
      void** vt = *reinterpret_cast<void***>(found.item);
      kind = reinterpret_cast<int (*)(void*)>(vt[1])(found.item);
      stage = 5;
      if (kind != LANE_ROAD) {
        const uint64_t* access = reinterpret_cast<const uint64_t* (*)(void*)>(vt[8])(found.item);
        stage = 6;
        if (access && (*access & AI_ACCESS)) {
          stage = 7;
          out = {found.item, owner};
        }
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    out = {};
    stage = 9;
  }
  if (why) *why = stage;
  if (type) *type = kind;
  return out;
}

inline bool ForceCurve(const Curve& c, bool on) {
  __try {
    game::At<void (*)(void*, bool)>(FORCE_CURVE)(c.item, on);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// ---- following the truck's own path ------------------------------------------------------------
// The car stays an ordinary AI car (it steers, its wheels turn, the physics is the game's). What we
// add is the escort's brain, like the Special Transport controller: where the car is relative to the
// trail the truck left, and how fast it should go to hold its place behind us.
//
// What does NOT work (MODLOG v2.6): writing the car's physics body (A=[phys+0x20] +0x12c, B=[A+0xf8]
// +0x150) only sticks while the body is asleep (car stopped); with the car moving the physics engine
// overwrites it every step, so the car cannot be dragged along the trail.
// What works (ets2-police): the speed limit (+0x430, never recomputed by the game; negative = "no
// limit") and, for cars with an active physics object, the speed itself (phys+0x70 and +0xe8).

struct Sample {
  Vec p;
  double heading = 0; // SCS turns, 0..1
};

struct Projection {
  double behind = 0;  // m along the trail from the car's nearest trail point to the trail's newest point
  double lateral = 0; // m between the car and that nearest point
  double heading = 0; // the truck's heading when it passed there
  Vec at;             // that nearest point
};

struct Trail {
  std::deque<Sample> pts; // oldest first
  static constexpr double STEP = 1.0, KEEP = 400.0;

  void Add(const Sample& s) {
    if (!pts.empty()) {
      const double dx = s.p.x - pts.back().p.x, dz = s.p.z - pts.back().p.z;
      const double d2 = dx * dx + dz * dz;
      if (d2 < STEP * STEP) return;
      if (d2 > 50.0 * 50.0) pts.clear(); // teleported: the old trail leads nowhere
    }
    pts.push_back(s);
    while (pts.size() > static_cast<size_t>(KEEP / STEP)) pts.pop_front();
  }

  double Length() const { return pts.size() < 2 ? 0 : (pts.size() - 1) * STEP; } // points are ~STEP apart

  // The point `behind` metres back along the trail from its newest point. False if the trail is shorter.
  bool At(double behind, Sample* out) const {
    double left = behind;
    for (size_t i = pts.size(); i-- > 1;) {
      const Sample &a = pts[i], &b = pts[i - 1];
      const double dx = b.p.x - a.p.x, dy = b.p.y - a.p.y, dz = b.p.z - a.p.z;
      const double len = std::sqrt(dx * dx + dz * dz);
      if (len >= left) {
        const double t = len > 0 ? left / len : 0;
        *out = a;
        out->p = {a.p.x + dx * t, a.p.y + dy * t, a.p.z + dz * t};
        return true;
      }
      left -= len;
    }
    return false;
  }

  // Where `p` is relative to the trail. False while the trail has fewer than two points.
  // After a U-turn (or a second lap of the same road) two stretches of trail run side by side, and
  // the nearest point may be on the wrong one. `hint` is how far behind the car was last time: with it,
  // only the stretch within HINT_WINDOW of that is considered (if the car is near it at all).
  static constexpr double HINT_WINDOW = 40.0;
  bool Project(const Vec& p, Projection* out, double hint = -1) const {
    if (pts.size() < 2) return false;
    size_t best = 0, near_best = 0;
    double best_d2 = 1e30, near_d2 = 1e30, near_behind = 0, best_behind = 0, behind = 0;
    for (size_t i = pts.size(); i-- > 0;) { // newest to oldest, accumulating the distance behind
      if (i + 1 < pts.size()) {
        const double sx = pts[i + 1].p.x - pts[i].p.x, sz = pts[i + 1].p.z - pts[i].p.z;
        behind += std::sqrt(sx * sx + sz * sz);
      }
      const double dx = pts[i].p.x - p.x, dz = pts[i].p.z - p.z, d2 = dx * dx + dz * dz;
      if (d2 < best_d2) best_d2 = d2, best = i, best_behind = behind;
      if (hint >= 0 && std::abs(behind - hint) <= HINT_WINDOW && d2 < near_d2) near_d2 = d2, near_best = i, near_behind = behind;
    }
    if (near_d2 <= 12.0 * 12.0) best = near_best, best_d2 = near_d2, best_behind = near_behind;
    *out = {best_behind, std::sqrt(best_d2), pts[best].heading, pts[best].p};
    return true;
  }
};

// The escort's speed: ours, plus a correction that closes or opens the distance to its place.
// Far behind it speeds up to catch up (up to +12 m/s over us), too close it backs off (down to
// -8 m/s), and when we are stopped it stops once it is about in place.
inline float WantSpeed(double truck_speed, double gap, double gap_target) {
  const double v = std::max(0.0, truck_speed), err = gap - gap_target;
  if (v < 0.5 && err < 3.0) return 0.0f;
  return static_cast<float>(std::clamp(v + std::clamp(err * 0.35, -8.0, 12.0), 0.0, 42.0));
}

// How good a place is for an escort car, on the trail (or relative to the truck while there is no
// trail yet): behind us, in our lane, facing our way.
struct Place {
  double behind = 0;   // m behind the truck (negative = ahead of it)
  double lateral = 0;  // m off our path
  double facing = 0;   // 1 = same direction as us there, -1 = oncoming
  bool on_trail = false;
  double side = 0;     // on the trail: m our path is to the car's right (negative = to its left)
  // lanes are 3.5-4.5 m apart and the trail runs down the middle of ours: 1.8 m still means "our lane"
  bool Good(double min_behind) const { return behind >= min_behind && lateral <= 1.8 && facing >= 0.7; }
};

// `hint` = how far behind the car was last time (-1 if unknown), see Trail::Project.
inline Place Locate(const Trail& trail, const Vec& truck, double truck_heading, const Vec& car, const Vec& car_forward, double hint = -1) {
  Place pl;
  Projection pr;
  // on the trail if it is near it and not at its very tip (the tip also catches cars in front of us)
  if (trail.Project(car, &pr, hint) && pr.lateral <= 12.0 && pr.behind >= 2.0) {
    const Vec f = Forward(pr.heading);
    // the car's right-hand vector is (-forward.z, forward.x): for a car heading north (-Z) that is east (+X)
    const double side = (pr.at.x - car.x) * -car_forward.z + (pr.at.z - car.z) * car_forward.x;
    pl = {pr.behind, pr.lateral, car_forward.x * f.x + car_forward.z * f.z, true, side};
  } else {
    const Vec f = Forward(truck_heading);
    const double dx = car.x - truck.x, dz = car.z - truck.z;
    pl = {-(dx * f.x + dz * f.z), std::abs(dx * f.z - dz * f.x), car_forward.x * f.x + car_forward.z * f.z, false};
  }
  return pl;
}

// Forward vector of a traffic vehicle (quaternion w,x,y,z at +0x38, forward = -Z).
inline Vec CarForward(const uint8_t* v) {
  const float* q = reinterpret_cast<const float*>(v + 0x38);
  const double w = q[0], x = q[1], y = q[2], z = q[3];
  return {-(2 * (x * z + w * y)), 0, -(1 - 2 * (x * x + y * y))};
}

constexpr uint64_t FLAG_ALLOW_OVERTAKE = 1ull << 20;
constexpr float HOLD_LIMIT = 0.001f; // "stand still"; 0 or negative would mean "no limit"

// One frame of escorting. Fills `place` (where the car is) and `want` (the speed asked of it).
// False if the car is gone from traffic.
inline bool Steer(const Car& c, const Trail& trail, const Vec& truck, double truck_heading, double truck_speed, double gap_target, double dt,
                  double hint, Place* place, float* want) {
  if (!StillThere(c)) return false;
  __try {
    *place = Locate(trail, truck, truck_heading, Position(c.ptr), CarForward(c.ptr), hint);
    *want = WantSpeed(truck_speed, place->behind, gap_target);
    uint64_t* flags = reinterpret_cast<uint64_t*>(c.ptr + 0x4b8);
    *flags &= ~(FLAG_DEBUG_PAUSE | FLAG_ALLOW_OVERTAKE); // spawned paused; and an escort does not overtake us
    *reinterpret_cast<float*>(c.ptr + 0x430) = std::max(*want, HOLD_LIMIT);
    // The AI only accelerates ~1 m/s^2 on its own; nudge the real speed so it catches up and brakes like an escort.
    uint8_t* const phys = game::Ptr(c.ptr, 0x238);
    if (phys && (phys[0x1c] & 1)) {
      const float cur = *reinterpret_cast<const float*>(phys + 0x70);
      float next = cur;
      if (cur < *want - 1.5f) next = std::min(*want, cur + static_cast<float>(3.0 * dt));
      else if (cur > *want + 1.5f) next = std::max(*want, cur - static_cast<float>(6.0 * dt));
      if (next != cur) {
        *reinterpret_cast<float*>(phys + 0x70) = next;
        *reinterpret_cast<float*>(phys + 0xe8) = next;
      }
    }
    // Our lane: on a road with several lanes the AI may sit in the one next to ours. Its lateral
    // displacement (+0x460, metres, positive = to its right; the ets2-police "pull over") moves it onto
    // our path without changing what the AI thinks its lane is. Only between 0.6 and 8 m off, facing our way.
    // (A version that only moved it when a whole lane off, v2.7.0, followed worse and was dropped.)
    float* displace = reinterpret_cast<float*>(c.ptr + 0x460);
    if (std::isfinite(*displace) && std::abs(*displace) < 12.0f) {
      float goal = *displace;
      if (place->on_trail && place->facing > 0.7 && std::abs(place->side) > 0.6 && std::abs(place->side) < 8.0)
        goal = std::clamp(*displace + static_cast<float>(place->side), -7.0f, 7.0f);
      const float step = static_cast<float>(1.2 * dt); // m/s sideways: a lane change, not a jump
      *displace = goal > *displace ? std::min(goal, *displace + step) : std::max(goal, *displace - step);
    }
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// Where a car that was just spawned ended up (to accept or reject it). False if it is gone.
inline bool LocateCar(const Car& c, const Trail& trail, const Vec& truck, double truck_heading, double hint, Place* place) {
  if (!StillThere(c)) return false;
  __try {
    *place = Locate(trail, truck, truck_heading, Position(c.ptr), CarForward(c.ptr), hint);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

} // namespace escort
