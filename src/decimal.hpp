#pragma once

#include <cstdint>
#include <string>

namespace ms {

// In-memory layout of System.Decimal on little-endian targets:
// flags (sign bit 31, scale bits 16-23), hi32, lo64 (low and mid 32-bit words).
struct Decimal {
    uint32_t flags = 0;
    uint32_t hi = 0;
    uint64_t lo = 0;

    unsigned scale() const { return (flags >> 16) & 0xFFu; }
    bool negative() const { return (flags >> 31) != 0; }
    bool valid() const { return (flags & 0x7F00FFFFu) == 0 && scale() <= 28; }
};
static_assert(sizeof(Decimal) == 16, "Decimal must match System.Decimal");

// "1234.50"-style text for a valid Decimal.
std::string to_string(const Decimal& d);

// Parses a non-negative amount such as "10000", "1 234,5" or "0.25".
// Accepts '.' or ',' as separator, ignores spaces and '_'. At most 28 digits.
bool parse_decimal(const std::string& text, Decimal& out);

// True if `actual`, cut to the number of fractional digits of `typed`, equals `typed`.
// Used to recognise a wallet from the amount shown on screen.
bool display_matches(const Decimal& actual, const Decimal& typed);

}  // namespace ms
