// SPDX-License-Identifier: GPL-3.0-or-later
// (Navi) The /fieldboss test command (lichii/hook/coop_field_boss.*): what it takes from the chat, where the enemy
// comes, and what it says as the director makes it, cannot make it, or loses it.
#include <doctest/doctest.h>

#include <cmath>
#include <map>
#include <string>

#include "coop_field_boss.h"

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
