// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_field_boss.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <limits>
#include <random>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <mutex>

#include "coop/host/player_reader.hpp"
#include "coop_mod_data.h"
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

// ---- the field's guardian (step 2) ----

const std::vector<std::string>& BossNames() {
    // Fighters that stand alone, strongest first; each is in the game's Pack/Actor. Not yet seen made in the field:
    // one the game cannot make there is given up on and the next is tried.
    static const std::vector<std::string> names = {"Enemy_IronKnuckle_Silver", "Enemy_MiddleBoss_Lizalfos",
                                                   "Enemy_Dinolfos", "Enemy_Stalfos", "Enemy_Moblin_Club"};
    return names;
}

bool IsBossName(std::string_view name) {
    const auto& n = BossNames();
    return std::find(n.begin(), n.end(), name) != n.end();
}

namespace {
std::atomic<bool> g_in_field{false};
}  // namespace

bool InField() { return g_in_field.load(std::memory_order_relaxed); }

bool IsFieldScene(std::string_view scene) {
    return scene == "HyruleField_Child" || scene == "HyruleField_Adult" || scene == "HyruleField";
}

bool IsNight(std::uint32_t seconds) { return seconds >= 18u * 3600u || seconds < 6u * 3600u; }

std::vector<std::uint8_t> LootValue(std::uint32_t event, std::uint64_t seed, std::uint8_t table) {
    std::vector<std::uint8_t> b{1};
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>(event >> (8 * i)));
    for (int i = 0; i < 8; ++i) b.push_back(static_cast<std::uint8_t>(seed >> (8 * i)));
    b.push_back(table);
    return b;
}

Boss::Out Boss::Step(const In& in) {
    Out out;
    if (off_) return out;
    if (!in.night) wait_for_day_ = false;   // (a new day: the next night may have its guardian)
    const bool lowest = in.me == 0 || std::all_of(in.in_field_too.begin(), in.in_field_too.end(),
                                                  [&](std::uint8_t n) { return n > in.me; });
    const auto& names = BossNames();
    switch (phase_) {
    case Phase::idle:
        if (in.in_field && in.night && !in.busy && lowest && !wait_for_day_ && in.now_ms >= wait_until_ms_) {
            if (candidate_ >= names.size()) {
                off_ = true;
                return out;
            }
            key_ = kBossKeyBit | next_key_++;
            at_ = SpawnPoint(in.link, in.turn, kBossAheadM);
            asked_ms_ = in.now_ms;
            actor_ = 0;
            raised_ = false;
            died_ = false;
            players_ = 1 + in.in_field_too.size();
            phase_ = Phase::asking;
            out.lines.push_back("fieldboss: night falls on the field: its guardian comes (" + names[candidate_] + ")");
        }
        break;
    case Phase::asking:
        if (!in.in_field) {
            phase_ = Phase::idle;
            wait_until_ms_ = in.now_ms + kGoneWaitMs;
            out.lines.push_back("fieldboss: you left the field before its guardian came");
        } else if (in.actor != 0) {
            actor_ = in.actor;
            phase_ = Phase::alive;
            out.lines.push_back("fieldboss: the field's guardian is here: " + names[candidate_] + " (actor " +
                                Hex(actor_) + ")");
        } else if (in.now_ms - asked_ms_ >= kCreateWaitMs) {
            out.lines.push_back("fieldboss: " + names[candidate_] + " could not be made in the field: not tried again");
            ++candidate_;
            phase_ = Phase::idle;
            if (candidate_ >= names.size()) {
                off_ = true;
                out.lines.push_back("fieldboss: none of the guardians could be made here: no field boss this run");
            }
        }
        break;
    case Phase::alive:
        if (in.actor != 0 && in.actor == actor_ && in.life && *in.life <= 0) died_ = true;
        if (in.actor != 0 && in.actor == actor_ && !raised_ && in.life && in.max_life && *in.life > 0 &&
            *in.max_life > 0) {
            const std::int64_t target = static_cast<std::int64_t>(*in.max_life) * static_cast<std::int64_t>(3 + players_);
            const std::int32_t to = static_cast<std::int32_t>(
                (std::min<std::int64_t>)(target, (std::numeric_limits<std::int32_t>::max)()));
            out.raise_to = to;
            raised_ = true;
            out.lines.push_back("fieldboss: its life raised from " + std::to_string(*in.max_life) + " to " +
                                std::to_string(to) + " (" + std::to_string(players_) + " player(s) in the field)");
        }
        if (in.actor == 0 || in.actor != actor_) {
            if (died_) {
                std::random_device rd;
                std::uint32_t event = 0;
                while (event == 0) event = rd();
                const std::uint64_t seed = (static_cast<std::uint64_t>(rd()) << 32) ^ rd();
                out.loot = LootValue(event, seed, kLootTable);
                out.lines.push_back("fieldboss: the field's guardian has fallen! Loot for everyone here (event " +
                                    std::to_string(event) + ")");
                wait_for_day_ = true;
            } else {
                out.lines.push_back("fieldboss: the field's guardian went away without falling");
            }
            wait_until_ms_ = in.now_ms + kGoneWaitMs;
            phase_ = Phase::idle;
            actor_ = 0;
        } else if (!in.in_field) {
            out.lines.push_back("fieldboss: you left the field: its guardian stays with the players there");
            wait_until_ms_ = in.now_ms + kGoneWaitMs;
            phase_ = Phase::idle;
            actor_ = 0;
        }
        break;
    }
    if (phase_ == Phase::asking || phase_ == Phase::alive) {
        out.want = Summoner::Want{key_, names[candidate_], at_};
    }
    return out;
}

