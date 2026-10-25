#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
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

    template <class T>
    bool read_value(uintptr_t address, T& out) const {
        return read(address, &out, sizeof(T));
    }

    // Addresses where `pattern` occurs and (address % align == 0).
    // writable_only limits the scan to writable pages (heap, statics).
    std::vector<uintptr_t> find_all(const void* pattern, size_t length, size_t align,
                                    bool writable_only) const;

    // Same for several byte patterns in one pass over memory. Result[i] belongs to patterns[i].
    std::vector<std::vector<uintptr_t>> find_patterns(const std::vector<std::string>& patterns,
                                                      size_t align, bool writable_only) const;

    // Every 8-aligned qword equal to one of `values`: (address, value) pairs.
    std::vector<std::pair<uintptr_t, uint64_t>> find_qwords(const std::vector<uint64_t>& values,
                                                            bool writable_only) const;

private:
    // Calls fn(base, data, own, n) for chunks of every matching region. `data` holds n bytes;
    // the first `own` bytes belong to the chunk, the rest is overlap into the next chunk.
    using ChunkFn = std::function<void(uintptr_t base, const unsigned char* data, size_t own, size_t n)>;
    void for_each_chunk(bool writable_only, size_t overlap, const ChunkFn& fn) const;

    HANDLE handle_ = nullptr;
};

}  // namespace ms
