#include "process.hpp"

#include <algorithm>
#include <cstring>
#include <cwchar>

#include <tlhelp32.h>

namespace ms {

namespace {

constexpr size_t kChunk = 32u * 1024u * 1024u;

constexpr DWORD kReadable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                            PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
constexpr DWORD kWritable =
    PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;

}  // namespace

Process::~Process() {
    if (handle_ != nullptr) {
        CloseHandle(handle_);
    }
}

DWORD Process::find_pid(const wchar_t* exe_name) {
    const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return 0;
    }
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    DWORD pid = 0;
    for (BOOL ok = Process32FirstW(snap, &entry); ok; ok = Process32NextW(snap, &entry)) {
        if (_wcsicmp(entry.szExeFile, exe_name) == 0) {
            pid = entry.th32ProcessID;
            break;
        }
    }
    CloseHandle(snap);
    return pid;
}

bool Process::open(DWORD pid) {
    handle_ = OpenProcess(PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION |
                              PROCESS_QUERY_INFORMATION | SYNCHRONIZE,
                          FALSE, pid);
    return handle_ != nullptr;
}

bool Process::alive() const {
    return handle_ != nullptr && WaitForSingleObject(handle_, 0) == WAIT_TIMEOUT;
}

bool Process::read(uintptr_t address, void* buffer, size_t size) const {
    SIZE_T done = 0;
    return ReadProcessMemory(handle_, reinterpret_cast<LPCVOID>(address), buffer, size, &done) &&
           done == size;
}

bool Process::write(uintptr_t address, const void* data, size_t size) const {
    SIZE_T done = 0;
    return WriteProcessMemory(handle_, reinterpret_cast<LPVOID>(address), data, size, &done) &&
           done == size;
}

void Process::for_each_chunk(bool writable_only, size_t overlap, const ChunkFn& fn) const {
    const DWORD wanted = writable_only ? kWritable : kReadable;
    std::vector<unsigned char> buf;

    uintptr_t cursor = 0;
    MEMORY_BASIC_INFORMATION mbi{};
    while (VirtualQueryEx(handle_, reinterpret_cast<LPCVOID>(cursor), &mbi, sizeof(mbi)) ==
           sizeof(mbi)) {
        const uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const size_t size = mbi.RegionSize;
        cursor = base + size;

        if (mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD) != 0 ||
            (mbi.Protect & wanted) == 0) {
            continue;
        }

        for (size_t off = 0; off < size; off += kChunk) {
            const size_t own = std::min(kChunk, size - off);
            const size_t n = std::min(own + overlap, size - off);
            buf.resize(n);
            if (!read(base + off, buf.data(), n)) {
                continue;
            }
            fn(base + off, buf.data(), own, n);
        }
    }
}

std::vector<std::vector<uintptr_t>> Process::find_patterns(const std::vector<std::string>& patterns,
                                                          size_t align, bool writable_only) const {
    std::vector<std::vector<uintptr_t>> hits(patterns.size());
    if (align == 0) {
        return hits;
    }
    size_t overlap = 0;
    for (const std::string& p : patterns) {
        if (!p.empty()) {
            overlap = std::max(overlap, p.size() - 1);
        }
    }

    for_each_chunk(writable_only, overlap,
                   [&](uintptr_t base, const unsigned char* data, size_t own, size_t n) {
        for (size_t k = 0; k < patterns.size(); ++k) {
            const std::string& pat = patterns[k];
            const size_t length = pat.size();
            if (length == 0 || n < length) {
                continue;
            }
            const unsigned char first = static_cast<unsigned char>(pat[0]);
            const unsigned char* p = data;
            const unsigned char* const end = data + (n - length + 1);
            while (p < end) {
                p = static_cast<const unsigned char*>(
                    std::memchr(p, first, static_cast<size_t>(end - p)));
                if (p == nullptr) {
                    break;
                }
                const size_t idx = static_cast<size_t>(p - data);
                if (idx < own && std::memcmp(p, pat.data(), length) == 0) {
                    const uintptr_t addr = base + idx;
                    if (addr % align == 0) {
                        hits[k].push_back(addr);
                    }
                }
                ++p;
            }
        }
    });
    return hits;
}

std::vector<uintptr_t> Process::find_all(const void* pattern, size_t length, size_t align,
                                         bool writable_only) const {
    const std::string pat(static_cast<const char*>(pattern), length);
    return find_patterns({pat}, align, writable_only)[0];
}

std::vector<std::pair<uintptr_t, uint64_t>> Process::find_qwords(
    const std::vector<uint64_t>& values, bool writable_only) const {
    std::vector<std::pair<uintptr_t, uint64_t>> hits;
    std::vector<uint64_t> sorted = values;
    std::sort(sorted.begin(), sorted.end());
    sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
    if (sorted.empty()) {
        return hits;
    }
    const uint64_t lo = sorted.front();
    const uint64_t hi = sorted.back();

    for_each_chunk(writable_only, 0,
                   [&](uintptr_t base, const unsigned char* data, size_t own, size_t) {
        const size_t words = own / sizeof(uint64_t);
        for (size_t i = 0; i < words; ++i) {
            uint64_t v;
            std::memcpy(&v, data + i * sizeof(uint64_t), sizeof(v));
            if (v < lo || v > hi) {
                continue;
            }
            if (std::binary_search(sorted.begin(), sorted.end(), v)) {
                hits.emplace_back(base + i * sizeof(uint64_t), v);
            }
        }
    });
    return hits;
}

}  // namespace ms
