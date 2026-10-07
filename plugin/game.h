// Calls into eurotrucks2.exe 1.61.1.1 (RVAs found statically; see MODLOG.md). Starting a job replays
// what the console command `cheat get_job <city> <city>` does, but with our cargo and companies;
// cancelling calls the same function as the game's own "cancel job" paths.
// Every entry point checks the function prologues first and runs under SEH, so another game build
// turns the feature off instead of crashing.
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace game {

constexpr uintptr_t CTRL = 0x3045760;     // game/economy controller pointer
constexpr uintptr_t GEN = 0x82ffb0;       // void (result*, params*, bool, bool): generate job offer
constexpr uintptr_t TAKE = 0x82eed0;      // int (params*, offer*, bool, bool, bool): take it, 0 = ok
constexpr uintptr_t PARAMS_DTOR = 0x82e270;
constexpr uintptr_t STRING_DTOR = 0x11a290;
constexpr uintptr_t FREE = 0xfbf00;
constexpr uintptr_t FIND_COMPANY = 0x7d0df0; // company* (u64* company token, u64* city token), as the generator looks them up
constexpr uintptr_t CARGO = 0xab7840;     // cargo_data* (u64* token): the game's cargo by token, null/dead if missing
constexpr uintptr_t TELEPORT = 0x5ddf20;  // bool (actor*, placement*, bool, bool, bool): what company_portal ends with
constexpr uintptr_t ACTOR_OWNER = 0x36ae6d8; // game object; +0x31b0 = the player's actor
constexpr uintptr_t CANCEL = 0x7a5c40;    // void (ctrl*): cancel the player's job (penalty applies)
constexpr uintptr_t STRING_VTBL = 0x21d18c0, EMPTY_STR = 0x1df110e, PARAMS_FLOAT = 0x251d65c;
// In the generator, right after the unit calculator (0x84f0e0): `mov r12d,eax; cmp eax,1; jae ok`,
// else mp_job_country_cargo_allowance_issue (18). 0 units = cargo too heavy/big for the trailer the
// route's countries allow. Patched to "at least 1 unit" only while we generate our own job.
constexpr uintptr_t UNITS_CHECK = 0x830bb3;
constexpr unsigned char kUnitsOrig[14] = {0x44, 0x8b, 0xe0, 0x83, 0xf8, 0x01, 0x73, 0x11, 0x48, 0x8b, 0x9d, 0xe0, 0x01, 0x00};
constexpr unsigned char kUnitsAtLeastOne[14] = {
    0x85, 0xc0,                   // test eax, eax
    0x75, 0x05,                   // jnz +5
    0xb8, 0x01, 0x00, 0x00, 0x00, // mov eax, 1
    0x44, 0x8b, 0xe0,             // mov r12d, eax
    0xeb, 0x0b,                   // jmp 0x830bcc (the success path)
};
constexpr uintptr_t ERROR_NAMES = 0x1e1a830; // const char* [] indexed by the result codes

struct Sig {
  uintptr_t rva;
  unsigned char bytes[10];
};
constexpr Sig kSigs[] = {
    {GEN, {0x44, 0x88, 0x4c, 0x24, 0x20, 0x44, 0x88, 0x44, 0x24, 0x18}},
    {TAKE, {0x44, 0x88, 0x4c, 0x24, 0x20, 0x44, 0x88, 0x44, 0x24, 0x18}},
    {PARAMS_DTOR, {0x40, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x83, 0x79, 0x68}},
    {STRING_DTOR, {0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0x51, 0x08}},
    {FREE, {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10}},
    {FIND_COMPANY, {0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x7c, 0x24, 0x18}},
    {CARGO, {0x4c, 0x8b, 0xdc, 0x48, 0x81, 0xec, 0xb8, 0x00, 0x00, 0x00}},
    {TELEPORT, {0x44, 0x88, 0x4c, 0x24, 0x20, 0x44, 0x88, 0x44, 0x24, 0x18}},
    {CANCEL, {0x48, 0x89, 0x5c, 0x24, 0x18, 0x57, 0x48, 0x83, 0xec, 0x30}},
};

inline uintptr_t Base() { return reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr)); }
template <class T> T At(uintptr_t rva) { return reinterpret_cast<T>(Base() + rva); }

inline bool Supported() {
  for (const Sig& s : kSigs)
    if (std::memcmp(At<const void*>(s.rva), s.bytes, sizeof s.bytes) != 0) return false;
  return std::memcmp(At<const void*>(UNITS_CHECK), kUnitsOrig, sizeof kUnitsOrig) == 0;
}

inline void WriteCode(uintptr_t rva, const unsigned char* bytes, size_t n) {
  void* at = At<void*>(rva);
  DWORD old;
  VirtualProtect(at, n, PAGE_EXECUTE_READWRITE, &old);
  std::memcpy(at, bytes, n);
  VirtualProtect(at, n, old, &old);
  FlushInstructionCache(GetCurrentProcess(), at, n);
}

// Engine objects carry a "valid" bit in the top bit of the dword at +8.
inline bool Alive(const uint8_t* o) { return o && (*reinterpret_cast<const uint32_t*>(o + 8) >> 31); }
inline uint8_t* Ptr(const uint8_t* o, int off) { return *reinterpret_cast<uint8_t* const*>(o + off); }

inline uint8_t* Player() {
  const uint8_t* ctrl = *At<uint8_t**>(CTRL);
  return ctrl ? Ptr(ctrl, 0x18) : nullptr;
}

