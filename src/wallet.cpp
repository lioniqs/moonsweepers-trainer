#include "wallet.hpp"

#include <algorithm>
#include <set>

namespace ms {

namespace {

constexpr char kClassName[] = "TeamWallet";    // includes the terminating NUL
constexpr uintptr_t kNamePtrOffset = 0x10;     // Il2CppClass::name
constexpr uintptr_t kBalanceOffset = 0x10;     // first instance field after the object header

bool header_matches(const Process& process, const Wallet& wallet) {
    uintptr_t klass = 0;
    return process.read(wallet.object, &klass, sizeof(klass)) && klass == wallet.klass;
}

}  // namespace

std::vector<Wallet> find_wallets(const Process& process) {
    // 1. Name strings. The match must be at the start of a string (previous byte is NUL).
    std::vector<uintptr_t> names;
    for (const uintptr_t addr : process.find_all(kClassName, sizeof(kClassName), 1, false)) {
        unsigned char prev = 0xFF;
        if (addr > 0 && process.read(addr - 1, &prev, 1) && prev == 0) {
            names.push_back(addr);
        }
    }

    // 2. Classes: whatever stores a pointer to the name at the Il2CppClass::name slot.
    std::set<uintptr_t> classes;
    for (const uintptr_t name : names) {
        for (const uintptr_t slot : process.find_all(&name, sizeof(name), sizeof(name), true)) {
            classes.insert(slot - kNamePtrOffset);
        }
    }

    // 3. Instances.
    std::vector<Wallet> wallets;
    for (const uintptr_t klass : classes) {
        for (const uintptr_t obj : process.find_all(&klass, sizeof(klass), sizeof(klass), true)) {
            Wallet w;
            w.object = obj;
            w.klass = klass;
            w.balance = obj + kBalanceOffset;
            if (process.read(w.balance, &w.value, sizeof(w.value)) && w.value.valid()) {
                wallets.push_back(w);
            }
        }
    }
    std::sort(wallets.begin(), wallets.end(),
              [](const Wallet& a, const Wallet& b) { return a.object < b.object; });
    return wallets;
}

bool read_balance(const Process& process, const Wallet& wallet, Decimal& out) {
    Decimal d;
    if (!header_matches(process, wallet) || !process.read(wallet.balance, &d, sizeof(d)) ||
        !d.valid()) {
        return false;
    }
    out = d;
    return true;
}

bool write_balance(const Process& process, const Wallet& wallet, const Decimal& value) {
    return value.valid() && header_matches(process, wallet) &&
           process.write(wallet.balance, &value, sizeof(value));
}

}  // namespace ms
