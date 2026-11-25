// SPDX-License-Identifier: GPL-3.0-or-later
// (Navi) The /fieldboss test command (lichii/hook/coop_field_boss.*): what it takes from the chat, where the enemy
// comes, and what it says as the director makes it, cannot make it, or loses it.
#include <doctest/doctest.h>

#include <cmath>
#include <map>
#include <string>

#include "coop/host/player_reader.hpp"
#include "coop_field_boss.h"
#include "coop_objects_sync.h"

using namespace CoopFieldBoss;

TEST_CASE("fieldboss: the chat command, its words and its refusals") {
    CHECK_FALSE(Parse("hello"));
    CHECK_FALSE(Parse("/fieldbossx"));
    CHECK_FALSE(Parse("/teleport x"));
    REQUIRE(Parse("/fieldboss"));
    CHECK(Parse("/fieldboss")->kind == Command::Kind::list);
    CHECK(Parse("  /FieldBoss  ")->kind == Command::Kind::list);
    CHECK(Parse("/fieldboss clear")->kind == Command::Kind::clear);
    CHECK(Parse("/fieldboss CLEAR")->kind == Command::Kind::clear);
    const auto make = Parse("/fieldboss Enemy_IronKnuckle_Silver");
    REQUIRE(make);
    CHECK(make->kind == Command::Kind::make);
    CHECK(make->name == "Enemy_IronKnuckle_Silver");
    CHECK(Parse("/fieldboss Link")->kind == Command::Kind::bad);                 // (not an enemy)
    CHECK(Parse("/fieldboss Enemy_")->kind == Command::Kind::bad);               // (no name after Enemy_)
    CHECK(Parse("/fieldboss Enemy_Iron Knuckle")->kind == Command::Kind::bad);   // (a space)
    CHECK(Parse("/fieldboss Enemy_../x")->kind == Command::Kind::bad);
    CHECK(Parse("/fieldboss Enemy_" + std::string(57, 'A'))->kind == Command::Kind::make);   // (63: the most)
    CHECK(Parse("/fieldboss Enemy_" + std::string(58, 'A'))->kind == Command::Kind::bad);
    CHECK_FALSE(Parse("/fieldboss Link")->error.empty());
}

TEST_CASE("fieldboss: every suggested name is one the command takes") {
    CHECK(Candidates().size() >= 5);
    for (const auto& n : Candidates()) {
        const auto c = Parse("/fieldboss " + n);
        REQUIRE(c);
        CHECK(c->kind == Command::Kind::make);
    }
}

TEST_CASE("fieldboss: it comes 6 m in front of our Link, level, whichever way he faces") {
    const Vec link{10, 5, -3};
    const Vec ahead = SpawnPoint(link, std::nullopt);
    CHECK(ahead.x == doctest::Approx(10));
    CHECK(ahead.y == doctest::Approx(5));
    CHECK(ahead.z == doctest::Approx(3));
    const coop::protocol::Quat none{0, 0, 0, 1};
    CHECK(SpawnPoint(link, none).z == doctest::Approx(3));
    // Turned 90 degrees about y: +z goes to +x.
    const float h = std::sqrt(0.5f);
    const coop::protocol::Quat right{0, h, 0, h};
    const Vec r = SpawnPoint(link, right);
    CHECK(r.x == doctest::Approx(16));
    CHECK(r.z == doctest::Approx(-3));
    CHECK(r.y == doctest::Approx(5));
    // Tipped forward (a slope): still 6 m away on the level.
    const float t = std::sin(0.3f), c = std::cos(0.3f);
    const coop::protocol::Quat tipped{t, 0, 0, c};
    const Vec p = SpawnPoint(link, tipped);
    CHECK(std::hypot(p.x - link.x, p.z - link.z) == doctest::Approx(6));
    CHECK(p.y == doctest::Approx(5));
    // A broken turn: straight ahead on +z.
    const coop::protocol::Quat bad{NAN, 0, 0, 1};
    CHECK(SpawnPoint(link, bad).z == doctest::Approx(3));
}

TEST_CASE("fieldboss: made, said once with its actor, then gone when the director loses it") {
    Summoner s;
    CHECK(s.Make("Enemy_Stalfos", {1, 2, 3}, 1000).empty());
    const auto wanted = s.Wanted();
    REQUIRE(wanted.size() == 1);
    const auto key = wanted[0].key;
    CHECK((key & kKeyBit) != 0);
    CHECK(wanted[0].actor == "Enemy_Stalfos");
    CHECK(s.Seen({}, 1500).empty());                  // (not made yet, still in time)
    auto lines = s.Seen({{key, 0xABC0}}, 1700);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].find("Enemy_Stalfos made (actor 0xabc0)") != std::string::npos);
    CHECK(lines[0].find("700 ms after asking") != std::string::npos);
    CHECK(s.Seen({{key, 0xABC0}}, 5000).empty());     // (said once)
    lines = s.Seen({}, 9700);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].find("is gone after 8 s") != std::string::npos);
    CHECK(s.Count() == 0);
    CHECK(s.Wanted().empty());
}

