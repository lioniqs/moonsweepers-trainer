#pragma once

#include <cstdint>
#include <vector>

#include "decimal.hpp"
#include "process.hpp"

namespace ms {

// An instance of the game's `TeamWallet` class (IL2CPP object).
struct Wallet {
    uintptr_t object = 0;   // address of the managed object
    uintptr_t klass = 0;    // Il2CppClass* stored in the object header
    uintptr_t balance = 0;  // address of the System.Decimal balance field
    Decimal value;          // balance when the wallet was found
};

// Finds TeamWallet instances by class name, independent of the current balance:
//   1. the ASCII name "TeamWallet" in memory,
//   2. a pointer to it at Il2CppClass::name (+0x10) gives the class,
//   3. objects whose first qword is that class pointer are the instances.
std::vector<Wallet> find_wallets(const Process& process);

// Both re-check that the object header still points at the class, so a freed or
// moved object is never touched.
bool read_balance(const Process& process, const Wallet& wallet, Decimal& out);
bool write_balance(const Process& process, const Wallet& wallet, const Decimal& value);

}  // namespace ms
