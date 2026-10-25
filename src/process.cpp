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

std::vector<uintptr_t> Process::find_all(const void* pattern, size_t length, size_t align,
                                         bool writable_only) const {
    std::vector<uintptr_t> hits;
    if (length == 0 || align == 0) {
        return hits;
    }
    const auto* pat = static_cast<const unsigned char*>(pattern);
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
            // Read `length - 1` extra bytes so matches that straddle chunks are not missed;
            // only matches that start inside this chunk's own range are reported.
            const size_t own = std::min(kChunk, size - off);
            const size_t n = std::min(own + length - 1, size - off);
            if (n < length) {
                continue;
            }
            buf.resize(n);
            if (!read(base + off, buf.data(), n)) {
                continue;
            }
            const unsigned char* p = buf.data();
            const unsigned char* const end = p + (n - length + 1);
            while (p < end) {
                p = static_cast<const unsigned char*>(
                    std::memchr(p, pat[0], static_cast<size_t>(end - p)));
                if (p == nullptr) {
                    break;
                }
                const size_t idx = static_cast<size_t>(p - buf.data());
                if (idx < own && std::memcmp(p, pat, length) == 0) {
                    const uintptr_t addr = base + off + idx;
                    if (addr % align == 0) {
                        hits.push_back(addr);
                    }
                }
                ++p;
            }
        }
    }
    return hits;
}

}  // namespace ms
