// SPDX-License-Identifier: GPL-3.0-or-later
// (Navi) Navi RPG's items on our Link (lichii/hook/coop_rpg_effects.*): the values read, and each effect's heal,
// never above the most, never on a Link at 0, never undoing a hit.
#include <doctest/doctest.h>

#include <vector>

#include "coop_rpg_effects.h"

using namespace CoopRpgEffects;

namespace {

Effects::In At(std::int32_t life, std::int32_t max, std::uint64_t now_ms, Worn worn = {}) {
    Effects::In in;
    in.life = life;
    in.max = max;
    in.now_ms = now_ms;
    in.worn = worn;
    return in;
}

}  // namespace

TEST_CASE("rpg effects: the wear and use values as Navi RPG writes them") {
    const auto w = DecodeWear({1, 2, 7, 0, 6, 0});
    REQUIRE(w);
    CHECK(w->vigor);
    CHECK(w->guardian);
    CHECK_FALSE(w->second_wind);
    CHECK(DecodeWear({1, 0}) == Worn{});
    CHECK_FALSE(DecodeWear({1, 2, 7, 0}));     // (short)
    CHECK_FALSE(DecodeWear({2, 0}));           // (another version)
    const auto u = DecodeUse({1, 3, 0, 0, 0, 8, 0});
    REQUIRE(u);
    CHECK(u->count == 3);
    CHECK(u->item == kTonic);
    CHECK_FALSE(DecodeUse({1, 3, 0, 0, 0, 8}));
}

TEST_CASE("rpg effects: a tonic fills the hearts once, and waits while our Link is at 0") {
    Effects e;
    auto in = At(6, 20, 1000);
    in.use = Use{1, kTonic};
    auto out = e.Step(in);
    REQUIRE(out.raise_to);
    CHECK(*out.raise_to == 20);
    in.now_ms = 1100;
    in.life = 20;
    CHECK_FALSE(e.Step(in).raise_to);   // (the same use: once)
    // Drunk while down: given when up again.
    in.use = Use{2, kTonic};
    in.life = 0;
    in.now_ms = 1200;
    CHECK_FALSE(e.Step(in).raise_to);
    in.life = 4;
    in.now_ms = 1300;
    out = e.Step(in);
    REQUIRE(out.raise_to);
    CHECK(*out.raise_to == 20);
    // Another item used: nothing.
    in.use = Use{3, 1};
    in.life = 10;
    in.now_ms = 5000;
    CHECK_FALSE(e.Step(in).raise_to);
}

TEST_CASE("rpg effects: the Vigor Charm gives a quarter heart every 6 s while hurt, never above the most") {
    Effects e;
    Worn w;
    w.vigor = true;
    CHECK_FALSE(e.Step(At(10, 12, 1000, w)).raise_to);          // (the clock starts)
    CHECK_FALSE(e.Step(At(10, 12, 1000 + kVigorMs - 1, w)).raise_to);
    auto out = e.Step(At(10, 12, 1000 + kVigorMs, w));
    REQUIRE(out.raise_to);
    CHECK(*out.raise_to == 11);
    out = e.Step(At(11, 12, 1000 + 2 * kVigorMs, w));
    REQUIRE(out.raise_to);
    CHECK(*out.raise_to == 12);
    CHECK_FALSE(e.Step(At(12, 12, 1000 + 4 * kVigorMs, w)).raise_to);   // (full)
    CHECK_FALSE(Effects{}.Step(At(10, 12, 100000)).raise_to);        // (not worn)
}

TEST_CASE("rpg effects: the Guardian Charm gives half a hit back a second later, rounded up") {
    Effects e;
    Worn w;
    w.guardian = true;
    e.Step(At(20, 20, 1000, w));
    CHECK_FALSE(e.Step(At(15, 20, 1100, w)).raise_to);   // (hit for 5)
    CHECK_FALSE(e.Step(At(15, 20, 1100 + kGuardianMs - 1, w)).raise_to);
    auto out = e.Step(At(15, 20, 1100 + kGuardianMs, w));
    REQUIRE(out.raise_to);
    CHECK(*out.raise_to == 18);   // (3 of 5 back)
    // Taken off before it came: nothing.
    Effects f;
    f.Step(At(20, 20, 1000, w));
    f.Step(At(12, 20, 1100, w));
    CHECK_FALSE(f.Step(At(12, 20, 1100 + kGuardianMs, Worn{})).raise_to);
}

TEST_CASE("rpg effects: the Second Wind Charm heals fast at one heart or less only") {
    Effects e;
    Worn w;
    w.second_wind = true;
    e.Step(At(8, 20, 1000, w));
    CHECK_FALSE(e.Step(At(8, 20, 5000, w)).raise_to);   // (two hearts: nothing)
    e.Step(At(3, 20, 5100, w));
    auto out = e.Step(At(3, 20, 5100 + kWindMs, w));
    REQUIRE(out.raise_to);
    CHECK(*out.raise_to == 4);
    out = e.Step(At(4, 20, 5100 + 2 * kWindMs, w));
    REQUIRE(out.raise_to);
    CHECK(*out.raise_to == 5);
    CHECK_FALSE(e.Step(At(5, 20, 5100 + 3 * kWindMs, w)).raise_to);   // (above one heart)
}

TEST_CASE("rpg effects: a hit while a heal waits takes the heal back, so the hit stands") {
    Effects e;
    Worn w;
    w.vigor = true;
    e.Step(At(10, 20, 1000, w));
    auto out = e.Step(At(10, 20, 1000 + kVigorMs, w));
    REQUIRE(out.raise_to);
    CHECK(*out.raise_to == 11);
    out = e.Step(At(6, 20, 1000 + kVigorMs + 50, w));   // (hit before the heal took)
    CHECK(out.drop_ask);
    CHECK_FALSE(out.raise_to);
    // No life read, a broken most, or a Link at 0: nothing at all.
    Effects f;
    CHECK_FALSE(f.Step(Effects::In{}).raise_to);
    CHECK_FALSE(f.Step(At(5, 0, 1000, w)).raise_to);
    CHECK_FALSE(f.Step(At(0, 20, 100000, w)).raise_to);
}

TEST_CASE("rpg effects: the upgraded charms: Vigor+ every 4 s, Guardian+ three quarters back, Second Wind+ at two hearts") {
    CHECK(DecodeWear({1, 3, 9, 0, 10, 0, 11, 0})->vigor_plus);
    Effects v;
    Worn w;
    w.vigor_plus = true;
    v.Step(At(10, 20, 1000, w));
    CHECK_FALSE(v.Step(At(10, 20, 1000 + kVigorPlusMs - 1, w)).raise_to);
    CHECK(v.Step(At(10, 20, 1000 + kVigorPlusMs, w)).raise_to == 11);
    Effects g;
    Worn gw;
    gw.guardian_plus = true;
    g.Step(At(20, 20, 1000, gw));
    g.Step(At(12, 20, 1100, gw));   // (hit for 8: 6 back)
    CHECK(g.Step(At(12, 20, 1100 + kGuardianMs, gw)).raise_to == 18);
    Effects s;
    Worn sw;
    sw.second_wind_plus = true;
    s.Step(At(8, 20, 1000, sw));
    CHECK(s.Step(At(8, 20, 1000 + kWindMs, sw)).raise_to == 9);   // (two hearts: it works)
}
