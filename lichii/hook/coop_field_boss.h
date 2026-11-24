// SPDX-License-Identifier: GPL-3.0-or-later
// (Navi) The Hyrule Field boss, step 1: a test command that makes an enemy in front of our Link, to find which
// mini-boss the game can make in the field and fight there (Navi's rpg/DESIGN.md, phase 1).
//
//   /fieldboss                 lists the enemies to try
//   /fieldboss <Enemy_Name>    makes one 6 m in front of our Link (at most 3 at a time)
//   /fieldboss clear           takes away the ones it made
//
// The spawn director (coop_spawn.h, keys with bit 60) makes it with the game's own create request, as it makes the
// other players' Stalchild, and never places it again: its own AI runs. This game only: nothing is sent to the other
// players yet (a later step shares it as a made enemy, as rolled spawns are). Each step is said in the chat and the log
// ("coop: fieldboss: ..."), which is what the test is for: made or not, and when it went.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "coop/protocol/messages.hpp"

namespace coop::host {
class Session;
struct PlayerReading;
}  // namespace coop::host

namespace CoopFieldBoss {

inline constexpr std::uint64_t kKeyBit = std::uint64_t{1} << 60;   // the director's keys for these (CoopSpawn)
inline constexpr std::size_t kMaxAtOnce = 3;
inline constexpr float kAheadM = 6.0f;
inline constexpr std::uint64_t kCreateWaitMs = 8000;   // not made by then: the game could not make it here

struct Vec {
    float x = 0, y = 0, z = 0;
};

// The enemies worth trying first: mini-bosses and big enemies that fight on their own (no room, no event).
const std::vector<std::string>& Candidates();

// A chat line that is this command, or nothing (chat). Any case for "/fieldboss"; the name as typed.
struct Command {
    enum class Kind : std::uint8_t { list, make, clear, bad } kind = Kind::list;
    std::string name;    // make: the actor's name
    std::string error;   // bad: why
};
std::optional<Command> Parse(std::string_view text);

// Where it comes: `ahead` metres in front of our Link, by its turn (the quaternion's forward, +z, kept level); straight
// ahead on +z when the turn is not known.
Vec SpawnPoint(Vec link, const std::optional<coop::protocol::Quat>& turn, float ahead = kAheadM);

// The summons and what became of them, without a game (tests).
class Summoner {
public:
    struct Want {
        std::uint64_t key = 0;
        std::string actor;
        Vec position;
    };
    // A new one; "" when taken, else why not.
    std::string Make(const std::string& actor, Vec position, std::uint64_t now_ms);
    // All of them away (the director deletes what it made).
    std::string Clear();
    // What the director should keep making now.
    std::vector<Want> Wanted() const;
    // The director's live copies now (key to actor): lines to say, once each.
    std::vector<std::string> Seen(const std::map<std::uint64_t, std::uint64_t>& live, std::uint64_t now_ms);
    std::size_t Count() const { return items_.size(); }

private:
    struct Item {
        Want want;
        std::uint64_t asked_ms = 0;
        std::uint64_t actor = 0;      // 0: not made yet
        std::uint64_t made_ms = 0;
    };
    std::vector<Item> items_;
    std::uint64_t next_ = 1;
};

// ---- the game side (coop_field_boss.cpp) ----

// A chat line: true when it was this command (its answer comes as chat notices).
bool OnChat(std::string_view text);
// Each frame (present thread), after the objects sync: our Link, and the director's list.
void Frame(std::uint64_t main_base, const std::optional<coop::host::PlayerReading>& reading, std::uint64_t now_ms);
// Lines for the chat, once each.
std::vector<std::string> TakeLines();

}  // namespace CoopFieldBoss
