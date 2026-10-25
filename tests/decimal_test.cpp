#include <cstdio>
#include <string>

#include "../src/decimal.hpp"

namespace {

int failures = 0;

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);  \
            ++failures;                                                  \
        }                                                                \
    } while (0)

ms::Decimal make(uint32_t flags, uint32_t hi, uint64_t lo) {
    ms::Decimal d;
    d.flags = flags;
    d.hi = hi;
    d.lo = lo;
    return d;
}

ms::Decimal parsed(const char* text) {
    ms::Decimal d;
    CHECK(ms::parse_decimal(text, d));
    return d;
}

}  // namespace

int main() {
    // Values taken from the running game.
    CHECK(ms::to_string(make(0x110000, 1, 0x02693BF7B3516300ull)) == "186.20480069391442688");
    CHECK(ms::to_string(make(0x20000, 0, 1000000)) == "10000.00");
    CHECK(ms::to_string(make(0x40000, 0, 95000)) == "9.5000");
    CHECK(ms::to_string(make(0, 0, 0)) == "0");
    CHECK(ms::to_string(make(0x20000, 0, 0)) == "0.00");
    CHECK(ms::to_string(make(0x80020000u, 0, 5)) == "-0.05");

    CHECK(make(0x110000, 1, 0).valid());
    CHECK(!make(0x1, 0, 0).valid());        // reserved bits set
    CHECK(!make(0x1D0000, 0, 0).valid());   // scale 29

    {
        const ms::Decimal d = parsed("10000");
        CHECK(d.scale() == 0 && d.hi == 0 && d.lo == 10000);
    }
    {
        const ms::Decimal d = parsed("1 234,5");
        CHECK(d.scale() == 1 && d.lo == 12345);
    }
    {
        const ms::Decimal d = parsed("0.25");
        CHECK(d.scale() == 2 && d.lo == 25);
    }
    {
        const ms::Decimal d = parsed("186.20480069391442688");
        CHECK(d.scale() == 17 && d.hi == 1 && d.lo == 0x02693BF7B3516300ull);
        CHECK(ms::to_string(d) == "186.20480069391442688");
    }

    ms::Decimal junk;
    CHECK(!ms::parse_decimal("", junk));
    CHECK(!ms::parse_decimal("abc", junk));
    CHECK(!ms::parse_decimal("1.2.3", junk));
    CHECK(!ms::parse_decimal("-5", junk));
    CHECK(!ms::parse_decimal("12345678901234567890123456789", junk));  // 29 digits

    // Matching the amount shown on screen against the stored value.
    const ms::Decimal wallet = make(0x110000, 1, 0x02693BF7B3516300ull);  // 186.2048...
    CHECK(ms::display_matches(wallet, parsed("186.20")));
    CHECK(ms::display_matches(wallet, parsed("186.2048")));
    CHECK(ms::display_matches(wallet, parsed("186")));
    CHECK(!ms::display_matches(wallet, parsed("186.21")));
    CHECK(!ms::display_matches(wallet, parsed("187")));
    CHECK(ms::display_matches(make(0x20000, 0, 1000000), parsed("10000")));
    CHECK(!ms::display_matches(make(0x20000, 0, 1000000), parsed("10000.5")));

    if (failures == 0) {
        std::puts("all decimal tests passed");
    }
    return failures == 0 ? 0 : 1;
}
