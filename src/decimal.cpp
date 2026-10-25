#include "decimal.hpp"

#include <algorithm>
#include <cctype>

namespace ms {

namespace {

// 96-bit unsigned magnitude, little-endian 32-bit limbs.
struct U96 {
    uint32_t w[3];
};

U96 magnitude(const Decimal& d) {
    return {{static_cast<uint32_t>(d.lo), static_cast<uint32_t>(d.lo >> 32), d.hi}};
}

uint32_t divmod(U96& v, uint32_t divisor) {
    uint64_t rem = 0;
    for (int i = 2; i >= 0; --i) {
        const uint64_t cur = (rem << 32) | v.w[i];
        v.w[i] = static_cast<uint32_t>(cur / divisor);
        rem = cur % divisor;
    }
    return static_cast<uint32_t>(rem);
}

// v = v * mul + add; false on 96-bit overflow.
bool mul_add(U96& v, uint32_t mul, uint32_t add) {
    uint64_t carry = add;
    for (int i = 0; i < 3; ++i) {
        const uint64_t cur = static_cast<uint64_t>(v.w[i]) * mul + carry;
        v.w[i] = static_cast<uint32_t>(cur);
        carry = cur >> 32;
    }
    return carry == 0;
}

bool is_zero(const U96& v) { return v.w[0] == 0 && v.w[1] == 0 && v.w[2] == 0; }

bool equal(const U96& a, const U96& b) {
    return a.w[0] == b.w[0] && a.w[1] == b.w[1] && a.w[2] == b.w[2];
}

}  // namespace

std::string to_string(const Decimal& d) {
    U96 v = magnitude(d);
    std::string digits;  // least significant digit first
    while (!is_zero(v)) {
        digits.push_back(static_cast<char>('0' + divmod(v, 10)));
    }
    const size_t scale = d.scale();
    while (digits.size() < scale + 1) {
        digits.push_back('0');
    }
    if (scale > 0) {
        digits.insert(scale, 1, '.');
    }
    if (d.negative()) {
        digits.push_back('-');
    }
    std::reverse(digits.begin(), digits.end());
    return digits;
}

bool parse_decimal(const std::string& text, Decimal& out) {
    U96 v{{0, 0, 0}};
    unsigned frac_digits = 0;
    unsigned significant = 0;
    bool seen_digit = false;
    bool seen_point = false;

    for (const char ch : text) {
        if (ch == ' ' || ch == '_' || ch == '\t' || ch == '\r' || ch == '\n') {
            continue;
        }
        if (ch == '.' || ch == ',') {
            if (seen_point) {
                return false;
            }
            seen_point = true;
            continue;
        }
        if (!std::isdigit(static_cast<unsigned char>(ch))) {
            return false;
        }
        seen_digit = true;
        if (significant > 0 || ch != '0') {
            ++significant;
        }
        if (significant > 28) {
            return false;
        }
        if (!mul_add(v, 10, static_cast<uint32_t>(ch - '0'))) {
            return false;
        }
        if (seen_point) {
            ++frac_digits;
        }
    }
    if (!seen_digit || frac_digits > 28) {
        return false;
    }

    out.flags = frac_digits << 16;
    out.hi = v.w[2];
    out.lo = (static_cast<uint64_t>(v.w[1]) << 32) | v.w[0];
    return true;
}

bool display_matches(const Decimal& actual, const Decimal& typed) {
    if (!actual.valid() || actual.negative() != typed.negative()) {
        return false;
    }
    U96 a = magnitude(actual);
    const U96 t = magnitude(typed);
    const unsigned as = actual.scale();
    const unsigned ts = typed.scale();

    if (as > ts) {
        for (unsigned i = 0; i < as - ts; ++i) {
            divmod(a, 10);  // truncate extra fractional digits
        }
    } else {
        for (unsigned i = 0; i < ts - as; ++i) {
            if (!mul_add(a, 10, 0)) {
                return false;
            }
        }
    }
    return equal(a, t);
}

}  // namespace ms