TEST_CASE("fieldboss: one the game does not make in time is given up, with why") {
    Summoner s;
    CHECK(s.Make("Enemy_IronKnuckle_Black", {0, 0, 0}, 1000).empty());
    CHECK(s.Seen({}, 1000 + kCreateWaitMs - 1).empty());
    const auto lines = s.Seen({}, 1000 + kCreateWaitMs);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].find("NOT made") != std::string::npos);
    CHECK(s.Count() == 0);
}

TEST_CASE("fieldboss: at most 3 at once, keys never repeat, and clear takes them all") {
    Summoner s;
    CHECK(s.Make("Enemy_Stalfos", {}, 0).empty());
    CHECK(s.Make("Enemy_Stalfos", {}, 0).empty());
    CHECK(s.Make("Enemy_Dinolfos", {}, 0).empty());
    CHECK_FALSE(s.Make("Enemy_Moblin_Club", {}, 0).empty());
    const auto w = s.Wanted();
    REQUIRE(w.size() == 3);
    CHECK(w[0].key != w[1].key);
    CHECK(w[1].key != w[2].key);
    CHECK(s.Clear().find("3 taken away") != std::string::npos);
    CHECK(s.Wanted().empty());
    CHECK(s.Clear().find("none") != std::string::npos);
    CHECK(s.Make("Enemy_Stalfos", {}, 0).empty());
    CHECK(s.Wanted()[0].key != w[0].key);               // (a new key after a clear: never the old actor's)
    CHECK_FALSE(s.Make("Link", {}, 0).empty());
}

// ---- step 2: the field's guardian ----

namespace {

Boss::In Night(std::uint64_t now_ms) {
    Boss::In in;
    in.in_field = true;
    in.night = true;
    in.now_ms = now_ms;
    in.link = {0, 0, 0};
    return in;
}

}  // namespace

TEST_CASE("fieldboss guardian: field, night, the clock's hours and the loot value as Navi RPG reads it") {
    CHECK(IsFieldScene("HyruleField_Child"));
    CHECK(IsFieldScene("HyruleField_Adult"));
    CHECK_FALSE(IsFieldScene("KakarikoVillage_Child"));
    CHECK_FALSE(IsFieldScene("HyruleField_Childx"));
    CHECK(IsNight(18 * 3600));
    CHECK(IsNight(2 * 3600));
    CHECK(IsNight(6 * 3600 - 1));
    CHECK_FALSE(IsNight(6 * 3600));
    CHECK_FALSE(IsNight(18 * 3600 - 1));
    for (const auto& n : BossNames()) {
        CHECK(IsBossName(n));
        CHECK(Parse("/fieldboss " + n)->kind == Command::Kind::make);   // (an enemy's name the director takes)
    }
    CHECK_FALSE(IsBossName("Enemy_Stalchild"));
    const auto v = LootValue(0x01020304, 0x1122334455667788ull, 1);
    REQUIRE(v.size() == 14);   // (rpg_loot.cpp DecodeLoot: exactly 14)
    CHECK(v[0] == 1);
    CHECK(v[1] == 0x04);
    CHECK(v[4] == 0x01);
    CHECK(v[5] == 0x88);
    CHECK(v[12] == 0x11);
    CHECK(v[13] == 1);
}

TEST_CASE("fieldboss guardian: comes only at night, in the field, to a free player, the lowest number there") {
    Boss b;
    auto in = Night(1000);
    in.night = false;
    CHECK_FALSE(b.Step(in).want);
    in = Night(1000);
    in.in_field = false;
    CHECK_FALSE(b.Step(in).want);
    in = Night(1000);
    in.busy = true;
    CHECK_FALSE(b.Step(in).want);
    in = Night(1000);
    in.me = 3;
    in.in_field_too = {2, 4};   // (player 2 is in the field: its game makes it)
    CHECK_FALSE(b.Step(in).want);
    in.in_field_too = {4, 5};
    const auto out = b.Step(in);
    REQUIRE(out.want);
    CHECK((out.want->key & kBossKeyBit) == kBossKeyBit);
    CHECK(out.want->actor == BossNames()[0]);
    CHECK(out.want->position.z == doctest::Approx(kBossAheadM));
    CHECK_FALSE(out.lines.empty());
}

