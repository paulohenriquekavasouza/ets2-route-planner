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
// `spawn vehicle` creates the car with bit 63 (debug_pause) set: it stands still until cleared.
constexpr uint64_t FLAG_DEBUG_PAUSE = 1ull << 63;

constexpr double SPAWN_BEHIND = 45.0; // m behind the truck's origin (truck + trailer are ~17 m)
constexpr double GAP = 30.0;          // m behind the truck along its trail

struct Vec {
  double x = 0, y = 0, z = 0;
};

struct Car {
  uint8_t* ptr = nullptr;
  uint32_t id = 0;
  float saved_limit = 0;
};

inline bool Supported() {
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

// ---- following the truck's own path ------------------------------------------------------------
// An AI car picks its own way at every junction, so steering only its speed cannot make it follow.
// Instead we record where the truck drove and carry the car along that trail, a fixed distance
// behind. Live RE (MODLOG v2.5): near the player the car is a physics body and the game recomputes
// vehicle+0x28 from it every tick, so the body is what we write:
//   phys = [veh+0x238] (active when byte +0x1c & 1), A = [phys+0x20], B = [A+0xf8] (the rigid body)
//   B+0x140 quaternion (x,y,z,w), B+0x150 position, B+0x15c linear velocity, B+0x168 angular velocity
//   A+0x12c position, A+0x13c quaternion (w,x,y,z)        (B+0xa0 / A+0x10c are previous-step copies)
// Body positions use the physics origin, not world coordinates, so we move the body by the same
// delta that takes the vehicle's world position to the target. Height is left to the physics.

struct Sample {
  Vec p;
  double heading = 0, pitch = 0; // SCS turns: heading 0..1, pitch +-0.25
};

struct Trail {
  std::deque<Sample> pts; // oldest first
  static constexpr double STEP = 1.0, KEEP = 250.0;

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
};

// Quaternion (w,x,y,z) for an SCS heading and pitch: yaw about +Y, then pitch about the local X.
inline void Orientation(double heading, double pitch, float q[4]) {
  const double y = heading * 3.141592653589793, p = pitch * 3.141592653589793; // half angles
  const double cy = std::cos(y), sy = std::sin(y), cp = std::cos(p), sp = std::sin(p);
  q[0] = static_cast<float>(cy * cp);
  q[1] = static_cast<float>(cy * sp);
  q[2] = static_cast<float>(sy * cp);
  q[3] = static_cast<float>(-sy * sp);
}

enum class State { Following, Lost };

constexpr double MAX_PULL = 1.5;   // m per frame the car may be moved towards its place on the trail
constexpr double DRAG_LIMIT = 80.0; // farther than this from its place = lost

// One frame: put the car at `target` (a trail point) moving at `speed` m/s. `off` gets how far it
// was from its place. Returns Lost if it is gone, too far, or has no physics body to move.
inline State Drive(const Car& c, const Sample& target, double speed, double* off) {
  if (!StillThere(c)) return State::Lost;
  __try {
    uint64_t* flags = reinterpret_cast<uint64_t*>(c.ptr + 0x4b8);
    *flags &= ~FLAG_DEBUG_PAUSE; // spawned paused; let it live
    *reinterpret_cast<float*>(c.ptr + 0x430) = static_cast<float>(speed); // keep the AI's own idea of speed in step
    const Vec at = Position(c.ptr);
    double dx = target.p.x - at.x, dz = target.p.z - at.z;
    const double d = std::sqrt(dx * dx + dz * dz);
    *off = d;
    if (d > DRAG_LIMIT) return State::Lost;
    uint8_t* const phys = game::Ptr(c.ptr, 0x238);
    if (!phys || !(phys[0x1c] & 1)) return State::Following; // no body yet (far away): the AI drives it
    uint8_t* const a = game::Ptr(phys, 0x20);
    uint8_t* const b = a ? game::Ptr(a, 0xf8) : nullptr;
    if (!b) return State::Following;
    if (d > MAX_PULL) dx *= MAX_PULL / d, dz *= MAX_PULL / d;
    float* bp = reinterpret_cast<float*>(b + 0x150);
    float* ap = reinterpret_cast<float*>(a + 0x12c);
    bp[0] += static_cast<float>(dx), bp[2] += static_cast<float>(dz);
    ap[0] += static_cast<float>(dx), ap[2] += static_cast<float>(dz);
    float q[4];
    Orientation(target.heading, target.pitch, q);
    float* bq = reinterpret_cast<float*>(b + 0x140); // x, y, z, w
    bq[0] = q[1], bq[1] = q[2], bq[2] = q[3], bq[3] = q[0];
    std::memcpy(a + 0x13c, q, sizeof q); // w, x, y, z
    const Vec f = Forward(target.heading);
    float* v = reinterpret_cast<float*>(b + 0x15c);
    v[0] = static_cast<float>(f.x * speed), v[2] = static_cast<float>(f.z * speed);
    float* w = reinterpret_cast<float*>(b + 0x168);
    w[0] = w[1] = w[2] = 0;
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
