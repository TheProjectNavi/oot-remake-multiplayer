// Tests for the mod-data channel's client side (lichii/hook/coop_mod_data.cpp, CoopModData::Store): MOD_DATA
// (492), issue 250 (1). Our values go out when they change and again after a new session; the others' are kept
// by player, mod and key.
#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "coop/net/client.hpp"
#include "coop/protocol/codec.hpp"
#include "coop_mod_data.h"

using CoopModData::Bytes;
using CoopModData::Store;
using coop::net::Event;
using coop::net::EventKind;
using coop::protocol::ModData;

namespace {
Event Kind(EventKind kind, std::uint8_t player = 0) {
    Event e;
    e.kind = kind;
    e.player = player;
    return e;
}
Event From(std::uint8_t player, const std::string& mod, const std::string& key, Bytes value) {
    Event e;
    e.kind = EventKind::message;
    e.message = ModData{player, mod, key, std::move(value)};
    return e;
}
}  // namespace

TEST_CASE("mod data: our changed values go out once, at most every kSendEveryMs, and again after a new session") {
    Store s;
    CHECK(s.Set("coop", "skin", {1}));
    auto sent = s.TakeSend(1000);
    REQUIRE(sent.size() == 1);
    CHECK(sent[0] == ModData{coop::protocol::kPlayerSelf, "coop", "skin", {1}});
    CHECK(s.TakeSend(5000).empty());  // nothing changed

    CHECK(s.Set("coop", "skin", {2}));
    CHECK(s.TakeSend(5000 + CoopModData::kSendEveryMs - 1).size() == 1);
    CHECK(s.Set("coop", "skin", {3}));
    CHECK(s.TakeSend(5000 + CoopModData::kSendEveryMs).empty());  // too soon after the last
    CHECK(s.TakeSend(5000 + 2 * CoopModData::kSendEveryMs).size() == 1);

    CHECK(s.Set("coop", "skin", {3}));  // the same value: nothing to send
    CHECK(s.TakeSend(9000).empty());

    s.OnEvent(Kind(EventKind::reconnected));  // the server may have lost ours
    sent = s.TakeSend(10000);
    REQUIRE(sent.size() == 1);
    CHECK(sent[0].value == Bytes{3});

    CHECK(s.Set("coop", "skin", {}));  // cleared: an empty value goes out
    sent = s.TakeSend(20000);
    REQUIRE(sent.size() == 1);
    CHECK(sent[0].value.empty());
}

TEST_CASE("mod data: a bad mod id, key or size is refused") {
    Store s;
    CHECK_FALSE(s.Set("", "skin", {1}));
    CHECK_FALSE(s.Set("Coop", "skin", {1}));
    CHECK_FALSE(s.Set("coop", "Skin", {1}));
    CHECK_FALSE(s.Set("coop", std::string(coop::protocol::kMaxModDataKeyChars + 1, 'a'), {1}));
    CHECK_FALSE(s.Set("coop", "skin", Bytes(coop::protocol::kMaxModDataValueBytes + 1, 1)));
    CHECK(s.TakeSend(1000).empty());
}

TEST_CASE("mod data: the others' values are kept by player and forgotten when they leave or a session starts") {
    Store s;
    CHECK(s.OnEvent(From(2, "coop", "skin", {7})));
    CHECK_FALSE(s.OnEvent(From(2, "coop", "skin", {7})));  // the same
    CHECK(s.Of(2, "coop", "skin") == Bytes{7});
    CHECK_FALSE(s.Of(3, "coop", "skin"));
    CHECK_FALSE(s.OnEvent(From(coop::protocol::kPlayerSelf, "coop", "skin", {9})));  // never from the server
    CHECK(s.OnEvent(From(2, "coop", "skin", {})));
    CHECK_FALSE(s.Of(2, "coop", "skin"));
    CHECK(s.OnEvent(From(3, "coop", "skin", {8})));
    CHECK(s.OnEvent(Kind(EventKind::player_left, 3)));
    CHECK_FALSE(s.Of(3, "coop", "skin"));
    CHECK(s.OnEvent(From(4, "coop", "skin", {8})));
    CHECK(s.OnEvent(Kind(EventKind::welcomed)));
    CHECK_FALSE(s.Of(4, "coop", "skin"));
}

TEST_CASE("mod data: a skin value holds the id and the pack's SHA-256, and a broken one is refused") {
    const std::string hash(64, 'a');
    const Bytes v = CoopModData::SkinValue("Malon", hash);
    REQUIRE(v.size() == 5 + 1 + 32);
    const auto back = CoopModData::SkinOf(v);
    REQUIRE(back);
    CHECK(back->first == "Malon");
    CHECK(back->second == hash);
    CHECK(CoopModData::SkinValue("", hash).empty());
    CHECK(CoopModData::SkinValue("Malon", "xyz").empty());
    CHECK(CoopModData::SkinValue("Malon", std::string(64, 'g')).empty());
    const auto none = CoopModData::SkinOf({});  // no skin: Link
    REQUIRE(none);
    CHECK(none->first.empty());
    CHECK_FALSE(CoopModData::SkinOf(Bytes{'a', 0, 1}));  // the hash cut short
    Bytes bad = v;
    bad[0] = '-';
    CHECK_FALSE(CoopModData::SkinOf(bad));
}
