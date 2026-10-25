#include "storage.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <windows.h>

namespace ms {

namespace {

namespace fs = std::filesystem;

std::string sanitized(std::string s) {
    for (char& c : s) {
        if (c == '\t' || c == '\r' || c == '\n') {
            c = ' ';
        }
    }
    return s;
}

}  // namespace

std::wstring Storage::folder() const {
    wchar_t buf[MAX_PATH * 2];
    const DWORD n = GetEnvironmentVariableW(L"APPDATA", buf, static_cast<DWORD>(sizeof(buf) / sizeof(buf[0])));
    std::wstring base;
    if (n > 0 && n < sizeof(buf) / sizeof(buf[0])) {
        base.assign(buf, n);
    } else {
        const DWORD m = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(sizeof(buf) / sizeof(buf[0])));
        base = fs::path(std::wstring(buf, m)).parent_path().wstring();
    }
    return (fs::path(base) / L"MoonSweepersTrainer").wstring();
}

void Storage::load() {
    marks.clear();
    visited.clear();
    const fs::path dir(folder());

    if (std::ifstream in{dir / L"marks.txt"}) {
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            std::vector<std::string> parts;
            std::stringstream ss(line);
            std::string part;
            while (std::getline(ss, part, '\t')) {
                parts.push_back(part);
            }
            if (parts.size() < 4) {
                continue;
            }
            Mark m;
            m.name = parts[0];
            m.dir.x = std::strtof(parts[1].c_str(), nullptr);
            m.dir.y = std::strtof(parts[2].c_str(), nullptr);
            m.dir.z = std::strtof(parts[3].c_str(), nullptr);
            if (!m.name.empty()) {
                marks.push_back(std::move(m));
            }
        }
    }
    if (std::ifstream in{dir / L"visited.txt"}) {
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (!line.empty()) {
                visited.insert(line);
            }
        }
    }
}

bool Storage::save() const {
    const fs::path dir(folder());
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        return false;
    }
    bool ok = true;
    {
        std::ofstream out(dir / L"marks.txt", std::ios::trunc);
        for (const Mark& m : marks) {
            char buf[160];
            std::snprintf(buf, sizeof(buf), "\t%.6f\t%.6f\t%.6f\n", m.dir.x, m.dir.y, m.dir.z);
            out << sanitized(m.name) << buf;
        }
        ok = ok && static_cast<bool>(out);
    }
    {
        std::ofstream out(dir / L"visited.txt", std::ios::trunc);
        for (const std::string& key : visited) {
            out << sanitized(key) << '\n';
        }
        ok = ok && static_cast<bool>(out);
    }
    return ok;
}

}  // namespace ms
