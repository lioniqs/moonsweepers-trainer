#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "decimal.hpp"
#include "il2cpp.hpp"
#include "process.hpp"

namespace ms {

struct Vec3 {
    float x = 0, y = 0, z = 0;
};

// One entry of MoonLandmarks.all: a country flag or a mission landing site.
struct Landmark {
    int index = 0;
    bool flag = true;      // false: landing site
    std::string code;
    std::string name;
    float lat = 0;         // degrees
    float lon = 0;         // degrees
    Vec3 dir;              // unit vector from the Moon's centre to the landmark
};

enum class Setting { TeleportPrice, FlagReward, SiteReward };

// A snapshot of the running game, built once by attach(). Everything after that is a cheap
// read/write of known addresses, so the object can be shared between threads.
class Game {
public:
    // Scans the game's memory (takes a few seconds). Null and `error` set on failure.
    static std::shared_ptr<Game> attach(std::string& error);

    bool alive() const { return process_.alive(); }
    const std::vector<Landmark>& landmarks() const { return landmarks_; }

    // Team wallet (MoonWorld.wallets).
    bool read_balance(Decimal& out) const;
    bool write_balance(const Decimal& value) const;

    // Static prices/rewards of MoonWorld.
    bool read_setting(Setting which, Decimal& out) const;
    bool write_setting(Setting which, const Decimal& value) const;

    // Local player: last known position and facing direction (world space, Moon centre at origin).
    bool player(Vec3& position, Vec3& heading) const;

private:
    Game() = default;
    uintptr_t setting_address(Setting which) const;

    Process process_;
    std::vector<Landmark> landmarks_;

    uintptr_t world_ = 0;
    uintptr_t walker_ = 0;
    uintptr_t wallet_ = 0;
    uintptr_t wallet_class_ = 0;
    uintptr_t world_statics_ = 0;

    int32_t balance_offset_ = 0;
    int32_t teleport_price_offset_ = -1;
    int32_t flag_reward_offset_ = -1;
    int32_t site_reward_offset_ = -1;
    int32_t position_offset_ = 0;
    int32_t heading_offset_ = 0;
};

}  // namespace ms
