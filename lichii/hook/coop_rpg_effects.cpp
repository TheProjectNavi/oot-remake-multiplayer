// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_rpg_effects.h"

#include <algorithm>

#include "coop_mod_data.h"

#ifdef LICHII_COOP
#include "common/logging.h"
#include "coop_actor_events.h"
#include "coop_dbno_body.h"
#endif

namespace CoopRpgEffects {

std::optional<Worn> DecodeWear(const std::vector<std::uint8_t>& b) {
    if (b.size() < 2 || b[0] != 1 || b.size() != 2u + 2u * b[1] || b[1] > 8) return std::nullopt;
    Worn w;
    for (std::size_t i = 0; i < b[1]; ++i) {
        const std::uint16_t item = static_cast<std::uint16_t>(b[2 + 2 * i] | (b[3 + 2 * i] << 8));
        w.vigor = w.vigor || item == kVigor;
        w.guardian = w.guardian || item == kGuardian;
        w.second_wind = w.second_wind || item == kSecondWind;
    }
    return w;
}

std::optional<Use> DecodeUse(const std::vector<std::uint8_t>& b) {
    if (b.size() != 7 || b[0] != 1) return std::nullopt;
    Use u;
    for (int i = 0; i < 4; ++i) u.count |= static_cast<std::uint32_t>(b[1 + i]) << (8 * i);
    u.item = static_cast<std::uint16_t>(b[5] | (b[6] << 8));
    return u;
}

Effects::Out Effects::Step(const In& in) {
    Out out;
    // A use not seen before (each game run's values start empty, so the first one seen is new too).
    if (in.use && (!last_use_ || *last_use_ != in.use->count)) {
        last_use_ = in.use->count;
        if (in.use->item == kTonic) {
            fill_ = true;
            out.lines.push_back("rpg: Fairy Tonic drunk: hearts to fill");
        }
    }
    if (!in.life || !in.max || *in.max <= 0) {
        last_life_ = in.life;
        return out;
    }
    const std::int32_t life = *in.life, max = *in.max;
    if (!started_) {
        started_ = true;
        vigor_at_ = wind_at_ = in.now_ms;
    }
    if (life <= 0) {   // (down or dead: down but not out's own rules; no timer runs meanwhile)
        back_.clear();
        last_life_ = life;
        vigor_at_ = wind_at_ = in.now_ms;
        return out;
    }
    // A hit taken: half of it back later.
    if (in.worn.guardian && last_life_ && *last_life_ > life) {
        back_.emplace_back(in.now_ms + kGuardianMs, (*last_life_ - life + 1) / 2);
    }
    if (!in.worn.guardian) back_.clear();
    if (last_life_ && *last_life_ > life && pending_to_ >= 0) {
        out.drop_ask = true;   // (an ask still waiting would heal the hit away: asked again from the new life)
        pending_to_ = -1;
    }
    last_life_ = life;
    if (pending_to_ >= 0 && (life >= pending_to_ || in.now_ms >= pending_until_)) pending_to_ = -1;

    std::int32_t to = std::max(life, pending_to_);
    if (fill_) {
        to = max;
        fill_ = false;
    }
    for (auto it = back_.begin(); it != back_.end();) {
        if (in.now_ms >= it->first) {
            to += it->second;
            it = back_.erase(it);
        } else {
            ++it;
        }
    }
    if (in.worn.vigor && life < max) {
        if (in.now_ms - vigor_at_ >= kVigorMs) {
            to += 1;
            vigor_at_ = in.now_ms;
        }
    } else {
        vigor_at_ = in.now_ms;   // (the first quarter comes kVigorMs after a hurt)
    }
    if (in.worn.second_wind && life <= kWindAtOrBelow && life < max) {
        if (in.now_ms - wind_at_ >= kWindMs) {
            to += 1;
            wind_at_ = in.now_ms;
        }
    } else {
        wind_at_ = in.now_ms;
    }
    to = std::min(to, max);
    if (to > life && to != pending_to_) {
        out.raise_to = to;
        pending_to_ = to;
        pending_until_ = in.now_ms + kPendingMs;
    }
    return out;
}

void Frame(std::optional<std::int32_t> life, std::optional<std::int32_t> max, std::uint64_t now_us) {
#ifdef LICHII_COOP
    static Effects effects;
    static bool said_no_step = false;
    Effects::In in;
    in.now_ms = now_us / 1000;
    in.life = life;
    in.max = max;
    if (const auto w = CoopModData::Ours("navi-rpg", "wear")) {
        if (const auto d = DecodeWear(*w)) in.worn = *d;
    }
    if (const auto u = CoopModData::Ours("navi-rpg", "use")) in.use = DecodeUse(*u);
    const auto out = effects.Step(in);
    for (const auto& line : out.lines) LOG_INFO(Core, "coop: {}", line);
    if (out.drop_ask) CoopDbno::Body::TheRaising().Drop();
    if (out.raise_to && in.life) {
        if (CoopActorEvents::LifeStepSeen().load(std::memory_order_relaxed)) {
            CoopDbno::Body::TheRaising().Start(*out.raise_to, now_us);   // (the game's heal; down but not out writes it if late)
        } else if (!said_no_step) {
            said_no_step = true;
            LOG_INFO(Core, "coop: rpg: no Life step seen in this game: Navi RPG's healing waits for it");
        }
    }
#else
    (void)life;
    (void)max;
    (void)now_us;
#endif
}

}  // namespace CoopRpgEffects
