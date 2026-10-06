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
// the camera). The pair the game uses for its message screens (0xa63a8c, 0xa05974 -> 0x9c1160):
//   0x68ca20(adviser = [actor+0x30])   hides the adviser panels, pauses the simulation, hands the mouse to the UI
//   0x68bb60()                         undoes it
// They keep counters ([exe+0x36ae718]+0xac0..0xacc), so every pause needs its resume.
constexpr uintptr_t UI_PAUSE = 0x68ca20, UI_RESUME = 0x68bb60;
constexpr unsigned char kUiPauseSig[2][10] = {{0x40, 0x53, 0x57, 0x48, 0x83, 0xec, 0x28, 0x48, 0x8b, 0x05},
                                              {0x48, 0x89, 0x4c, 0x24, 0x08, 0x53, 0x55, 0x56, 0x57, 0x48}};
inline bool PauseForUi(bool pause) {
  if (std::memcmp(At<const void*>(UI_PAUSE), kUiPauseSig[0], 10) != 0 || std::memcmp(At<const void*>(UI_RESUME), kUiPauseSig[1], 10) != 0) return false;
  __try {
    uint8_t* const owner = *At<uint8_t**>(ACTOR_OWNER);
    uint8_t* const actor = owner ? Ptr(owner, 0x31b0) : nullptr;
    uint8_t* const adviser = Alive(actor) ? Ptr(actor, 0x30) : nullptr;
    if (!adviser) return false;
    At<void (*)(void*)>(pause ? UI_PAUSE : UI_RESUME)(adviser);
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
// Diagnostics for the experiment: copies `size` bytes of the widget `id` of `window`. False if not there.
inline bool SnapshotWidget(void* window, uint32_t id, uint8_t* out, size_t size) {
  __try {
    uint8_t* const w = FindWidget(static_cast<uint8_t*>(window), id);
    if (!w) return false;
    std::memcpy(out, w, size);
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
