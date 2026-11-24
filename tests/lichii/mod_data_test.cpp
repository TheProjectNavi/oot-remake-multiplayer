// SPDX-License-Identifier: GPL-3.0-or-later
// (Navi) The mod-data channel's client side (lichii/hook/coop_mod_data.*): values sent within the server's budget and
// never lost over it, the longest-waiting first, all of them again at a refresh, clears sent again once, the server's
// key and mod limits kept, and the session view mod API v4's players() reads.
#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "coop/net/client.hpp"
#include "coop_mod_data.h"

using CoopModData::Bytes;
using CoopModData::Store;
using coop::protocol::ModData;

namespace {

coop::net::Event Welcomed() {
    coop::net::Event e;
    e.kind = coop::net::EventKind::welcomed;
    return e;
}

coop::net::Event From(std::uint8_t player, const std::string& mod, const std::string& key, Bytes value) {
    coop::net::Event e;
    e.kind = coop::net::EventKind::message;
    ModData d;
    d.player_number = player;
    d.mod_id = mod;
    d.key = key;
    d.value = std::move(value);
    e.message = d;
    return e;
}

std::set<std::string> Keys(const std::vector<ModData>& sent) {
    std::set<std::string> out;
    for (const auto& d : sent) out.insert(d.mod_id + "/" + d.key);
    return out;
}

}  // namespace

TEST_CASE("mod data: a value goes out once, and not again until it changes or a refresh") {
    Store s;
    CHECK(s.Set("rpg", "a", {1}));
    auto sent = s.TakeSend(1000);
    REQUIRE(sent.size() == 1);
    CHECK(sent[0].mod_id == "rpg");
    CHECK(sent[0].key == "a");
    CHECK(sent[0].value == Bytes{1});
    CHECK(s.TakeSend(1300).empty());
    CHECK(s.Set("rpg", "a", {1}));   // (the same value: nothing new)
    CHECK(s.TakeSend(1600).empty());
    CHECK(s.Set("rpg", "a", {2}));
    sent = s.TakeSend(1900);
    REQUIRE(sent.size() == 1);
    CHECK(sent[0].value == Bytes{2});
}

TEST_CASE("mod data: never more than 3 sends in a second, the rest wait, none is lost") {
    Store s;
    for (const char* k : {"a", "b", "c", "d", "e"}) CHECK(s.Set("rpg", k, {7}));
    auto first = s.TakeSend(1000);
    CHECK(first.size() == CoopModData::kSendBudget);
    CHECK(s.Due() == 2);
    CHECK(s.TakeSend(1250).empty());   // (the budget is spent for this second)
    CHECK(s.TakeSend(1999).empty());
    auto second = s.TakeSend(2000);
    CHECK(second.size() == 2);
    CHECK(s.Due() == 0);
    auto all = Keys(first);
    for (const auto& k : Keys(second)) all.insert(k);
    CHECK(all == std::set<std::string>{"rpg/a", "rpg/b", "rpg/c", "rpg/d", "rpg/e"});
}

TEST_CASE("mod data: in any second at most 3 go out, whatever the ticks") {
    Store s;
    std::vector<std::uint64_t> times;
    for (std::uint64_t t = 1000; t < 30000; t += 50) {
        // a value that changes all the time, and another every tick
        s.Set("rpg", "fast", {static_cast<std::uint8_t>(t / 50 % 256)});
        s.Set("rpg", "faster", {static_cast<std::uint8_t>(t / 50 % 251)});
        for (const auto& d : s.TakeSend(t)) {
            (void)d;
            times.push_back(t);
        }
    }
    for (std::size_t i = 0; i < times.size(); ++i) {
        std::size_t in_second = 0;
        for (std::size_t j = i; j < times.size() && times[j] - times[i] < 1000; ++j) ++in_second;
        CHECK(in_second <= CoopModData::kSendBudget);
    }
    CHECK(times.size() > 50);   // (and they do keep going out)
}

TEST_CASE("mod data: a value that changes all the time cannot keep another waiting") {
    Store s;
    for (const char* k : {"a", "b", "c"}) s.Set("rpg", k, {1});
    s.TakeSend(1000);   // (a, b and c out)
    s.Set("rpg", "z", {1});
    bool z_out = false;
    for (std::uint64_t t = 1250; t < 4000 && !z_out; t += 250) {
        for (const char* k : {"a", "b", "c"}) s.Set("rpg", k, {static_cast<std::uint8_t>(t / 250)});
        for (const auto& d : s.TakeSend(t)) z_out = z_out || d.key == "z";
    }
    CHECK(z_out);
}

