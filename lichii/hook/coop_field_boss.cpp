// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_field_boss.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <mutex>

#include "coop/host/player_reader.hpp"
#include "coop_spawn.h"

#ifdef LICHII_COOP
#include "common/logging.h"
#endif
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace CoopFieldBoss {

const std::vector<std::string>& Candidates() {
    // Read from the game's Pack/Actor (build e0f3ccfaf50065ae): each is there. Fighters that stand alone first.
    static const std::vector<std::string> names = {
        "Enemy_IronKnuckle_Silver", "Enemy_IronKnuckle_Black", "Enemy_IronKnuckle_White", "Enemy_MiddleBoss_Lizalfos",
        "Enemy_Dinolfos",           "Enemy_Stalfos",           "Enemy_Moblin_Club",       "Enemy_Lizalfos",
        "Enemy_Wolfos_White",       "Enemy_LikeLike",          "Enemy_Peahat",
    };
    return names;
}

namespace {

std::string Lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string_view Trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

// An actor name the create request takes: "Enemy_" and letters, digits and _, at most 63 (the director's name cell).
bool EnemyName(std::string_view name) {
    if (name.size() < 7 || name.size() > 63 || name.substr(0, 6) != "Enemy_") return false;
    return std::all_of(name.begin(), name.end(), [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
    });
}

std::string Num(float v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.1f", static_cast<double>(v));
    return b;
}

std::string Hex(std::uint64_t v) {
    char b[24];
    std::snprintf(b, sizeof b, "0x%llx", static_cast<unsigned long long>(v));
    return b;
}

}  // namespace

std::optional<Command> Parse(std::string_view text) {
    text = Trim(text);
    const std::string_view word = "/fieldboss";
    if (text.size() < word.size() || Lower(text.substr(0, word.size())) != word) return std::nullopt;
    std::string_view rest = text.substr(word.size());
    if (!rest.empty() && !std::isspace(static_cast<unsigned char>(rest.front()))) return std::nullopt;   // ("/fieldbossx")
    rest = Trim(rest);
    Command c;
    if (rest.empty()) {
        c.kind = Command::Kind::list;
    } else if (Lower(rest) == "clear") {
        c.kind = Command::Kind::clear;
    } else if (EnemyName(rest)) {
        c.kind = Command::Kind::make;
        c.name = std::string(rest);
    } else {
        c.kind = Command::Kind::bad;
        c.error = "fieldboss: \"" + std::string(rest) + "\" is not an enemy's name (Enemy_..., letters, digits and _)";
    }
    return c;
}

Vec SpawnPoint(Vec link, const std::optional<coop::protocol::Quat>& turn, float ahead) {
    float fx = 0.0f, fz = 1.0f;
    if (turn) {
        const float x = (*turn)[0], y = (*turn)[1], z = (*turn)[2], w = (*turn)[3];
        // The quaternion turns +z (the actor's forward) to this; kept level (y dropped).
        const float rx = 2.0f * (x * z + w * y);
        const float rz = 1.0f - 2.0f * (x * x + y * y);
        const float len = std::sqrt(rx * rx + rz * rz);
        if (std::isfinite(len) && len > 1e-3f) {
            fx = rx / len;
            fz = rz / len;
        }
    }
    return {link.x + fx * ahead, link.y, link.z + fz * ahead};
}

std::string Summoner::Make(const std::string& actor, Vec position, std::uint64_t now_ms) {
    if (!EnemyName(actor)) return "fieldboss: not an enemy's name";
    if (items_.size() >= kMaxAtOnce) {
        return "fieldboss: " + std::to_string(kMaxAtOnce) + " made already: \"/fieldboss clear\" first";
    }
    Item it;
    it.want = {kKeyBit | next_++, actor, position};
    it.asked_ms = now_ms;
    items_.push_back(it);
    return "";
}

std::string Summoner::Clear() {
    const std::size_t n = items_.size();
    items_.clear();
    return n == 0 ? "fieldboss: none to clear" : "fieldboss: " + std::to_string(n) + " taken away";
}

std::vector<Summoner::Want> Summoner::Wanted() const {
    std::vector<Want> out;
    for (const Item& it : items_) out.push_back(it.want);
    return out;
}

std::vector<std::string> Summoner::Seen(const std::map<std::uint64_t, std::uint64_t>& live, std::uint64_t now_ms) {
    std::vector<std::string> lines;
    for (auto it = items_.begin(); it != items_.end();) {
        const auto found = live.find(it->want.key);
        const std::string& name = it->want.actor;
        if (it->actor == 0) {
            if (found != live.end() && found->second != 0) {
                it->actor = found->second;
                it->made_ms = now_ms;
                lines.push_back("fieldboss: " + name + " made (actor " + Hex(it->actor) + ") at " + Num(it->want.position.x) +
                                " " + Num(it->want.position.y) + " " + Num(it->want.position.z) + ", " +
                                std::to_string(now_ms - it->asked_ms) + " ms after asking");
            } else if (now_ms - it->asked_ms >= kCreateWaitMs) {
                lines.push_back("fieldboss: " + name + " NOT made in " + std::to_string(kCreateWaitMs / 1000) +
                                " s: the game could not make it here (its pack may not load in this area)");
                it = items_.erase(it);
                continue;
            }
        } else if (found == live.end()) {
            lines.push_back("fieldboss: " + name + " is gone after " + std::to_string((now_ms - it->made_ms) / 1000) +
                            " s (killed, or the game took it away)");
            it = items_.erase(it);
            continue;
        }
        ++it;
    }
    return lines;
}

// ---- the game side ----

namespace {

struct State {
    std::mutex mutex;
    Summoner summoner;
    std::vector<std::string> lines;
    std::optional<Command> pending;   // asked in the chat, made at the next frame (our Link is read there)
    bool active = false;              // (the director has been given a list since the last clear)
};

State& S() {
    static State s;
    return s;
}

void Say(State& s, std::string line) {
#ifdef LICHII_COOP
    LOG_INFO(Core, "coop: {}", line);
#endif
    s.lines.push_back(std::move(line));
}

// The game's own actor packs beside the game, when we can find them: a name it does not have is refused at once.
bool GameHasActor(const std::string& name, std::string* why) {
#ifdef _WIN32
    wchar_t exe[MAX_PATH * 2] = {};
    const DWORD n = GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
    if (n == 0 || n >= std::size(exe)) return true;   // (not known: let the create say)
    const std::filesystem::path actors = std::filesystem::path(exe).parent_path() / "romfs" / "Pack" / "Actor";
    std::error_code ec;
    if (!std::filesystem::is_directory(actors, ec)) return true;
    if (std::filesystem::exists(actors / (name + ".pack.zs"), ec)) return true;
    *why = "fieldboss: this game has no actor " + name + " (romfs/Pack/Actor)";
    return false;
#else
    (void)name;
    (void)why;
    return true;
#endif
}

}  // namespace

bool OnChat(std::string_view text) {
    auto c = Parse(text);
    if (!c) return false;
    State& s = S();
    std::lock_guard lock(s.mutex);
    switch (c->kind) {
    case Command::Kind::list: {
        std::string line = "fieldboss: try one of:";
        for (const auto& n : Candidates()) line += " " + n;
        Say(s, line);
        Say(s, "fieldboss: \"/fieldboss <name>\" makes it 6 m in front of you (this game only); \"/fieldboss clear\" takes them away");
        break;
    }
    case Command::Kind::clear:
        Say(s, s.summoner.Clear());
        s.pending.reset();
        break;
    case Command::Kind::bad:
        Say(s, c->error);
        break;
    case Command::Kind::make: {
        std::string why;
        if (!GameHasActor(c->name, &why)) {
            Say(s, why);
            break;
        }
        s.pending = std::move(c);
        break;
    }
    }
    return true;
}

void Frame(std::uint64_t main_base, const std::optional<coop::host::PlayerReading>& reading, std::uint64_t now_ms) {
    State& s = S();
    std::vector<CoopSpawn::Copy> copies;
    {
        std::lock_guard lock(s.mutex);
        if (s.pending) {
            if (main_base == 0 || !reading) {
                Say(s, "fieldboss: not in a game now: try again when you are playing");
            } else {
                const Vec link{reading->position[0], reading->position[1], reading->position[2]};
                const Vec at = SpawnPoint(link, reading->rotation);
                const std::string why = s.summoner.Make(s.pending->name, at, now_ms);
                Say(s, why.empty() ? "fieldboss: asking the game for " + s.pending->name + " at " + Num(at.x) + " " +
                                         Num(at.y) + " " + Num(at.z) + (reading->scene ? " in " + *reading->scene : "")
                                   : why);
            }
            s.pending.reset();
        }
        if (s.summoner.Count() == 0 && !s.active) return;   // (nothing asked: the director is not touched)
        for (const auto& line : s.summoner.Seen(CoopSpawn::TheDirector().Live(), now_ms)) Say(s, line);
        for (const auto& w : s.summoner.Wanted()) {
            CoopSpawn::Copy c;
            c.key = w.key;
            c.actor = w.actor;
            c.position = {w.position.x, w.position.y, w.position.z};   // (Vec3: an array of x, y, z)
            c.act = CoopSpawn::Act::show;
            c.place = false;    // (made there, then never placed again: its own AI runs)
            c.remakes = 0;      // (gone is gone: killed or taken away)
            c.real = true;      // (the game's own actor, never a pack's stand-in)
            copies.push_back(std::move(c));
        }
        s.active = !copies.empty();
    }
    CoopSpawn::TheDirector().SetSummons(main_base, std::move(copies));
}

std::vector<std::string> TakeLines() {
    State& s = S();
    std::lock_guard lock(s.mutex);
    std::vector<std::string> out;
    out.swap(s.lines);
    return out;
}

}  // namespace CoopFieldBoss
