// Calls into eurotrucks2.exe 1.61.1.1 (RVAs found statically; see MODLOG.md). Starting a job replays
// what the console command `cheat get_job <city> <city>` does, but with our cargo and companies;
// cancelling calls the same function as the game's own "cancel job" paths.
// Every entry point checks the function prologues first and runs under SEH, so another game build
// turns the feature off instead of crashing.
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace game {

constexpr uintptr_t CTRL = 0x3045760;     // game/economy controller pointer
constexpr uintptr_t PICK = 0x82e0b0;      // bool (params*, u64* src_city, u64* dst_city): random linked companies
constexpr uintptr_t GEN = 0x82ffb0;       // void (result*, params*, bool, bool): generate job offer
constexpr uintptr_t TAKE = 0x82eed0;      // int (params*, offer*, bool, bool, bool): take it, 0 = ok
constexpr uintptr_t PARAMS_DTOR = 0x82e270;
constexpr uintptr_t STRING_DTOR = 0x11a290;
constexpr uintptr_t FREE = 0xfbf00;
constexpr uintptr_t CANCEL = 0x7a5c40;    // void (ctrl*): cancel the player's job (penalty applies)
constexpr uintptr_t STRING_VTBL = 0x21d18c0, EMPTY_STR = 0x1df110e, PARAMS_FLOAT = 0x251d65c;
constexpr uintptr_t ERROR_NAMES = 0x1e1a830; // const char* [] indexed by the result codes

struct Sig {
  uintptr_t rva;
  unsigned char bytes[10];
};
constexpr Sig kSigs[] = {
    {PICK, {0x40, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83, 0xec, 0x70, 0x48}},
    {GEN, {0x44, 0x88, 0x4c, 0x24, 0x20, 0x44, 0x88, 0x44, 0x24, 0x18}},
    {TAKE, {0x44, 0x88, 0x4c, 0x24, 0x20, 0x44, 0x88, 0x44, 0x24, 0x18}},
    {PARAMS_DTOR, {0x40, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x83, 0x79, 0x68}},
    {STRING_DTOR, {0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0x51, 0x08}},
    {FREE, {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10}},
    {CANCEL, {0x48, 0x89, 0x5c, 0x24, 0x18, 0x57, 0x48, 0x83, 0xec, 0x30}},
};

inline uintptr_t Base() { return reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr)); }
template <class T> T At(uintptr_t rva) { return reinterpret_cast<T>(Base() + rva); }

inline bool Supported() {
  for (const Sig& s : kSigs)
    if (std::memcmp(At<const void*>(s.rva), s.bytes, sizeof s.bytes) != 0) return false;
  return true;
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

// Returns false and fills `err` if the game refused; `trace` gets the raw params the game picked.
inline bool StartJob(uint64_t src_city, uint64_t dst_city, uint64_t src_co, uint64_t dst_co, uint64_t cargo, char* err, size_t err_size,
                     uint64_t trace[4]) {
  using PickFn = bool (*)(void*, uint64_t*, uint64_t*);
  using GenFn = void (*)(void*, void*, bool, bool);
  using TakeFn = int (*)(void*, void*, bool, bool, bool);
  using VoidFn = void (*)(void*);
  struct Result {
    uint8_t* offer;
    int status;
  };
  alignas(16) uint8_t p[0x100] = {}; // the game's params struct is 0x70 bytes
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

    *reinterpret_cast<uintptr_t*>(p + 0x30) = Base() + STRING_VTBL;
    *reinterpret_cast<uintptr_t*>(p + 0x38) = Base() + EMPTY_STR;
    *reinterpret_cast<uint16_t*>(p + 0x61) = 1;
    *reinterpret_cast<float*>(p + 0x64) = *At<const float*>(PARAMS_FLOAT);
    uint64_t s = src_city, d = dst_city;
    if (!At<PickFn>(PICK)(p, &s, &d)) {
      std::snprintf(err, err_size, "O jogo não liga empresas entre essas duas cidades.");
    } else {
      // The game filled (company, city) pairs for source (+0x00/+0x08) and target (+0x10/+0x18);
      // which slot is the city is found by value, then the company slot gets our choice.
      uint64_t* q = reinterpret_cast<uint64_t*>(p);
      for (int i = 0; i < 4; ++i) trace[i] = q[i];
      const int sc = q[0] == src_city ? 1 : q[1] == src_city ? 0 : -1;
      const int dc = q[2] == dst_city ? 3 : q[3] == dst_city ? 2 : -1;
      if (sc < 0 || dc < 0) {
        std::snprintf(err, err_size, "Formato de parâmetros inesperado (veja o log).");
      } else {
        q[sc] = src_co;
        q[dc] = dst_co;
        q[4] = cargo;
        Result r{nullptr, 1};
        At<GenFn>(GEN)(&r, p, true, own_trailer);
        if (r.status != 0) {
          std::snprintf(err, err_size, "Não deu para gerar o serviço: %s (%d)", ErrorName(r.status), r.status);
        } else {
          const int t = At<TakeFn>(TAKE)(p, r.offer, true, own_trailer, false);
          if (t != 0) std::snprintf(err, err_size, "Não deu para assumir o serviço: %s (%d)", ErrorName(t), t);
          ok = t == 0;
        }
        if (r.offer) { // drop the generator's reference, exactly like get_job
          (*reinterpret_cast<VoidFn**>(r.offer))[1](r.offer);
          if ((InterlockedDecrement(reinterpret_cast<volatile LONG*>(r.offer + 8)) & 0x1ffffff) == 0) At<VoidFn>(FREE)(r.offer);
        }
      }
    }
    At<VoidFn>(PARAMS_DTOR)(p);
    At<VoidFn>(STRING_DTOR)(p + 0x30);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    std::snprintf(err, err_size, "Exceção dentro do jogo (0x%08lX).", GetExceptionCode());
    return false;
  }
  return ok;
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