inline bool OnJob() {
  __try {
    const uint8_t* p = Player();
    return p && Alive(Ptr(p, 0x28));
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

inline const char* ErrorName(int code) {
  const char* s = nullptr;
  __try {
    s = At<const char* const*>(ERROR_NAMES)[code];
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  return s ? s : "?";
}

using VoidFn = void (*)(void*);

// The game's job params struct (0x70 bytes), initialised like `cheat get_job` does:
// +0x00 source company, +0x08 source city, +0x10/+0x18 target, +0x20 cargo (tokens).
inline void InitParams(uint8_t* p) {
  std::memset(p, 0, 0x100);
  *reinterpret_cast<uintptr_t*>(p + 0x30) = Base() + STRING_VTBL;
  *reinterpret_cast<uintptr_t*>(p + 0x38) = Base() + EMPTY_STR;
  *reinterpret_cast<uint16_t*>(p + 0x61) = 1;
  *reinterpret_cast<float*>(p + 0x64) = *At<const float*>(PARAMS_FLOAT);
}
inline void FreeParams(uint8_t* p) {
  At<VoidFn>(PARAMS_DTOR)(p);
  At<VoidFn>(STRING_DTOR)(p + 0x30);
}

// Cargo the game knows about (anything else fails with mp_job_missing_cargo).
inline bool CargoExists(uint64_t token) {
  __try {
    return Alive(At<uint8_t* (*)(uint64_t*)>(CARGO)(&token));
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// Returns false and fills `err` if the game refused. Any company pair works, not only the ones the
// freight market pairs up (get_job's 0x82e0b0 only ever picks one pair per city pair).
// Freight distance between two companies, in the economy's km. Income = fixed_revenue (600) +
// km * revenue_per_km_base (15) * revenue_coef_per_km (0.9) * cargo factors (def/economy_data.sii),
// and the generator copies params+0x64 (float km) into offer.shortest_distance_km (u16, 0x830afc).
// The game takes it from its navigation cache, which only knows the pairs the freight market
// offers, so we estimate it: straight line between the two companies' map items (bbox centres,
// [company+0x10] +0x0c min / +0x20 max) * the map scale (1 map metre = 19 m of "Europe") * a road
// factor. Returns -1 if a company can't be found.
// ponytail: straight line * 1.2 underestimates mountain/ferry routes; the game's own navigation
// (route search between the two companies) would be exact if this turns out too far off.
constexpr double MAP_SCALE = 19.0, ROAD_FACTOR = 1.2;

// Centre of a company's map item (world x, y, z); false if the game has no such company.
inline bool CompanyCenter(uint64_t company_tok, uint64_t city_tok, double out[3]) {
  using FindFn = uint8_t* (*)(uint64_t* company, uint64_t* city);
  __try {
    const uint8_t* company = At<FindFn>(FIND_COMPANY)(&company_tok, &city_tok);
    const uint8_t* item = Alive(company) ? Ptr(company, 0x10) : nullptr;
    if (!item) return false;
    const float* mn = reinterpret_cast<const float*>(item + 0x0c);
    const float* mx = reinterpret_cast<const float*>(item + 0x20);
    for (int i = 0; i < 3; ++i) out[i] = (mn[i] + mx[i]) / 2.0;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

inline double MetersToFreightKm(double metres) { return metres / 1000.0 * MAP_SCALE * ROAD_FACTOR; }

inline double FreightKm(uint64_t src_co, uint64_t src_city, uint64_t dst_co, uint64_t dst_city) {
  double a[3], b[3];
  if (!CompanyCenter(src_co, src_city, a) || !CompanyCenter(dst_co, dst_city, b)) return -1;
  return MetersToFreightKm(std::sqrt((b[0] - a[0]) * (b[0] - a[0]) + (b[2] - a[2]) * (b[2] - a[2])));
}

// `code` gets the game's result code (0 = ok; 14 = trailer spot occupied, worth retrying).
inline bool StartJob(uint64_t src_city, uint64_t dst_city, uint64_t src_co, uint64_t dst_co, uint64_t cargo, float km, char* err,
                     size_t err_size, int* code) {
  *code = -1;
  using GenFn = void (*)(void*, void*, bool, bool);
  using TakeFn = int (*)(void*, void*, bool, bool, bool);
  struct Result {
    uint8_t* offer;
    int status;
  };
  alignas(16) uint8_t p[0x100]; // the game's params struct is 0x70 bytes
  bool ok = false;
  __try {
    uint8_t* const player = Player();
    if (!player) {
      std::snprintf(err, err_size, "Jogo não está num mundo carregado.");
      return false;
    }
    // the player's first truck, and whether it has its own trailer (get_job passes this along)
    const uint64_t trucks = *reinterpret_cast<uint64_t*>(player + 0x80);
    if (!trucks) {
      std::snprintf(err, err_size, "Sem caminhão.");
      return false;
    }
    const uint8_t* truck = Ptr(Ptr(player, 0x78), 0);
    const bool own_trailer = Alive(truck) && Alive(Ptr(truck, 0x48));

    InitParams(p);
    uint64_t* q = reinterpret_cast<uint64_t*>(p);
    q[0] = src_co;
    q[1] = src_city;
    q[2] = dst_co;
    q[3] = dst_city;
    q[4] = cargo;
    // +0x64 = route length in km (see FreightKm). It starts at -1 (as in get_job) and generate
    // and take refuse a negative value with mp_job_missing_target_navigation.
    *reinterpret_cast<float*>(p + 0x64) = km < 0 ? 0.0f : std::min(km, 65000.0f);
    Result r{nullptr, 1};
    WriteCode(UNITS_CHECK, kUnitsAtLeastOne, sizeof kUnitsAtLeastOne);
    __try {
      At<GenFn>(GEN)(&r, p, true, own_trailer);
    } __finally {
      WriteCode(UNITS_CHECK, kUnitsOrig, sizeof kUnitsOrig);
    }
    if (r.status != 0) {
      std::snprintf(err, err_size, "Não deu para gerar o serviço: %s (%d)", ErrorName(r.status), r.status);
    } else {
      const int t = At<TakeFn>(TAKE)(p, r.offer, true, own_trailer, false);
      *code = t;
      if (t != 0) std::snprintf(err, err_size, "Não deu para assumir o serviço: %s (%d)", ErrorName(t), t);
      ok = t == 0;
    }
    if (r.offer) { // drop the generator's reference, exactly like get_job
      (*reinterpret_cast<VoidFn**>(r.offer))[1](r.offer);
      if ((InterlockedDecrement(reinterpret_cast<volatile LONG*>(r.offer + 8)) & 0x1ffffff) == 0) At<VoidFn>(FREE)(r.offer);
    }
    FreeParams(p);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    std::snprintf(err, err_size, "Exceção dentro do jogo (0x%08lX).", GetExceptionCode());
    return false;
  }
  return ok;
}

// Moves the player's truck into the source company's yard of the current job: 20 m in front of one
// of its trailer spots, facing the same way, so the job trailer (spawned when the player is near)
// ends up right behind it. Live RE (MODLOG v1.3): job = [[ctrl+0x18]+0x28]; [job+0x28] = company;
// [company+0x10] = its map item; item+0x70/+0x78 = array of trailer-spot map nodes (pos s32x3 in
// 1/256 m, quaternion w,x,y,z at +0x10). Teleport = 0x5ddf20(actor, placement*, 0, 0, 0) like
// company_portal does; placement = f32 x,y,z local + i16 sector x,z (world = local + sector*512),
// then quaternion w,x,y,z. Fills `where` with the chosen target (world x,y,z).
inline bool TeleportToTrailerSpot(float where[3]) {
  struct Spot {
    double x, y, z;
    float q[4];
  };
  struct Placement {
    float x, y, z;
    int16_t sx, sz;
    float q[4];
  };
  __try {
    const uint8_t* const player = Player();
    const uint8_t* job = player ? Ptr(player, 0x28) : nullptr;
    const uint8_t* company = Alive(job) ? Ptr(job, 0x28) : nullptr;
    const uint8_t* item = company ? Ptr(company, 0x10) : nullptr;
    uint8_t* const owner = *At<uint8_t**>(ACTOR_OWNER);
    uint8_t* const actor = owner ? Ptr(owner, 0x31b0) : nullptr;
    if (!item || !Alive(actor)) return false;
    const uint8_t* const* spots = *reinterpret_cast<const uint8_t* const* const*>(item + 0x70);
    const uint64_t n = *reinterpret_cast<const uint64_t*>(item + 0x78);
    if (!spots || n == 0 || n > 64) return false;
    double best_score = -1, target[3] = {};
    Spot best{};
    for (uint64_t i = 0; i < n; ++i) {
      Spot s;
      const int32_t* pos = reinterpret_cast<const int32_t*>(spots[i]);
      s.x = pos[0] / 256.0, s.y = pos[1] / 256.0, s.z = pos[2] / 256.0;
      std::memcpy(s.q, spots[i] + 0x10, sizeof s.q);
      // forward = the quaternion applied to -Z (SCS convention)
      const double w = s.q[0], x = s.q[1], y = s.q[2], z = s.q[3];
      const double fx = -(2 * (x * z + w * y)), fy = -(2 * (y * z - w * x)), fz = -(1 - 2 * (x * x + y * y));
      const double t[3] = {s.x + fx * 20.0, s.y + fy * 20.0, s.z + fz * 20.0};
      // keep the spot whose target point stays farthest from every spot (least likely to be blocked)
      double nearest = 1e18;
      for (uint64_t j = 0; j < n; ++j) {
        const int32_t* o = reinterpret_cast<const int32_t*>(spots[j]);
        const double dx = o[0] / 256.0 - t[0], dz = o[2] / 256.0 - t[2];
        nearest = std::min(nearest, dx * dx + dz * dz);
      }
      if (nearest > best_score) {
        best_score = nearest;
        best = s;
        std::memcpy(target, t, sizeof t);
      }
    }
    Placement pl{};
    pl.sx = static_cast<int16_t>(std::floor(target[0] / 512.0 + 0.5));
    pl.sz = static_cast<int16_t>(std::floor(target[2] / 512.0 + 0.5));
    pl.x = static_cast<float>(target[0] - pl.sx * 512.0);
    pl.y = static_cast<float>(target[1] + 0.5);
    pl.z = static_cast<float>(target[2] - pl.sz * 512.0);
    std::memcpy(pl.q, best.q, sizeof pl.q);
    where[0] = static_cast<float>(target[0]), where[1] = pl.y, where[2] = static_cast<float>(target[2]);
    return At<bool (*)(void*, void*, bool, bool, bool)>(TELEPORT)(actor, &pl, false, false, false);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// The teleport (0x5ddf20, at 0x5de9f8) does `actor+0x1c4 |= (g_park_brake_init != 0)` and, when that
// engages it, writes 1.0 to actor+0x3cc and +0x3d0: +0x1c4 is the truck's parking brake. Clearing
// them releases it, as if the player had pressed the parking brake key.
inline bool ReleaseParkingBrake() {
  __try {
    uint8_t* const owner = *At<uint8_t**>(ACTOR_OWNER);
    uint8_t* const actor = owner ? Ptr(owner, 0x31b0) : nullptr;
    if (!Alive(actor)) return false;
    actor[0x1c4] = 0;
    *reinterpret_cast<float*>(actor + 0x3cc) = 0.0f;
    *reinterpret_cast<float*>(actor + 0x3d0) = 0.0f;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// Fuel. The telemetry channel truck.fuel.amount (getter 0x64b630) is
//   [veh+0x190] (tank capacity, litres) * ([veh+0x1b8] (level, 0..1) + [truck+0x1158] (pending change)),
// with truck = [actor+0x18] (vtable rva 0x22f0260) and veh = [truck+0x1f8]. Filling the tank = level 1,
// pending 0. Returns the litres before, or a negative number when the truck was not found as expected.
constexpr uintptr_t TRUCK_VTBL = 0x22f0260;
inline float Refuel() {
  __try {
    uint8_t* const owner = *At<uint8_t**>(ACTOR_OWNER);
    uint8_t* const actor = owner ? Ptr(owner, 0x31b0) : nullptr;
    uint8_t* const truck = Alive(actor) ? Ptr(actor, 0x18) : nullptr;
    if (!truck || *reinterpret_cast<uintptr_t*>(truck) != Base() + TRUCK_VTBL) return -1.0f;
    uint8_t* const veh = Ptr(truck, 0x1f8);
    if (!veh) return -1.0f;
    const float capacity = *reinterpret_cast<const float*>(veh + 0x190);
    float* const level = reinterpret_cast<float*>(veh + 0x1b8);
    float* const pending = reinterpret_cast<float*>(truck + 0x1158);
    if (!(capacity > 20.0f && capacity < 5000.0f) || !(*level >= -0.01f && *level <= 1.01f)) return -1.0f; // not a fuel tank: hands off
    const float before = capacity * (*level + *pending);
    *level = 1.0f;
    *pending = 0.0f;
    return before;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return -1.0f;
  }
}

// The route adviser's message box (the one that says "Freio de mão acionado!"). The adviser is
// [actor+0x30]; its message queue (+0xd8, entries of 0x70 bytes) is fed by
//   0x623a80(queue, char** text, void* sub (exe+0x2732198 = ""), int kind, void* icon, int priority, int a, int b,
//            u16 c, void* object, {u8 has_key; i32 key}*)
// and a keyed message is taken down by 0x623970(queue, key). Values copied from the game's own callers:
// "car delivery ready" (0x68d4d0): kind 2, priority 2, a 0, b 2, c 0xffff; parking brake (0x68a5eb): kind 6
// (its icon), priority 2, a 1, b 0, c 7, key {1, 0}. Text takes the game's markup (<br>,
// <color value=@@clr_sel@@>, @@keys@@), UTF-8. Call from the game thread only.
// (0xa63920, tried first, is the Driving Academy's box: it only works in that mode.)
constexpr uintptr_t HINT_SHOW = 0x623a80, HINT_HIDE = 0x623970, HINT_SUB = 0x2732198;
constexpr unsigned char kHintSig[2][10] = {{0x44, 0x89, 0x4c, 0x24, 0x20, 0x4c, 0x89, 0x44, 0x24, 0x18},
                                           {0x4c, 0x6b, 0x41, 0x20, 0x70, 0x48, 0x8b, 0x41, 0x18, 0x4c}};
constexpr int HINT_KEY = 0x52504c; // ours ("RPL"); the game's own keys are small numbers
inline uint8_t* HintQueue() {
  if (std::memcmp(At<const void*>(HINT_SHOW), kHintSig[0], 10) != 0 || std::memcmp(At<const void*>(HINT_HIDE), kHintSig[1], 10) != 0) return nullptr;
  uint8_t* const owner = *At<uint8_t**>(ACTOR_OWNER);
  uint8_t* const actor = owner ? Ptr(owner, 0x31b0) : nullptr;
  uint8_t* const adviser = Alive(actor) ? Ptr(actor, 0x30) : nullptr;
  return adviser ? adviser + 0xd8 : nullptr;
}
// 0 = sent; 1 game not recognised or no adviser, 9 exception.
inline int ShowHint(const char* text) {
  struct Key {
    uint8_t has;
    int32_t key;
  };
  using Fn = void (*)(void*, const char**, void*, int, void*, int, int, int, uint16_t, void*, const Key*);
  __try {
    uint8_t* const queue = HintQueue();
    if (!queue) return 1;
    const Key key{1, HINT_KEY};
    At<Fn>(HINT_SHOW)(queue, &text, At<void*>(HINT_SUB), 2, At<void*>(HINT_SUB), 2, 0, 2, 0xffff, nullptr, &key);
    return 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return 9;
  }
}
inline bool HideHint() {
  __try {
    uint8_t* const queue = HintQueue();
    if (!queue) return false;
    At<void (*)(void*, int)>(HINT_HIDE)(queue, HINT_KEY);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// EXPERIMENT: a window of the game's own UI, built from a script of ours. The game's screens are
// SiiNunit scripts (/ui/*.sii: ui::window, ui::text_common, ui::button_common... with look templates) and
// are created, shown and closed like this (adviser options, 0x68cda0):
//   fs = 0x1536e0(4); fs->vt[4](fs, char** path)                       does the file exist
//   0x374f20(void** tmp, char** name, char** layer ("hud"), 0x100, char** path, u8 0x80)   build the window
//   0x33cb90(void** slot, void** tmp)                                  take ownership
//   0x38c080([exe+0x36ae6f8] (UI manager), window, 0)                  show
//   0x38bbc0(UI manager, window); 0x108650(void** slot)                close and let go
// The user's game folder (Documents) is mounted as /home, so the script needs no mod.
constexpr uintptr_t UI_MANAGER = 0x36ae6f8, UI_FS = 0x1536e0, UI_CREATE = 0x374f20, UI_ASSIGN = 0x33cb90, UI_SHOW = 0x38c080, UI_REMOVE = 0x38bbc0,
                    UI_RELEASE = 0x108650;
constexpr Sig kUiSigs[] = {
    {UI_FS, {0x48, 0x83, 0xec, 0x48, 0x48, 0x63, 0xd1, 0x48, 0x3b, 0x15}},     {UI_CREATE, {0x4c, 0x8b, 0xdc, 0x49, 0x89, 0x5b, 0x18, 0x49, 0x89, 0x73}},
    {UI_ASSIGN, {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10}}, {UI_SHOW, {0x48, 0x89, 0x54, 0x24, 0x10, 0x55, 0x56, 0x41, 0x56, 0x48}},
    {UI_REMOVE, {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x54, 0x24, 0x10}}, {UI_RELEASE, {0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20}},
};
// 0 = shown; 1 game not recognised, 2 no UI manager, 3 script not found by the game, 4 the game built nothing, 9 exception.
inline int OpenGameWindow(void** slot, const char* name, const char* path) {
  for (const Sig& s : kUiSigs)
    if (std::memcmp(At<const void*>(s.rva), s.bytes, sizeof s.bytes) != 0) return 1;
  __try {
    void* const mgr = *At<void**>(UI_MANAGER);
    if (!mgr) return 2;
    void* const fs = At<void* (*)(int)>(UI_FS)(4);
    if (!fs || !(*reinterpret_cast<bool (***)(void*, const char**)>(fs))[4](fs, &path)) return 3;
    void* tmp = nullptr;
    const char* layer = "hud";
    At<void** (*)(void**, const char**, const char**, uint32_t, const char**, uint8_t)>(UI_CREATE)(&tmp, &name, &layer, 0x100, &path, 0x80);
    if (!tmp) return 4;
    At<void** (*)(void**, void**)>(UI_ASSIGN)(slot, &tmp);
    At<void (*)(void*, void*, int)>(UI_SHOW)(mgr, *slot, 0);
    return 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return 9;
  }
}
// The game's own cursor only exists while the game is paused for a screen (driving, the mouse belongs to
// the camera). The game's message screens do it with 0x68ca20(adviser) / 0x68bb60(), but those also
// switch the camera off ([exe+0x36ae740]+0x28) and tell the HUD to change mode: with a small window of
// ours the world went black. This is the same sequence without those two parts:
//   pause : input = uimgr+0x3b0; input->vt[33](&0, &2); 0x38a770(uimgr);
//           G = [exe+0x36ae718]; ++G[0xacc]; ++G[0xac8]; ++G[0xac0];
//           first pause: G.b[0x1112] = 1 + 0x428b20(G) if it was not; G.b[0x13e0] = 0; 0x428240(G);
//           0x441f60([exe+0x36ae6d0], 2)
//   resume: the counters back; last resume: G.b[0x1112] = 0 + 0x428b20(G) if set; G.b[0x13e0] = 0;
//           0x10aeb0(G+0x13b8); 0x58c1e0(G+0x13b8, 0); 0x428240(G);
//           input->vt[21](&{0, -1}); 0x38a770(uimgr); 0x441f60([exe+0x36ae6d0], 1) unless [owner+0x35b8]
// Every pause needs its resume (they are counters). G+0xac4 is NOT touched: only the game's full-screen
// screens raise it, and with it the world is not drawn (black behind our window); a plain pause
// (0xa05aa0) leaves it alone.
constexpr uintptr_t GAME_STATE = 0x36ae718, PAUSE_MODE_OBJ = 0x36ae6d0, PAUSE_NOTIFY = 0x428b20, PAUSE_APPLY = 0x428240, PAUSE_TIMER_A = 0x10aeb0,
                    PAUSE_TIMER_B = 0x58c1e0, PAUSE_MODE = 0x441f60, UI_INPUT_REFRESH = 0x38a770;
constexpr Sig kPauseSigs[] = {
    {PAUSE_NOTIFY, {0x40, 0x53, 0x48, 0x83, 0xec, 0x50, 0x48, 0x8d, 0x44, 0x24}},  {PAUSE_APPLY, {0x48, 0x83, 0xec, 0x28, 0x48, 0x8b, 0x15, 0xdd, 0x95, 0x25}},
    {PAUSE_TIMER_A, {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10}}, {PAUSE_TIMER_B, {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10}},
    {PAUSE_MODE, {0x48, 0x89, 0x6c, 0x24, 0x20, 0x56, 0x48, 0x83, 0xec, 0x50}},    {UI_INPUT_REFRESH, {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10}},
};
inline bool PauseForUi(bool pause) {
  for (const Sig& s : kPauseSigs)
    if (std::memcmp(At<const void*>(s.rva), s.bytes, sizeof s.bytes) != 0) return false;
  using Fn = void (*)(void*);
  __try {
    uint8_t* const owner = *At<uint8_t**>(ACTOR_OWNER);
    uint8_t* const g = *At<uint8_t**>(GAME_STATE);
    uint8_t* const mgr = *At<uint8_t**>(UI_MANAGER);
    void* const mode = *At<void**>(PAUSE_MODE_OBJ);
    if (!owner || !g || !mgr || !mode) return false;
    uint8_t* const input = mgr + 0x3b0;
    void** const vt = *reinterpret_cast<void***>(input);
    int32_t* const count = reinterpret_cast<int32_t*>(g + 0xac0); // [0] paused, [1] +0xac4, [2] +0xac8, [3] +0xacc
    if (pause) {
      int32_t key = 0;
      int64_t value = 2;
      reinterpret_cast<void (*)(void*, int32_t*, int64_t*)>(vt[33])(input, &key, &value);
      At<Fn>(UI_INPUT_REFRESH)(mgr);
      ++count[3], ++count[2], ++count[0];
      if (count[0] == 1) {
        if (g[0x1112] != 1) {
          g[0x1112] = 1;
          At<Fn>(PAUSE_NOTIFY)(g);
        }
        g[0x13e0] = 0;
        At<Fn>(PAUSE_APPLY)(g);
      }
      At<void (*)(void*, int)>(PAUSE_MODE)(mode, 2);
    } else {
      --count[3], --count[2];
      if (--count[0] == 0) {
        if (g[0x1112] != 0) {
          g[0x1112] = 0;
          At<Fn>(PAUSE_NOTIFY)(g);
        }
        g[0x13e0] = 0;
        At<Fn>(PAUSE_TIMER_A)(g + 0x13b8);
        At<void (*)(void*, int)>(PAUSE_TIMER_B)(g + 0x13b8, 0);
        At<Fn>(PAUSE_APPLY)(g);
      }
      struct {
        int32_t key, pad;
        int64_t value;
      } reset{0, 0, -1};
      reinterpret_cast<void (*)(void*, void*)>(vt[21])(input, &reset);
      At<Fn>(UI_INPUT_REFRESH)(mgr);
      if (*reinterpret_cast<int32_t*>(owner + 0x35b8) == 0) At<void (*)(void*, int)>(PAUSE_MODE)(mode, 1);
    }
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// A widget of a game window by its `id:` in the script (what the game's 0x385e20 does): children are
// the array at +0x70 (count +0x78), the id is the u32 at +0x14, containers have bit 7 of +0x60.
inline uint8_t* FindWidget(uint8_t* w, uint32_t id, int depth = 0) {
  if (!w || depth > 8) return nullptr;
  uint8_t** kids = *reinterpret_cast<uint8_t***>(w + 0x70);
  const uint64_t n = *reinterpret_cast<uint64_t*>(w + 0x78);
  for (uint64_t i = 0; kids && i < n && i < 512; ++i) {
    uint8_t* k = kids[i];
    if (!k) continue;
    if (*reinterpret_cast<uint32_t*>(k + 0x14) == id) return k;
    if (*reinterpret_cast<uint32_t*>(k + 0x60) & 0x80)
      if (uint8_t* found = FindWidget(k, id, depth + 1)) return found;
  }
  return nullptr;
}
// State flags (+0x60) of the widget `id` of `window`; bit 24 is set while the pointer is over it
// (seen live on a button). 0 if the widget is not there.
inline uint32_t WidgetFlags(void* window, uint32_t id) {
  __try {
    uint8_t* const w = FindWidget(static_cast<uint8_t*>(window), id);
    return w ? *reinterpret_cast<uint32_t*>(w + 0x60) : 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return 0;
  }
}

// The game's map widgets (ui_world_map, ui_job_map, ui_map) keep their zoom level at +0x1e8. A new
// widget has 8 there, one past the last level, and drawing it like that takes the game down
// ("Index outside array boundaries: 8 >= 8" in the country names, crash of 2026-10-06): the game's own
// screens set a level first, with 0x10005c0(map, level) (also fills the scale at +0x1e0 from the table
// of levels at [[exe+0x36ae6d8]+0x98]+0x10 and refreshes the widget). Higher = closer.
// The levels are the zoom_uplift[] of /def/map_data.sii: 0-1 the 3D map, 2-3 the minimap, and for the
// world map 4 (city detail), 5 (closest), 6 (middle), 7 (whole world): higher = farther. Below 4 a world
// map is so close that it looks empty.
// What a map draws (icons of companies, services, road numbers, city names...) is the set of flags at
// +0x888. A new widget has all of them on (0xFFFFFFFF: every icon of Europe at once); the game keeps one
// set per zoom level in the table at [[exe+0x36ae6d8]+0x98]+0x178 (read live: level 6 = 0x685407, level
// 7 = 0x481402) and applies it with 0x10017d0(map, flags). Neither 0x10005c0 nor the widget's own wheel
// zoom does that (the screens' handlers do), so it is done here; `rebuild` then has the map collect its
// content again (not needed on a widget that is still to be set up). NOT by calling 0xffee60(map, 3)
// ourselves, as v3.2 did: the game crashed now and then while the wheel was turned. The widget's own
// update (0x100f240) only calls it when the widget is ready ([map+0x81c]) and no collection is still
// running ([map+0xdd0] == 0), with the bits pending at +0x7c8 (3 = everything, what the game's code
// writes there). So the bits are set and the widget does it when it can.
// The set used is the one of the next level farther out: the icons come in one level of zoom later than
// on the game's own map, which is what the user asked for on a map this small.
// Returns the level set (clamped to the table), or -1 if the widget or the function is not there.
constexpr uintptr_t MAP_SET_ZOOM = 0x10005c0, MAP_SET_FLAGS = 0x10017d0;
constexpr unsigned char kMapZoomSig[10] = {0x48, 0x83, 0xec, 0x48, 0x4c, 0x8b, 0xc9, 0x3b, 0x91, 0xe8};
constexpr unsigned char kMapFlagsSig[10] = {0x48, 0x8b, 0x05, 0x71, 0xcf, 0x6a, 0x02, 0x89, 0x91, 0x88};
// (no checks of its own: for the two functions below) Returns the map's level, -1 if it has no set of flags.
inline int MapIcons(uint8_t* map, uint8_t* data, bool rebuild) {
  const int level = *reinterpret_cast<const int*>(map + 0x1e8);
  const uint32_t* const presets = reinterpret_cast<const uint32_t*>(Ptr(data + 0x178, 8));
  const uint64_t count = *reinterpret_cast<const uint64_t*>(data + 0x178 + 0x10);
  if (!presets || level < 0 || static_cast<uint64_t>(level) >= count) return -1;
  const uint32_t want = presets[static_cast<uint64_t>(level) + 1 < count ? level + 1 : level];
  if (*reinterpret_cast<const uint32_t*>(map + 0x888) != want) {
    At<void (*)(void*, uint32_t)>(MAP_SET_FLAGS)(map, want);
    if (rebuild) *reinterpret_cast<uint32_t*>(map + 0x7c8) |= 3;
  }
  return level;
}
inline int SetMapZoom(void* window, uint32_t id, int level, bool rebuild = false) {
  if (std::memcmp(At<const void*>(MAP_SET_ZOOM), kMapZoomSig, sizeof kMapZoomSig) != 0 || std::memcmp(At<const void*>(MAP_SET_FLAGS), kMapFlagsSig, sizeof kMapFlagsSig) != 0) return -1;
  __try {
    uint8_t* const map = FindWidget(static_cast<uint8_t*>(window), id);
    uint8_t* const owner = *At<uint8_t**>(ACTOR_OWNER);
    uint8_t* const data = owner ? Ptr(owner, 0x98) : nullptr;
    if (!map || !data) return -1;
    const int levels = static_cast<int>(*reinterpret_cast<uint64_t*>(data + 0x10 + 0x10));
    if (levels < 1 || levels > 64) return -1;
    level = level < 0 ? 0 : level >= levels ? levels - 1 : level;
    At<void (*)(void*, int)>(MAP_SET_ZOOM)(map, level);
    // 0x10005c0 only sets the level and its height (+0x1e0): icons and names change size, the view does
    // not (the zoom buttons of v4.0). What the map looks from is the height at +0x1ac (the y of the
    // position at +0x1a8), which 0x1000b00 copies from +0x1e0 when it places the map: the same here.
    *reinterpret_cast<float*>(map + 0x1ac) = *reinterpret_cast<const float*>(map + 0x1e0);
    MapIcons(map, data, rebuild);
    return level;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return -1;
  }
}
// Every frame while a map of ours is on screen: the wheel changes the level without touching the icons.
// Only acts when the level changed (never twice for the same one, whatever the game does with the flags).
// Returns the map's level, or -1.
inline int KeepMapIcons(void* window, uint32_t id) {
  static const uint8_t* last_map = nullptr;
  static int last_level = -1;
  if (std::memcmp(At<const void*>(MAP_SET_FLAGS), kMapFlagsSig, sizeof kMapFlagsSig) != 0) return -1;
  __try {
    uint8_t* const map = FindWidget(static_cast<uint8_t*>(window), id);
    uint8_t* const owner = *At<uint8_t**>(ACTOR_OWNER);
    uint8_t* const data = owner ? Ptr(owner, 0x98) : nullptr;
    if (!map || !data) return -1;
    const int level = *reinterpret_cast<const int*>(map + 0x1e8);
    if (map == last_map && level == last_level) return level;
    last_map = map, last_level = level;
    return MapIcons(map, data, true);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return -1;
  }
}

// The game's GPS. What `cheat company_portal` does after teleporting (0x5c9fe9):
//   0x7b47b0(game, target*, company map item, 0, 0)   builds a 24-byte navigation target (first dword 2 = none)
//   0x4fad00(game+0x4128, 5, array{vtbl, data, size, capacity}*)   replaces the GPS waypoints (copies the array)
// with game = [exe+0x36ae6d8]. The game only allows it while [game+0x42f0] is 0, 2, 3, 4 or 5 (1, 6, 7 =
// "Unable to override gps while on job"). The maps draw the route the GPS is on, so this is how a route
// is previewed: waypoints = the companies, in order (the route starts at the truck).
// Returns how many waypoints were set; 0 clears the GPS; -1 = refused or not possible.
// A waypoint is a route_task_node_t (24 bytes; first dword 2 = none). `via`, when given, is the whole
// route as the map widget holds it after the player changed it (MapClickWaypoint below: the stops with
// the player's points among them), and goes to the GPS as it is instead of the stops.
// [game+0x42f0] is the navigation's mode, nav+0x1c8: 0 nothing, 5 free waypoints, 1/6/7 a job. On a job
// the waypoints at nav+0x1f8 (data +0x200, count +0x208) end with the job's own target, and the player's
// points go before it with the mode kept: what the map does when the player adds one (0x1013bd3).
struct NavNode {
  uint64_t kind, what;
  uint32_t extra, pad;
};
static_assert(sizeof(NavNode) == 24);
constexpr int kMaxVia = 10; // the game's own limit (0x1013114)
constexpr uintptr_t NAV_TARGET = 0x7b47b0, NAV_SET = 0x4fad00, NAV_ARRAY_VTBL = 0x21fafa8;
constexpr Sig kNavSigs[] = {{NAV_TARGET, {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10}}, {NAV_SET, {0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18}}};
inline int SetGpsRoute(const uint64_t (*stops)[2], int count, const NavNode* via = nullptr, int via_count = 0) { // stops[i] = {company token, city token}
  struct Array {
    uintptr_t vtbl;
    void* data;
    uint64_t size, capacity;
  };
  for (const Sig& s : kNavSigs)
    if (std::memcmp(At<const void*>(s.rva), s.bytes, sizeof s.bytes) != 0) return -1;
  if (count < 0 || count > 4 || via_count < 0 || via_count > kMaxVia) return -1;
  __try {
    uint8_t* const game = *At<uint8_t**>(ACTOR_OWNER);
    if (!game) return -1;
    const int state = *reinterpret_cast<const int*>(game + 0x42f0);
    if (state < 0 || state > 7 || state == 1 || state == 6 || state == 7) return -1;
    alignas(16) uint8_t targets[(4 + kMaxVia) * 24 + 16] = {};
    int n = 0;
    for (int i = 0; i < count && !via_count; ++i) {
      uint64_t company_tok = stops[i][0], city_tok = stops[i][1];
      const uint8_t* company = At<uint8_t* (*)(uint64_t*, uint64_t*)>(FIND_COMPANY)(&company_tok, &city_tok);
      uint8_t* const item = Alive(company) ? Ptr(company, 0x10) : nullptr;
      if (!item) continue;
      alignas(16) uint8_t one[32] = {};
      At<void (*)(void*, void*, void*, uint64_t, bool)>(NAV_TARGET)(game, one, item, 0, false);
      if (*reinterpret_cast<const int*>(one) == 2) continue; // no navigation point for that company
      std::memcpy(targets + n * 24, one, 24);
      ++n;
    }
    if (via_count) {
      std::memcpy(targets, via, via_count * 24);
      n = via_count;
    }
    if (count > 0 && n == 0) return -1;
    Array array{Base() + NAV_ARRAY_VTBL, n ? targets : nullptr, static_cast<uint64_t>(n), static_cast<uint64_t>(n)};
    At<void (*)(void*, int, void*)>(NAV_SET)(game + 0x4128, 5, &array);
    return n;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return -1;
  }
}

// The waypoint the GPS gets for a company (what SetGpsRoute sends for a stop), to tell the stops from the
// player's points in the map's list.
inline bool CompanyNode(uint64_t company_tok, uint64_t city_tok, NavNode* out) {
  for (const Sig& s : kNavSigs)
    if (std::memcmp(At<const void*>(s.rva), s.bytes, sizeof s.bytes) != 0) return false;
  __try {
    uint8_t* const game = *At<uint8_t**>(ACTOR_OWNER);
    const uint8_t* company = game ? At<uint8_t* (*)(uint64_t*, uint64_t*)>(FIND_COMPANY)(&company_tok, &city_tok) : nullptr;
    uint8_t* const item = Alive(company) ? Ptr(company, 0x10) : nullptr;
    if (!item) return false;
    alignas(16) uint8_t one[32] = {};
    At<void (*)(void*, void*, void*, uint64_t, bool)>(NAV_TARGET)(game, one, item, 0, false);
    std::memcpy(out, one, sizeof *out);
    return static_cast<uint32_t>(out->kind) != 2;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}
// The navigation's mode and how many waypoints it holds (for the log).
inline bool NavState(int* mode, uint64_t* count) {
  __try {
    const uint8_t* const game = *At<uint8_t**>(ACTOR_OWNER);
    if (!game) return false;
    *mode = *reinterpret_cast<const int*>(game + 0x4128 + 0x1c8);
    *count = *reinterpret_cast<const uint64_t*>(game + 0x4128 + 0x208);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// The player's points on a running job: 1 = put before the job's target, 0 = not now (no destination in
// the GPS, or it holds more than the one target already), -1 = not possible. The modes are the ones in
// which the map itself keeps the last target (0x1013b6e: 1-4, 6, 7).
inline int ApplyJobWaypoints(const NavNode* via, int via_count) {
  struct Array {
    uintptr_t vtbl;
    void* data;
    uint64_t size, capacity;
  };
  for (const Sig& s : kNavSigs)
    if (std::memcmp(At<const void*>(s.rva), s.bytes, sizeof s.bytes) != 0) return -1;
  if (via_count < 1 || via_count > kMaxVia) return -1;
  __try {
    uint8_t* const game = *At<uint8_t**>(ACTOR_OWNER);
    if (!game) return -1;
    uint8_t* const nav = game + 0x4128;
    const int mode = *reinterpret_cast<const int*>(nav + 0x1c8);
    const NavNode* const now = *reinterpret_cast<const NavNode* const*>(nav + 0x200);
    if (mode < 1 || mode > 7 || mode == 5 || *reinterpret_cast<const uint64_t*>(nav + 0x208) != 1 || !now) return 0;
    alignas(16) NavNode nodes[kMaxVia + 1];
    std::memcpy(nodes, via, via_count * sizeof(NavNode));
    nodes[via_count] = now[0];
    Array array{Base() + NAV_ARRAY_VTBL, nodes, static_cast<uint64_t>(via_count) + 1, static_cast<uint64_t>(via_count) + 1};
    At<void (*)(void*, int, void*)>(NAV_SET)(nav, mode, &array);
    return 1;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return -1;
  }
}

// The map widget keeps the player's own waypoints itself: array<route_task_node_t> at +0x8c0 (data +0x8c8,
// count +0x8d0, at most 10), and puts them into the navigation whenever they change (vt[0x1d8] = 0x1013b00).
// What the game's map screen does on its "add waypoint" key (shortcut_1, in 0x10ab418), for the place
// under the pointer (the widget follows the pointer by itself: +0xd44/+0xd48, hovered +0xd10/+0xd18):
//   index = [map+0xd18] != -1 ? [map+0xd18] : [map+0xd10];  node = {2, 0, 2};  kind = 0
//   on_one = 0x10112a0(map, &index, &node, &kind)      a waypoint of the list is under the pointer
//   on_one:  0x1012c30(map, index, &node, kind); 0x1012eb0(map)       picks it up and takes it out
//   else, if node is a place (kind != 2, !0x100b8a0(&node)) and there is room:
//            [map+0xae8] = 0 (1 = a place to avoid); 0x10130e0(map, index, &node)   in at its place along the route
// The screen's handler first asks vt[0x1b8] and 0x1012f30 (not while dragging: bits 0-1 of +0x8a4, nothing
// picked up: [map+0xcf0] == -1); the same is checked here. It also starts with 0x10127e0(map, x, y), the
// pointer's place (what a mouse move does: finds what is under it); done here with the place the widget
// last heard of, because without it most clicks found nothing (first test, 2026-10-06).
// On attaching the navigation the widget takes the GPS's waypoints into its list: with our route in the
// GPS the list is origin, destination, and what the player adds goes between them.
// The widget does all of this by itself on a click (its own handling of the pointer's "select"), so the
// plugin only reads the list.
// The widget's waypoints, in route order. Returns how many (at most kMaxVia), -1 if the widget is not there.
inline int MapWaypoints(void* window, uint32_t id, NavNode* out) {
  __try {
    const uint8_t* const map = FindWidget(static_cast<uint8_t*>(window), id);
    if (!map) return -1;
    const NavNode* const data = *reinterpret_cast<const NavNode* const*>(map + 0x8c8);
    const uint64_t count = *reinterpret_cast<const uint64_t*>(map + 0x8d0);
    if (count > kMaxVia || (count && !data)) return -1;
    std::memcpy(out, data, count * sizeof(NavNode));
    return static_cast<int>(count);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return -1;
  }
}
// The names on the map (cities, countries) are markup the widget writes itself (0x100ca87):
// "<font face=/font/%s.font xscale=%g yscale=%g>" with the font's name in the string at +0x230 (data
// +0x238, length +0x240) and the size times the float at +0x250 (1.0). A new widget has "map", a font
// that does not exist (hence the plain fallback letters); the game's screens give theirs "normal_o", the
// outlined one (0x10a71b6, 0x105f3ad), with the string's assign 0xf4a40(string, 0, {text, 0, length}*).
// The same here, and the size on top.
constexpr uintptr_t STRING_ASSIGN = 0xf4a40, EMPTY_TEXT = 0x1df110e;
constexpr unsigned char kStringAssignSig[10] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10};
inline bool SetMapNames(void* window, uint32_t id, const char* font, float scale) {
  struct Piece {
    const char* text;
    uint64_t zero, length;
  };
  if (std::memcmp(At<const void*>(STRING_ASSIGN), kStringAssignSig, sizeof kStringAssignSig) != 0) return false;
  __try {
    uint8_t* const map = FindWidget(static_cast<uint8_t*>(window), id);
    if (!map) return false;
    uint8_t* const name = map + 0x230;
    char* const data = *reinterpret_cast<char**>(name + 8);
    if (!data) return false;
    if (data != At<char*>(EMPTY_TEXT)) {
      data[0] = 0;
      *reinterpret_cast<uint32_t*>(name + 0x10) = 0;
    }
    Piece piece{font, 0, std::strlen(font)};
    At<void (*)(void*, uint32_t, Piece*)>(STRING_ASSIGN)(name, 0, &piece);
    *reinterpret_cast<float*>(map + 0x250) = scale;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}
// Where the map is looking: world x, y (height), z as floats at +0x1a8. (+0x1b4 is only where it was
// told to look by 0x1000b00; reading that brought a rebuilt map back to the middle of the route, test of
// 2026-10-06.) Dragging adds each move to +0x8b0 (x) and +0x8b8 (z) in the mouse handler (0x10127e0), and
// the widget's update (0x100f90e) folds that into +0x1a8 and zeroes it: both are summed here.
inline bool MapViewCenter(void* window, uint32_t id, double out_xz[2]) {
  __try {
    const uint8_t* const map = FindWidget(static_cast<uint8_t*>(window), id);
    if (!map) return false;
    const float* const at = reinterpret_cast<const float*>(map + 0x1a8);
    const float* const drag = reinterpret_cast<const float*>(map + 0x8b0);
    out_xz[0] = static_cast<double>(at[0]) + drag[0], out_xz[1] = static_cast<double>(at[2]) + drag[2];
    return std::isfinite(out_xz[0]) && std::isfinite(out_xz[1]);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// The route a map draws is its own list of items at +0x160 (data +0x168, count +0x170, 0xb0 bytes each),
// in order from the truck: [item] = 0 a waypoint (its route_task_node_t at +0x18), 1 a stretch of road,
// anything else is skipped by the drawing code (0x1014b10: only 0 and 1 are handled). The GPS always
// starts at the truck, so the list begins with the way from the truck to the first waypoint; for a
// preview of a job that begins at the origin company that stretch is noise (the planner teleports
// there). Its road items are turned into a kind nobody draws. Only when the first waypoint is `origin`:
// with the truck already there the game has dropped that waypoint and the list starts on the job's way.
// The widget makes the list again when it collects its content (after a drag, a zoom), so this is
// called every frame. Returns how many items it hid now, -1 if the list does not look right.
inline int HideRouteToOrigin(void* window, uint32_t id, const NavNode& origin) {
  constexpr size_t kItem = 0xb0;
  __try {
    uint8_t* const map = FindWidget(static_cast<uint8_t*>(window), id);
    if (!map) return -1;
    uint8_t* const data = *reinterpret_cast<uint8_t**>(map + 0x168);
    const uint64_t count = *reinterpret_cast<const uint64_t*>(map + 0x170);
    if (!count) return 0;
    if (!data || count > 200000) return -1;
    uint64_t first = 0;
    while (first < count && *reinterpret_cast<const uint32_t*>(data + first * kItem) != 0) ++first;
    if (first == count) return 0;
    const uint8_t* const node = data + first * kItem + 0x18;
    if (*reinterpret_cast<const uint32_t*>(node) != static_cast<uint32_t>(origin.kind) || *reinterpret_cast<const uint64_t*>(node + 8) != origin.what) return 0;
    int hidden = 0;
    for (uint64_t i = 0; i < first; ++i) {
      uint32_t* const kind = reinterpret_cast<uint32_t*>(data + i * kItem);
      if (*kind == 1) *kind = 3, ++hidden;
    }
    return hidden;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return -1;
  }
}

// A map widget only draws the GPS route when it has been told where the navigation is: the game's own
// screens hand it the navigation object, game+0x4128 (the same one the GPS waypoints go to), right
// after creating it. From the set-up of the game's maps (0x54a2a9, and the job offer's at 0x105f426):
//   [map+0xb0] = nav;  0x100e9b0(map, 0.0f);  0x10012e0(map, true);  0x10009d0(map, nav);  0x1000a60(map);
// 0x10009d0 also stores nav at +0xb8 and resets the route the widget holds (+0x540, +0x568, +0x6d8...).
// In a widget of ours +0xb8 was null (compared with the real map's memory, 2026-10-06).
constexpr uintptr_t MAP_VIEW = 0x100e9b0, MAP_FOLLOW = 0x10012e0, MAP_SET_NAV = 0x10009d0, MAP_PREPARE = 0x1000a60;
constexpr Sig kMapNavSigs[] = {
    {MAP_VIEW, {0x40, 0x57, 0x48, 0x83, 0xec, 0x70, 0x48, 0x8b, 0x81, 0x40}},    {MAP_FOLLOW, {0x40, 0x55, 0x53, 0x57, 0x48, 0x8d, 0x6c, 0x24, 0xb9, 0x48}},
    {MAP_SET_NAV, {0x48, 0x83, 0xec, 0x48, 0x33, 0xc0, 0x48, 0xc7, 0x44, 0x24}}, {MAP_PREPARE, {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10}},
};
inline bool AttachMapNavigation(void* window, uint32_t id) {
  for (const Sig& s : kMapNavSigs)
    if (std::memcmp(At<const void*>(s.rva), s.bytes, sizeof s.bytes) != 0) return false;
  __try {
    uint8_t* const map = FindWidget(static_cast<uint8_t*>(window), id);
    uint8_t* const game = *At<uint8_t**>(ACTOR_OWNER);
    if (!map || !game) return false;
    uint8_t* const nav = game + 0x4128;
    *reinterpret_cast<uint8_t**>(map + 0xb0) = nav;
    At<void (*)(void*, float)>(MAP_VIEW)(map, 0.0f);
    At<void (*)(void*, bool)>(MAP_FOLLOW)(map, true);
    At<void (*)(void*, void*)>(MAP_SET_NAV)(map, nav);
    At<void (*)(void*)>(MAP_PREPARE)(map);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// Where the player is and what the map looks at: 0x1000b00(map, placement*, bool, focus*, null), what the
// HUD does every frame with the truck's placement (0x689e3e) and the job offer's map with its own points.
// placement = {f32 x, y, z in the sector; i16 sector x, z; quaternion w, x, y, z} (as for the teleport);
// it becomes the player's marker (+0x258 position, +0x268 rotation). focus (optional, 16 bytes, same
// position format) is what the view centres on (+0x1b4), the placement itself when null. The call also
// puts the scale of the current zoom level into effect (+0x1ac = +0x1e0): without it a new widget
// shows the player at the world's origin and a far too close view until the wheel is turned.
constexpr uintptr_t MAP_PLACE = 0x1000b00;
constexpr unsigned char kMapPlaceSig[10] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10};
inline bool SetMapPlacement(void* window, uint32_t id, double x, double y, double z, double heading_turns, const double* focus_xz) {
  struct Position {
    float x, y, z;
    int16_t sx, sz;
  };
  struct Placement {
    Position p;
    float q[4]; // w, x, y, z
  };
  if (std::memcmp(At<const void*>(MAP_PLACE), kMapPlaceSig, sizeof kMapPlaceSig) != 0) return false;
  const auto position = [](double wx, double wy, double wz) {
    Position p{};
    p.sx = static_cast<int16_t>(std::floor(wx / 512.0));
    p.sz = static_cast<int16_t>(std::floor(wz / 512.0));
    p.x = static_cast<float>(wx - p.sx * 512.0);
    p.y = static_cast<float>(wy);
    p.z = static_cast<float>(wz - p.sz * 512.0);
    return p;
  };
  Placement at{position(x, y, z), {}};
  const double half = heading_turns * 6.283185307179586 / 2; // rotation about +Y
  at.q[0] = static_cast<float>(std::cos(half));
  at.q[2] = static_cast<float>(std::sin(half));
  Position focus = focus_xz ? position(focus_xz[0], y, focus_xz[1]) : at.p;
  __try {
    uint8_t* const map = FindWidget(static_cast<uint8_t*>(window), id);
    if (!map) return false;
    At<void (*)(void*, void*, bool, void*, void*)>(MAP_PLACE)(map, &at, false, focus_xz ? &focus : nullptr, nullptr);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

inline bool CloseGameWindow(void** slot) {
  __try {
    void* const mgr = *At<void**>(UI_MANAGER);
    if (!mgr || !*slot) return false;
    At<void (*)(void*, void*)>(UI_REMOVE)(mgr, *slot);
    At<void (*)(void**)>(UI_RELEASE)(slot);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    *slot = nullptr;
    return false;
  }
}

inline bool CancelJob() {
  __try {
    uint8_t* const ctrl = *At<uint8_t**>(CTRL);
    if (!ctrl || !OnJob()) return false;
    At<void (*)(void*)>(CANCEL)(ctrl);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

} // namespace game
