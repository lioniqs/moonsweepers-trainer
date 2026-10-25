#include "il2cpp.hpp"

#include <algorithm>
#include <set>

namespace ms::il2cpp {

namespace {

// Offsets inside Il2CppClass for the Unity version this was written against.
constexpr uintptr_t kClassName = 0x10;
constexpr uintptr_t kClassStaticFields = 0x98;
constexpr uintptr_t kClassFieldsScanBegin = 0x40;
constexpr uintptr_t kClassFieldsScanEnd = 0x200;

// FieldInfo: name*, type*, parent*, offset (int32), token (uint32).
constexpr size_t kFieldInfoSize = 0x20;
constexpr uintptr_t kFieldParent = 0x10;
constexpr uintptr_t kFieldOffset = 0x18;

// Il2CppType: data*, then bit-field whose low 16 bits are the attributes.
constexpr uintptr_t kTypeBits = 0x8;
constexpr uint32_t kFieldAttrStatic = 0x10;

bool printable_identifier(const std::string& s) {
    if (s.empty()) {
        return false;
    }
    return std::all_of(s.begin(), s.end(), [](unsigned char c) { return c >= 0x20 && c < 0x7F; });
}

}  // namespace

int32_t Class::offset_of(const std::string& field) const {
    for (const Field& f : fields) {
        if (f.name == field) {
            return f.offset;
        }
    }
    return -1;
}

bool Class::has_fields(std::initializer_list<const char*> names) const {
    return std::all_of(names.begin(), names.end(),
                       [this](const char* n) { return offset_of(n) >= 0; });
}

std::string read_cstring(const Process& process, uintptr_t address, size_t max_length) {
    std::string out;
    if (address < 0x10000) {
        return out;
    }
    char buf[32];
    while (out.size() < max_length) {
        size_t n = std::min(sizeof(buf), max_length - out.size());
        if (!process.read(address + out.size(), buf, n)) {
            n = 1;  // near the end of a mapping: fall back to byte reads
            if (!process.read(address + out.size(), buf, 1)) {
                break;
            }
        }
        for (size_t i = 0; i < n; ++i) {
            if (buf[i] == '\0') {
                return out;
            }
            out.push_back(buf[i]);
        }
    }
    return out;
}

std::string read_string(const Process& process, uintptr_t string_object) {
    if (string_object < 0x10000) {
        return {};
    }
    int32_t length = 0;
    if (!process.read_value(string_object + 0x10, length) || length <= 0 || length > 1000) {
        return {};
    }
    std::vector<wchar_t> wide(static_cast<size_t>(length));
    static_assert(sizeof(wchar_t) == 2, "System.String is UTF-16 and wchar_t is 16-bit on Windows");
    if (!process.read(string_object + 0x14, wide.data(), wide.size() * sizeof(wchar_t))) {
        return {};
    }
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide.data(), length, nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) {
        return {};
    }
    std::string out(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), length, &out[0], bytes, nullptr, nullptr);
    return out;
}

bool read_class(const Process& process, uintptr_t klass, Class& out) {
    uint64_t name_ptr = 0;
    if (!process.read_value(klass + kClassName, name_ptr)) {
        return false;
    }
    Class c;
    c.klass = klass;
    c.name = read_cstring(process, static_cast<uintptr_t>(name_ptr));
    if (!printable_identifier(c.name)) {
        return false;
    }
    uint64_t statics = 0;
    process.read_value(klass + kClassStaticFields, statics);
    c.static_fields = static_cast<uintptr_t>(statics);

    // The FieldInfo array: some pointer inside the class whose entries name `klass` as their parent.
    for (uintptr_t off = kClassFieldsScanBegin; off < kClassFieldsScanEnd; off += 8) {
        uint64_t array = 0;
        if (!process.read_value(klass + off, array) || array < 0x10000) {
            continue;
        }
        uint64_t parent = 0;
        if (!process.read_value(static_cast<uintptr_t>(array) + kFieldParent, parent) || parent != klass) {
            continue;
        }
        for (size_t i = 0; i < 4096; ++i) {
            const uintptr_t fi = static_cast<uintptr_t>(array) + i * kFieldInfoSize;
            uint64_t fname = 0, ftype = 0, fparent = 0;
            int32_t foffset = 0;
            if (!process.read_value(fi, fname) || !process.read_value(fi + 8, ftype) ||
                !process.read_value(fi + kFieldParent, fparent) ||
                !process.read_value(fi + kFieldOffset, foffset) || fparent != klass) {
                break;
            }
            Field f;
            f.name = read_cstring(process, static_cast<uintptr_t>(fname));
            f.offset = foffset;
            uint32_t bits = 0;
            if (ftype != 0 && process.read_value(static_cast<uintptr_t>(ftype) + kTypeBits, bits)) {
                f.is_static = (bits & kFieldAttrStatic) != 0;
            }
            c.fields.push_back(std::move(f));
        }
        if (!c.fields.empty()) {
            out = std::move(c);
            return true;
        }
    }
    return false;
}

std::map<std::string, std::vector<Class>> find_classes(const Process& process,
                                                       const std::vector<std::string>& names) {
    // 1. The class-name strings (they start right after a NUL).
    std::vector<std::string> patterns;
    for (const std::string& n : names) {
        patterns.push_back(n + '\0');
    }
    const auto found = process.find_patterns(patterns, 1, false);

    std::map<uint64_t, std::string> name_at;  // string address -> class name
    for (size_t i = 0; i < names.size(); ++i) {
        for (const uintptr_t addr : found[i]) {
            unsigned char prev = 0xFF;
            if (addr > 0 && process.read(addr - 1, &prev, 1) && prev == 0) {
                name_at[addr] = names[i];
            }
        }
    }

    // 2. Il2CppClass::name points at those strings.
    std::vector<uint64_t> targets;
    for (const auto& kv : name_at) {
        targets.push_back(kv.first);
    }
    std::map<std::string, std::vector<Class>> result;
    std::set<uintptr_t> seen;
    for (const auto& hit : process.find_qwords(targets, true)) {
        const uintptr_t klass = hit.first - kClassName;
        if (!seen.insert(klass).second) {
            continue;
        }
        Class c;
        if (read_class(process, klass, c) && c.name == name_at[hit.second]) {
            result[c.name].push_back(std::move(c));
        }
    }
    return result;
}

std::map<uintptr_t, std::vector<uintptr_t>> find_instances(const Process& process,
                                                           const std::vector<uintptr_t>& klasses) {
    std::vector<uint64_t> targets(klasses.begin(), klasses.end());
    std::map<uintptr_t, std::vector<uintptr_t>> result;
    for (const auto& hit : process.find_qwords(targets, true)) {
        result[static_cast<uintptr_t>(hit.second)].push_back(hit.first);
    }
    return result;
}

}  // namespace ms::il2cpp
