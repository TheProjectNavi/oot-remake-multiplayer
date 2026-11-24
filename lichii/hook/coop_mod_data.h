// SPDX-License-Identifier: GPL-3.0-or-later
// The mod-data channel's client side (MOD_DATA, 492; docs/protocol.md "Mod data", issue 250 (1)): small
// values a mod gives the other players' copies of the same mod, under keys of its own. Our own values are
// kept here and sent when they change (at most every kSendEveryMs) and again after a reconnect; the server
// keeps everyone's newest, gives them to a joiner and drops a player's when it leaves. The others' values
// are kept by player, mod and key.
//
// Co-op itself uses the mod id "coop": key "skin" is the custom model a player wears (SkinValue).
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "coop/protocol/messages.hpp"

namespace coop::host {
class Session;
}
namespace coop::net {
class Client;
struct Event;
}  // namespace coop::net

namespace CoopModData {

using Bytes = std::vector<std::uint8_t>;
inline constexpr std::uint64_t kSendEveryMs = 250;
// (Navi) The server takes at most kModDataPerSecond (4) MOD_DATA a second from a player, all mods together, and drops
// the rest without a word (server/session.cpp on_mod_data), so a value sent over that budget never reached the others
// and was not sent again until it changed. We send at most kSendBudget in any kBudgetWindowMs (one under the server's,
// for a window the server opens a little late and for messages that arrive closer together than they left); a value
// over the budget stays due and goes in the next window. And every kRefreshMs all our values go out again, so one the
// server dropped anyway (a burst after a reconnect, an older server's other limits) is right again within that time.
inline constexpr std::size_t kSendBudget = 3;
inline constexpr std::uint64_t kBudgetWindowMs = 1000;
inline constexpr std::uint64_t kRefreshMs = 10000;

// (Navi) Who is in the session, as this game's client knows it (mod API v4's players(), coop_mod_api.h).
struct SessionView {
    bool in_session = false;            // welcomed, not disconnected
    std::uint8_t me = 0;                // our number; 0 when not in a session
    std::uint8_t host = 0;              // the host's number; 0 when none is known
    std::vector<std::pair<std::uint8_t, std::string>> players;   // number and nickname, ours included, ascending
    bool operator==(const SessionView&) const = default;
};

// The logic, without a network (tests).
class Store {
public:
    // Our value for mod and key ({} clears it); false when the id, key or size is not allowed, or (Navi) when it would
    // be a key or mod over the server's limits (kMaxModDataKeys keys a mod, kMaxModDataMods mods): the server would drop
    // it without a word.
    bool Set(const std::string& mod_id, const std::string& key, Bytes value);
    // A received message or a session event; true when another player's value changed.
    bool OnEvent(const coop::net::Event& event);
    // Our values to send now (each changed one; all of them after a new session and every kRefreshMs), at most
    // kSendBudget in any kBudgetWindowMs, the longest-waiting first; the rest stay due.
    std::vector<coop::protocol::ModData> TakeSend(std::uint64_t now_ms);
    std::optional<Bytes> Of(std::uint8_t player, const std::string& mod_id, const std::string& key) const;
    // (Navi) Our own value for mod and key, as Set left it.
    std::optional<Bytes> Ours(const std::string& mod_id, const std::string& key) const;
    // (Navi) Values waiting to be sent (tests, and the log).
    std::size_t Due() const { return due_.size(); }

private:
    using Key = std::tuple<std::string, std::string>;
    std::map<Key, Bytes> ours_;
    std::map<Key, bool> due_;   // ours to send (a cleared one sends an empty value)
    std::map<std::uint8_t, std::map<Key, Bytes>> theirs_;
    std::map<Key, std::uint64_t> sent_ms_;   // (Navi) when each key last went out: the longest-waiting goes first
    std::vector<std::uint64_t> window_;      // (Navi) when the sends of the last kBudgetWindowMs went out
    std::map<Key, bool> cleared_;            // (Navi) keys we cleared this session (true: sent again at a refresh)
    std::uint64_t last_send_ms_ = 0;
    std::uint64_t last_refresh_ms_ = 0;
    bool sent_once_ = false;
};

// The process-wide store, any thread.
bool Set(const std::string& mod_id, const std::string& key, Bytes value);
std::optional<Bytes> Of(std::uint8_t player, const std::string& mod_id, const std::string& key);
// (Navi) Our own value (as Of for the others).
std::optional<Bytes> Ours(const std::string& mod_id, const std::string& key);
// (Navi) Who is in the session now (kept from the client at each event); any thread.
SessionView Session();
// (Navi) The view from a client's state: what OnEvent keeps (tests give it numbers of their own).
SessionView ViewOf(bool in_session, std::uint8_t me, std::uint8_t host,
                   std::vector<std::pair<std::uint8_t, std::string>> players);
// The network thread: each event (coop_hook.cpp's event loop).
void OnEvent(const coop::net::Event& event, coop::net::Client& client);
// Each frame: our changed values out.
void Frame(coop::host::Session* session, std::uint64_t now_ms);

// "coop" / "skin": the skin's id, a 0 byte, and the 32 bytes of its SHA-256 (the pack's hex sha256). An empty
// id: no skin (Link). Decode gives the id and the hash as lowercase hex; nothing for a broken value.
Bytes SkinValue(const std::string& id, const std::string& sha256_hex);
std::optional<std::pair<std::string, std::string>> SkinOf(const Bytes& value);

}  // namespace CoopModData