TEST_CASE("mod data: at a refresh every value goes out again, within the budget") {
    Store s;
    for (const char* k : {"a", "b", "c", "d"}) s.Set("rpg", k, {3});
    std::uint64_t t = 1000;
    std::size_t sent = 0;
    for (; t < 5000; t += 250) sent += s.TakeSend(t).size();
    CHECK(sent == 4);
    CHECK(s.Due() == 0);
    for (; t < 1000 + CoopModData::kRefreshMs; t += 250) CHECK(s.TakeSend(t).empty());
    std::set<std::string> again;
    for (; t < 1000 + CoopModData::kRefreshMs + 3000; t += 250) {
        const auto out = s.TakeSend(t);
        CHECK(out.size() <= CoopModData::kSendBudget);
        for (const auto& k : Keys(out)) again.insert(k);
    }
    CHECK(again == std::set<std::string>{"rpg/a", "rpg/b", "rpg/c", "rpg/d"});
}

TEST_CASE("mod data: a clear sends an empty value, once more at the next refresh, then never") {
    Store s;
    s.Set("rpg", "a", {1});
    s.TakeSend(1000);
    CHECK(s.Set("rpg", "a", {}));
    auto out = s.TakeSend(1300);
    REQUIRE(out.size() == 1);
    CHECK(out[0].value.empty());
    CHECK_FALSE(s.Ours("rpg", "a"));
    int empties = 0;
    for (std::uint64_t t = 1600; t < 1000 + 3 * CoopModData::kRefreshMs + 1000; t += 250) {
        for (const auto& d : s.TakeSend(t)) {
            CHECK(d.value.empty());
            ++empties;
        }
    }
    CHECK(empties == 1);
    CHECK(s.Set("rpg", "b", {}));   // (clearing one never set: nothing to send)
    CHECK(s.TakeSend(1000 + 4 * CoopModData::kRefreshMs).empty());
}

TEST_CASE("mod data: the server's limits: 8 keys a mod, 16 mods, a clear frees room") {
    Store s;
    for (int i = 0; i < 8; ++i) CHECK(s.Set("rpg", "k" + std::to_string(i), {1}));
    CHECK_FALSE(s.Set("rpg", "k8", {1}));
    CHECK(s.Set("rpg", "k3", {2}));    // (a key it has: a new value is fine)
    CHECK(s.Set("rpg", "k3", {}));
    CHECK(s.Set("rpg", "k8", {1}));    // (room again)
    for (int i = 1; i < 16; ++i) CHECK(s.Set("mod" + std::to_string(i), "k", {1}));
    CHECK_FALSE(s.Set("mod16", "k", {1}));   // (a 17th mod)
    CHECK(s.Set("mod1", "k2", {1}));         // (a mod it has: a new key is fine)
    CHECK_FALSE(s.Set("RPG", "k", {1}));     // (not a mod id)
    CHECK_FALSE(s.Set("rpg", "", {1}));
    CHECK_FALSE(s.Set("rpg", "k", Bytes(65, 1)));
    CHECK(s.Set("rpg", "k0", Bytes(64, 1)));
}

TEST_CASE("mod data: our own values read back, the others' come by player and leave with a new session") {
    Store s;
    s.Set("rpg", "inv", {4, 5});
    CHECK(s.Ours("rpg", "inv") == Bytes{4, 5});
    CHECK_FALSE(s.Ours("rpg", "none"));
    CHECK(s.OnEvent(From(3, "rpg", "inv", {9})));
    CHECK_FALSE(s.OnEvent(From(3, "rpg", "inv", {9})));   // (the same again: no change)
    CHECK(s.Of(3, "rpg", "inv") == Bytes{9});
    CHECK_FALSE(s.Of(4, "rpg", "inv"));
    CHECK(s.OnEvent(From(3, "rpg", "inv", {})));          // (cleared)
    CHECK_FALSE(s.Of(3, "rpg", "inv"));
    s.OnEvent(From(3, "rpg", "inv", {9}));
    s.TakeSend(1000);
    CHECK(s.Due() == 0);
    CHECK(s.OnEvent(Welcomed()));                         // (a new session: theirs gone, ours due again)
    CHECK_FALSE(s.Of(3, "rpg", "inv"));
    CHECK(s.Due() == 1);
}

TEST_CASE("mod data: the session view: sorted, one entry a number, nothing outside a session") {
    using CoopModData::ViewOf;
    const auto v = ViewOf(true, 2, 1, {{5, "c"}, {1, "a"}, {2, "b"}, {5, "c again"}, {0, "bad"}, {17, "bad"}});
    CHECK(v.in_session);
    CHECK(v.me == 2);
    CHECK(v.host == 1);
    REQUIRE(v.players.size() == 3);
    CHECK(v.players[0].first == 1);
    CHECK(v.players[1].first == 2);
    CHECK(v.players[2].first == 5);
    CHECK(ViewOf(false, 2, 1, {{1, "a"}}) == CoopModData::SessionView{});
    CHECK(ViewOf(true, 0, 1, {{1, "a"}}) == CoopModData::SessionView{});
}
