#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include <windows.h>

namespace ms {

// Handle to another process with scan/read/write helpers.
class Process {
public:
    Process() = default;
    ~Process();
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;

    // Process id of the first process with this executable name, or 0.
    static DWORD find_pid(const wchar_t* exe_name);

    bool open(DWORD pid);
    bool alive() const;

    bool read(uintptr_t address, void* buffer, size_t size) const;
    bool write(uintptr_t address, const void* data, size_t size) const;

    // Addresses where `pattern` occurs and (address % align == 0).
    // writable_only limits the scan to writable pages (heap, statics).
    std::vector<uintptr_t> find_all(const void* pattern, size_t length, size_t align,
                                    bool writable_only) const;

private:
    HANDLE handle_ = nullptr;
};

}  // namespace ms