TEST_CASE("fieldboss guardian: made, its life raised once, killed: loot once, then no guardian until the next night") {
    Boss b;
    auto in = Night(1000);
    in.in_field_too = {2};
    in.me = 1;
    REQUIRE(b.Step(in).want);
    in.now_ms = 1500;
    in.actor = 0xBEEF;
    in.life = 40;
    in.max_life = 40;
    auto out = b.Step(in);           // (made: alive)
    REQUIRE(out.want);
    out = b.Step(in);                // (its life seen: raised to (3 + 2 players) x 40)
    REQUIRE(out.raise_to);
    CHECK(*out.raise_to == 200);
    in.life = 200;
    in.max_life = 200;
    CHECK_FALSE(b.Step(in).raise_to);   // (once)
    in.life = 0;
    CHECK_FALSE(b.Step(in).loot);       // (at 0, still on the list: not yet)
    in.actor = 0;
    in.life.reset();
    in.max_life.reset();
    in.now_ms = 3000;
    out = b.Step(in);
    REQUIRE(out.loot);
    CHECK(out.loot->size() == 14);
    CHECK(out.loot->at(13) == kLootTable);
    CHECK_FALSE(out.want);
    in.now_ms = 3000 + kGoneWaitMs + 1;
    CHECK_FALSE(b.Step(in).want);    // (the same night: no second guardian)
    in.night = false;
    CHECK_FALSE(b.Step(in).want);    // (day)
    in.night = true;
    CHECK(b.Step(in).want);          // (the next night)
}

TEST_CASE("fieldboss guardian: a candidate not made in time is not tried again, with none left, no guardian") {
    Boss b;
    std::uint64_t now = 1000;
    for (std::size_t i = 0; i < BossNames().size(); ++i) {
        auto in = Night(now);
        const auto out = b.Step(in);
        REQUIRE(out.want);
        CHECK(out.want->actor == BossNames()[i]);
        now += kCreateWaitMs;
        in.now_ms = now;
        b.Step(in);                  // (not made: given up)
        now += 1;
    }
    CHECK(b.Off());
    CHECK_FALSE(b.Step(Night(now + 100000)).want);
}

TEST_CASE("fieldboss guardian: gone without falling gives no loot and comes again later, leaving the field drops it") {
    Boss b;
    auto in = Night(1000);
    REQUIRE(b.Step(in).want);
    in.actor = 0xAA;
    b.Step(in);
    in.actor = 0;                    // (went: never at 0)
    in.now_ms = 2000;
    auto out = b.Step(in);
    CHECK_FALSE(out.loot);
    CHECK_FALSE(out.want);
    in.now_ms = 2000 + kGoneWaitMs - 1;
    CHECK_FALSE(b.Step(in).want);
    in.now_ms = 2000 + kGoneWaitMs;
    REQUIRE(b.Step(in).want);        // (again, a new key)
    in.actor = 0xBB;
    b.Step(in);
    in.in_field = false;             // (we leave the field)
    out = b.Step(in);
    CHECK_FALSE(out.want);
    CHECK_FALSE(out.loot);
}

TEST_CASE("fieldboss guardian: its names are rolled spawns for the objects sync only while this game is in the field") {
    coop::host::PlayerReading r;
    r.scene = "HyruleField_Adult";
    Frame(0x1000, r, 1000);          // (no clock: no night, nothing made; the field is noted)
    CHECK(InField());
    CHECK(CoopObjects::IsRolled("Enemy_IronKnuckle_Silver"));
    CHECK(CoopObjects::IsRolled("Enemy_Stalchild"));
    r.scene = "SpiritTemple";
    Frame(0x1000, r, 1100);
    CHECK_FALSE(InField());
    CHECK_FALSE(CoopObjects::IsRolled("Enemy_IronKnuckle_Silver"));
    CHECK(CoopObjects::IsRolled("Enemy_Stalchild"));
    Frame(0, std::nullopt, 1200);
    CHECK_FALSE(InField());
}

TEST_CASE("fieldboss guardian: the areas and their tables, and every third guardian is elite with its table") {
    CHECK(AreaTable("HyruleField_Child") == kLootTable);
    CHECK(AreaTable("GerudoValley_Adult") == 3);
    CHECK(AreaTable("DeathMountainTrail_Child") == 4);
    CHECK(AreaTable("KakarikoVillage_Child") == 0);
    Boss b;
    std::uint64_t now = 1000;
    for (int n = 0; n < 3; ++n) {
        auto in = Night(now);
        in.table = 3;   // (Gerudo Valley)
        in.night = true;
        if (n > 0) {   // (a day between: one guardian a night)
            in.night = false;
            b.Step(in);
            in.night = true;
        }
        REQUIRE(b.Step(in).want);
        in.actor = 0x100 + static_cast<std::uint64_t>(n);
        in.life = 10;
        in.max_life = 10;
        b.Step(in);                       // (made)
        const auto raised = b.Step(in);   // (its life raised)
        REQUIRE(raised.raise_to);
        CHECK(*raised.raise_to == (n == 2 ? 10 * 4 * kEliteLife : 40));   // (alone: 4 times; the third, twice that)
        in.life = 0;
        b.Step(in);
        in.actor = 0;
        in.life.reset();
        in.max_life.reset();
        now += 100;
        in.now_ms = now;
        const auto out = b.Step(in);
        REQUIRE(out.loot);
        CHECK(out.loot->at(13) == (n == 2 ? kEliteTable : 3));
        now += kGoneWaitMs + 1;
    }
}
