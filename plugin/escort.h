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

namespace escort {

constexpr uintptr_t TRAFFIC = 0x36ae728;
constexpr uintptr_t SPAWN = 0x566960; // bool (traffic_mgr*, array_t<string_dyn_t>* args, placement*)
constexpr unsigned char kSpawnSig[10] = {0x40, 0x55, 0x53, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8d};

constexpr double SPAWN_BEHIND = 30.0; // m behind the truck's origin
constexpr double GAP = 30.0;          // m we try to keep
constexpr double LOST_DIST = 300.0;   // farther than this (or ahead of us) = respawn

struct Vec {
  double x = 0, y = 0, z = 0;
};

struct Car {
  uint8_t* ptr = nullptr;
  uint32_t id = 0;
  float saved_limit = 0;
};

inline bool Supported() { return std::memcmp(game::At<const void*>(SPAWN), kSpawnSig, sizeof kSpawnSig) == 0; }

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

// Speed the car should be allowed: ours, plus a correction that closes or opens the gap.
inline float FollowSpeed(double truck_speed, double dist) {
  const double correction = std::clamp((dist - GAP) * 0.4, -6.0, 10.0);
  return static_cast<float>(std::max(0.0, truck_speed + correction));
}

enum class State { Following, Lost };

// One frame of following. `dist` and `ahead` (m along our heading; positive = in front of us) are
// filled for the log. Lost = gone from traffic, too far, or it ended up in front of us.
inline State Follow(const Car& c, const Vec& truck, double heading, double truck_speed, double* dist, double* ahead) {
  if (!StillThere(c)) return State::Lost;
  __try {
    const Vec p = Position(c.ptr), f = Forward(heading);
    const double dx = p.x - truck.x, dz = p.z - truck.z;
    *dist = std::sqrt(dx * dx + dz * dz);
    *ahead = dx * f.x + dz * f.z;
    if (*dist > LOST_DIST || *ahead > 10.0) return State::Lost;
    *reinterpret_cast<float*>(c.ptr + 0x430) = FollowSpeed(truck_speed, *dist);
    return State::Following;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return State::Lost;
  }
}

// Give the car back to the game's own speed limit (it is never recomputed otherwise).
inline void Release(const Car& c) {
  if (!StillThere(c)) return;
  __try {
    *reinterpret_cast<float*>(c.ptr + 0x430) = c.saved_limit;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

} // namespace escort
