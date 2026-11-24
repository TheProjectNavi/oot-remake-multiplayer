// SPDX-License-Identifier: GPL-3.0-or-later
// (Navi) What Navi RPG's items do to our Link (Navi's rpg/DESIGN.md, phase 5). Navi RPG (navi_rpg.dll, mod API 4)
// shares, under its mod id "navi-rpg", the charms we wear ("wear") and the last item we used ("use"); this reads our
// own two values (CoopModData::Ours) and heals our Link through the game's own Life step, as down but not out's revive
// does (CoopDbno::Body::Raising: the game's heal, a write if the step does not take it in time). Every effect only
// ever raises life, never above the most, never on a Link at 0 (down, dead: down but not out's own rules then).
//
//   Fairy Tonic (8)        drunk: all the hearts back.
//   Vigor Charm (7)        worn: a quarter heart back every kVigorMs while hurt.
//   Guardian Charm (6)     worn: half of each hit taken (rounded up) back kGuardianMs later.
//   Second Wind Charm (5)  worn: at one heart or less, a quarter heart back every kWindMs.
//
// Alone too: our Link's life and most come from the player reader (read_life, read_max_life), read each frame.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace coop::host {
struct PlayerReading;
}

namespace CoopRpgEffects {

inline constexpr std::uint16_t kTonic = 8, kVigor = 7, kGuardian = 6, kSecondWind = 5;
inline constexpr std::uint64_t kVigorMs = 6000, kGuardianMs = 1000, kWindMs = 1000;
inline constexpr std::int32_t kWindAtOrBelow = 4;   // one heart, in quarters
inline constexpr std::uint64_t kPendingMs = 2000;   // a raise asked: counted as done, or given up, after this

struct Worn {
    bool vigor = false, guardian = false, second_wind = false;
    bool operator==(const Worn&) const = default;
};
struct Use {
    std::uint32_t count = 0;   // rises by one with each use this run
    std::uint16_t item = 0;
    bool operator==(const Use&) const = default;
};
// Navi RPG's values (rpg_mod.cpp): "wear" = version 1, n, n item ids (u16 LE); "use" = version 1, count (u32 LE),
// item (u16 LE). Anything else: nothing.
std::optional<Worn> DecodeWear(const std::vector<std::uint8_t>& b);
std::optional<Use> DecodeUse(const std::vector<std::uint8_t>& b);

class Effects {
public:
    struct In {
        std::optional<std::int32_t> life, max;   // quarter hearts
        std::uint64_t now_ms = 0;
        Worn worn;
        std::optional<Use> use;
    };
    struct Out {
        std::optional<std::int32_t> raise_to;   // ask the game's heal to this
        bool drop_ask = false;                  // a hit came while our ask waited: take the ask back (it would undo the hit)
        std::vector<std::string> lines;
    };
    Out Step(const In& in);

private:
    std::optional<std::int32_t> last_life_;
    std::optional<std::uint32_t> last_use_;
    bool fill_ = false;                          // a tonic drunk, not yet given (our Link at 0 then)
    std::vector<std::pair<std::uint64_t, std::int32_t>> back_;   // guardian: (when, how much)
    std::uint64_t vigor_at_ = 0, wind_at_ = 0;
    std::int32_t pending_to_ = -1;
    std::uint64_t pending_until_ = 0;
    bool started_ = false;
};

// The game side: each frame (present thread), with our Link's life and most (quarter hearts; nothing: not read).
void Frame(std::optional<std::int32_t> life, std::optional<std::int32_t> max, std::uint64_t now_us);

}  // namespace CoopRpgEffects
