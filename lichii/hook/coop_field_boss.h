// SPDX-License-Identifier: GPL-3.0-or-later
// (Navi) The Hyrule Field boss, step 1: a test command that makes an enemy in front of our Link, to find which
// mini-boss the game can make in the field and fight there (Navi's rpg/DESIGN.md, phase 1).
//
//   /fieldboss                 lists the enemies to try
//   /fieldboss <Enemy_Name>    makes one 6 m in front of our Link (at most 3 at a time)
//   /fieldboss clear           takes away the ones it made
//
// The spawn director (coop_spawn.h, keys with bit 60) makes it with the game's own create request, as it makes the
// other players' Stalchild, and never places it again: its own AI runs. This game only. Each step is said in the chat
// and the log ("coop: fieldboss: ..."), which is what the test is for: made or not, and when it went.
//
// Step 2, the boss itself (Boss below): at night in Hyrule Field, the game of the lowest player number there (alone:
// ours) makes the field's guardian 20 m in front of its Link, the first of kBossNames the game can make (one it cannot
// make in kCreateWaitMs is not tried again this run). It is shared as a rolled enemy (coop_objects_sync IsRolled: its
// names, only while this game is in the field), so every player's game has it, with one owner, every hit counting and
// one death, by the objects sync's own rules. Its life is raised once, to (3 + players in the field) times its own.
// When it dies (its life seen at 0, then gone), its maker shares a loot event as Navi RPG's value "bossloot" (mod API
// 4, coop_mod_data.h): every player's Navi RPG takes its own drop, once. Then no boss until the next night; one that
// went without dying (left behind, unloaded) may come again after kGoneWaitMs. NAVI_FIELD_BOSS=0 turns it off.
#pragma once

#include <cstdint>
#include <functional>
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
inline constexpr float kBossAheadM = 20.0f;
inline constexpr std::uint64_t kGoneWaitMs = 30'000;     // a boss that went without dying: again after this
inline constexpr std::uint64_t kBossKeyBit = kKeyBit | (std::uint64_t{1} << 40);   // (apart from /fieldboss's keys)
inline constexpr std::uint8_t kLootTable = 1;            // Navi RPG's "Field guardian" table (rpg_loot.cpp)

// The boss's candidates, in the order tried.
const std::vector<std::string>& BossNames();
bool IsBossName(std::string_view name);
// Whether this game is in Hyrule Field now (set each frame): with IsBossName, coop_objects_sync's IsRolled.
bool InField();
bool IsFieldScene(std::string_view scene);
bool IsNight(std::uint32_t seconds_of_day);   // 18:00 to 06:00
// Navi RPG's loot value (rpg_loot.cpp EncodeLoot's bytes): version 1, the event, the seed, the table.
std::vector<std::uint8_t> LootValue(std::uint32_t event, std::uint64_t seed, std::uint8_t table);

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

// The field's guardian, without a game (tests).
class Boss {
public:
    struct In {
        bool in_field = false;
        bool night = false;
        bool busy = false;                     // an event, a menu, our Link down or loading
        std::uint8_t me = 0;                   // our number; 0 alone
        std::vector<std::uint8_t> in_field_too;   // the other players in the field now
        Vec link;
        std::optional<coop::protocol::Quat> turn;
        std::uint64_t now_ms = 0;
        std::uint64_t actor = 0;               // the director's live actor for the boss's key (0: none)
        std::optional<std::int32_t> life, max_life;   // read from it
    };
    struct Out {
        std::optional<Summoner::Want> want;    // what the director should keep making
        std::optional<std::int32_t> raise_to;  // write life and max life to this, once
        std::optional<std::vector<std::uint8_t>> loot;   // share as Navi RPG's "bossloot"
        std::vector<std::string> lines;
    };
    Out Step(const In& in);
    bool Off() const { return off_; }

private:
    enum class Phase : std::uint8_t { idle, asking, alive } phase_ = Phase::idle;
    std::size_t candidate_ = 0;
    std::uint64_t key_ = 0, next_key_ = 1;
    std::uint64_t asked_ms_ = 0;
    Vec at_;
    std::uint64_t actor_ = 0;
    bool raised_ = false;
    bool died_ = false;                        // its life seen at 0
    std::size_t players_ = 1;
    std::uint64_t wait_until_ms_ = 0;
    bool wait_for_day_ = false;
    bool off_ = false;                         // every candidate failed
};

// ---- the game side (coop_field_boss.cpp) ----

// A chat line: true when it was this command (its answer comes as chat notices).
bool OnChat(std::string_view text);
// Each frame (present thread), after the objects sync: our Link, the others' scenes (number, scene), the director's
// list; `memory` reads and writes the boss's life (null: not touched); `clock` the game's time, seconds since
// midnight (CoopTime::ClockSeconds; nothing: not known, so no night).
struct OtherPlayer {
    std::uint8_t number = 0;
    std::optional<std::string> scene;
};
struct Memory {
    std::function<bool(std::uint64_t, void*, std::size_t)> read;
    std::function<bool(std::uint64_t, const void*, std::size_t)> write;
};
void Frame(std::uint64_t main_base, const std::optional<coop::host::PlayerReading>& reading, std::uint64_t now_ms,
           std::uint8_t me = 0, const std::vector<OtherPlayer>& others = {}, const Memory* memory = nullptr,
           std::optional<std::uint32_t> clock = std::nullopt);
// Lines for the chat, once each.
std::vector<std::string> TakeLines();

}  // namespace CoopFieldBoss