// ---- the game side ----

namespace {

struct State {
    std::mutex mutex;
    Summoner summoner;
    std::vector<std::string> lines;
    std::optional<Command> pending;   // asked in the chat, made at the next frame (our Link is read there)
    bool active = false;              // (the director has been given a list since the last clear)
    Boss boss;
    std::optional<std::uint64_t> boss_key;   // the key the boss's want had last frame
};

bool BossOn() {
#ifdef _WIN32
    char* v = nullptr;
    std::size_t n = 0;
    const bool off = _dupenv_s(&v, &n, "NAVI_FIELD_BOSS") == 0 && v != nullptr && std::string(v) == "0";
    std::free(v);
    return !off;
#else
    const char* v = std::getenv("NAVI_FIELD_BOSS");
    return v == nullptr || std::string(v) != "0";
#endif
}

// The life of an actor (the objects sync's chain, docs/objects.md "Fields of a live actor"): its two value cells.
constexpr std::uint64_t kActorComponents = 0x270, kComponentLife = 0x2b8, kLifeValue = 0x1c10, kLifeMax = 0x1c18;

std::optional<std::uint64_t> Ptr(const Memory& m, std::uint64_t at) {
    std::uint64_t v = 0;
    if (at < 0x1000 || !m.read(at, &v, sizeof v) || v < 0x1000) return std::nullopt;
    return v;
}

std::optional<std::pair<std::uint64_t, std::uint64_t>> LifeCells(const Memory& m, std::uint64_t actor) {
    const auto table = Ptr(m, actor + kActorComponents);
    const auto life = table ? Ptr(m, *table + kComponentLife) : std::nullopt;
    const auto value = life ? Ptr(m, *life + kLifeValue) : std::nullopt;
    const auto max = life ? Ptr(m, *life + kLifeMax) : std::nullopt;
    if (!value || !max) return std::nullopt;
    return std::make_pair(*value + 8, *max + 8);
}

std::optional<std::int32_t> ReadS32(const Memory& m, std::uint64_t at) {
    std::int32_t v = 0;
    if (!m.read(at, &v, sizeof v)) return std::nullopt;
    return v;
}

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

void Frame(std::uint64_t main_base, const std::optional<coop::host::PlayerReading>& reading, std::uint64_t now_ms,
           std::uint8_t me, const std::vector<OtherPlayer>& others, const Memory* memory,
           std::optional<std::uint32_t> clock) {
    State& s = S();
    std::vector<CoopSpawn::Copy> copies;
    static const bool boss_on = BossOn();
    const bool in_field = main_base != 0 && reading && reading->scene && IsFieldScene(*reading->scene);
    g_in_field.store(in_field, std::memory_order_relaxed);
    {
        std::lock_guard lock(s.mutex);
        // The field's guardian.
        std::optional<Summoner::Want> boss_want;
        if (boss_on && main_base != 0 && reading) {
            Boss::In in;
            in.in_field = in_field;
            in.night = clock && IsNight(*clock);
            in.busy = reading->event_running.value_or(false) || reading->in_menu.value_or(false) ||
                      reading->dead.value_or(false);
            in.me = me;
            for (const auto& o : others) {
                if (o.number != me && o.scene && IsFieldScene(*o.scene)) in.in_field_too.push_back(o.number);
            }
            in.link = {reading->position[0], reading->position[1], reading->position[2]};
            in.turn = reading->rotation;
            in.now_ms = now_ms;
            if (s.boss_key) {
                const auto live = CoopSpawn::TheDirector().Live();
                if (const auto it = live.find(*s.boss_key); it != live.end()) in.actor = it->second;
            }
            std::optional<std::pair<std::uint64_t, std::uint64_t>> cells;
            if (in.actor != 0 && memory != nullptr && memory->read) {
                cells = LifeCells(*memory, in.actor);
                if (cells) {
                    in.life = ReadS32(*memory, cells->first);
                    in.max_life = ReadS32(*memory, cells->second);
                }
            }
            Boss::Out out = s.boss.Step(in);
            if (out.raise_to && cells && memory->write) {
                const std::int32_t v = *out.raise_to;
                const bool ok = memory->write(cells->second, &v, sizeof v) && memory->write(cells->first, &v, sizeof v);
                out.lines.push_back(ok ? "fieldboss: life written" : "fieldboss: life NOT written (memory refused)");
            }
            if (out.loot) {
                const bool shared = CoopModData::Set("navi-rpg", "bossloot", *out.loot);
                out.lines.push_back(shared ? "fieldboss: loot event shared with Navi RPG"
                                           : "fieldboss: loot event NOT shared (Navi RPG's values are full)");
            }
            for (auto& line : out.lines) Say(s, std::move(line));
            boss_want = out.want;
        }
        s.boss_key = boss_want ? std::optional<std::uint64_t>(boss_want->key) : std::nullopt;
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
        if (s.summoner.Count() == 0 && !boss_want && !s.active) return;   // (nothing asked: the director is not touched)
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
        if (boss_want) {
            CoopSpawn::Copy c;
            c.key = boss_want->key;
            c.actor = boss_want->actor;
            c.position = {boss_want->position.x, boss_want->position.y, boss_want->position.z};
            c.act = CoopSpawn::Act::show;
            c.place = false;
            c.remakes = 0;
            c.real = true;
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
