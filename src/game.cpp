#include "game.hpp"

#include <algorithm>

namespace ms {

namespace {

constexpr wchar_t kExeName[] = L"MoonSweepers.exe";
constexpr uintptr_t kArrayLength = 0x18;   // Il2CppArray::max_length
constexpr uintptr_t kArrayData = 0x20;     // first element
constexpr uintptr_t kMonitor = 0x8;        // object header: klass, monitor

template <class T>
T value_at(const Process& p, uintptr_t address, T fallback = T{}) {
    T v = fallback;
    p.read_value(address, v);
    return v;
}

// Picks the class candidate that has all the fields we rely on.
const il2cpp::Class* pick(const std::map<std::string, std::vector<il2cpp::Class>>& index,
                          const char* name, std::initializer_list<const char*> required) {
    const auto it = index.find(name);
    if (it == index.end()) {
        return nullptr;
    }
    for (const il2cpp::Class& c : it->second) {
        if (c.has_fields(required)) {
            return &c;
        }
    }
    return nullptr;
}

}  // namespace

std::shared_ptr<Game> Game::attach(std::string& error) {
    std::shared_ptr<Game> g(new Game());
    const DWORD pid = Process::find_pid(kExeName);
    if (pid == 0) {
        error = "MoonSweepers.exe is not running";
        return nullptr;
    }
    if (!g->process_.open(pid)) {
        error = "cannot open the game process (error " + std::to_string(GetLastError()) +
                "); try running as administrator";
        return nullptr;
    }
    const Process& p = g->process_;

    const auto classes = il2cpp::find_classes(
        p, {"TeamWallet", "MoonWorld", "MoonWalker", "MoonLandmarks", "MoonLandmark"});
    const il2cpp::Class* wallet = pick(classes, "TeamWallet", {"Balance"});
    const il2cpp::Class* world = pick(classes, "MoonWorld", {"landmarks", "landmarkDirections", "wallets"});
    const il2cpp::Class* walker = pick(classes, "MoonWalker", {"world", "previousPosition", "heading"});
    const il2cpp::Class* all = pick(classes, "MoonLandmarks", {"all"});
    const il2cpp::Class* landmark =
        pick(classes, "MoonLandmark", {"name", "latitude", "longitude", "kind", "index"});
    if (wallet == nullptr || world == nullptr || walker == nullptr || all == nullptr ||
        landmark == nullptr) {
        error = "game classes not found: load into a game session first (not only the main menu)";
        return nullptr;
    }

    // MoonLandmarks.all: static array with every flag and site.
    const uintptr_t all_array = value_at<uint64_t>(p, all->static_fields + all->offset_of("all"));
    const uint64_t all_count = value_at<uint64_t>(p, all_array + kArrayLength);
    if (all_array == 0 || all_count == 0 || all_count > 5000) {
        error = "landmark table not found";
        return nullptr;
    }

    // Instances of MoonWorld and MoonWalker: the class pointer sits in the object header.
    const auto instances = il2cpp::find_instances(p, {world->klass, walker->klass});
    const int32_t world_landmarks = world->offset_of("landmarks");
    for (const uintptr_t obj : instances.count(world->klass) ? instances.at(world->klass)
                                                              : std::vector<uintptr_t>{}) {
        const uintptr_t arr = value_at<uint64_t>(p, obj + world_landmarks);
        if (value_at<uint64_t>(p, obj + kMonitor) == 0 && arr != 0 &&
            value_at<uint64_t>(p, arr + kArrayLength) == all_count) {
            g->world_ = obj;
            break;
        }
    }
    if (g->world_ == 0) {
        error = "MoonWorld not found: load into a game session first";
        return nullptr;
    }
    const int32_t walker_world = walker->offset_of("world");
    for (const uintptr_t obj : instances.count(walker->klass) ? instances.at(walker->klass)
                                                              : std::vector<uintptr_t>{}) {
        if (value_at<uint64_t>(p, obj + kMonitor) == 0 &&
            value_at<uint64_t>(p, obj + walker_world) == g->world_) {
            g->walker_ = obj;
            break;
        }
    }
    if (g->walker_ == 0) {
        error = "player (MoonWalker) not found";
        return nullptr;
    }
    g->position_offset_ = walker->offset_of("previousPosition");
    g->heading_offset_ = walker->offset_of("heading");

    // The wallet the world really uses.
    g->wallet_ = value_at<uint64_t>(p, g->world_ + world->offset_of("wallets"));
    g->wallet_class_ = wallet->klass;
    g->balance_offset_ = wallet->offset_of("Balance");
    if (g->wallet_ == 0 || value_at<uint64_t>(p, g->wallet_) != g->wallet_class_) {
        error = "team wallet not found";
        return nullptr;
    }

    // Static prices and rewards.
    g->world_statics_ = world->static_fields;
    g->teleport_price_offset_ = world->offset_of("TeleportPrice");
    g->flag_reward_offset_ = world->offset_of("FlagReward");
    g->site_reward_offset_ = world->offset_of("SiteReward");

    // Landmarks: names/coordinates from the objects, unit directions from the world.
    const uintptr_t dirs = value_at<uint64_t>(p, g->world_ + world->offset_of("landmarkDirections"));
    const uint64_t dir_count = dirs != 0 ? value_at<uint64_t>(p, dirs + kArrayLength) : 0;
    for (uint64_t i = 0; i < all_count; ++i) {
        const uintptr_t obj = value_at<uint64_t>(p, all_array + kArrayData + i * 8);
        if (obj == 0) {
            continue;
        }
        Landmark lm;
        lm.code = il2cpp::read_string(p, value_at<uint64_t>(p, obj + landmark->offset_of("code")));
        lm.name = il2cpp::read_string(p, value_at<uint64_t>(p, obj + landmark->offset_of("name")));
        lm.lat = value_at<float>(p, obj + landmark->offset_of("latitude"));
        lm.lon = value_at<float>(p, obj + landmark->offset_of("longitude"));
        lm.flag = value_at<int32_t>(p, obj + landmark->offset_of("kind")) == 0;
        lm.index = value_at<int32_t>(p, obj + landmark->offset_of("index"));
        if (lm.index >= 0 && static_cast<uint64_t>(lm.index) < dir_count) {
            p.read(dirs + kArrayData + static_cast<uintptr_t>(lm.index) * sizeof(Vec3), &lm.dir, sizeof(Vec3));
        }
        if (lm.name.empty()) {
            lm.name = lm.code;
        }
        g->landmarks_.push_back(std::move(lm));
    }
    if (g->landmarks_.empty()) {
        error = "no landmarks read";
        return nullptr;
    }
    return g;
}

bool Game::read_balance(Decimal& out) const {
    Decimal d;
    if (value_at<uint64_t>(process_, wallet_) != wallet_class_ ||
        !process_.read_value(wallet_ + balance_offset_, d) || !d.valid()) {
        return false;
    }
    out = d;
    return true;
}

bool Game::write_balance(const Decimal& value) const {
    return value.valid() && value_at<uint64_t>(process_, wallet_) == wallet_class_ &&
           process_.write(wallet_ + balance_offset_, &value, sizeof(value));
}

uintptr_t Game::setting_address(Setting which) const {
    int32_t offset = -1;
    switch (which) {
        case Setting::TeleportPrice: offset = teleport_price_offset_; break;
        case Setting::FlagReward: offset = flag_reward_offset_; break;
        case Setting::SiteReward: offset = site_reward_offset_; break;
    }
    return (offset < 0 || world_statics_ == 0) ? 0 : world_statics_ + static_cast<uintptr_t>(offset);
}

bool Game::read_setting(Setting which, Decimal& out) const {
    const uintptr_t a = setting_address(which);
    Decimal d;
    if (a == 0 || !process_.read_value(a, d) || !d.valid()) {
        return false;
    }
    out = d;
    return true;
}

bool Game::write_setting(Setting which, const Decimal& value) const {
    const uintptr_t a = setting_address(which);
    return a != 0 && value.valid() && process_.write(a, &value, sizeof(value));
}

bool Game::player(Vec3& position, Vec3& heading) const {
    return process_.read_value(walker_ + position_offset_, position) &&
           process_.read_value(walker_ + heading_offset_, heading);
}

}  // namespace ms
