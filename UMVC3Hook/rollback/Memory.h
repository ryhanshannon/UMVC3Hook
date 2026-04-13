#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>
#include <windows.h>

namespace umvc3 {

// SEH-protected memory operations for crash safety when reading/writing
// game memory that may become invalid during rollback or state transitions.

bool SafeReadMem(const void* src, void* dst, size_t size);
bool SafeWriteMem(void* dst, const void* src, size_t size);
bool SafeWriteProtected(uint64_t addr, const void* src, size_t size);
bool IsReadable(uint64_t addr, size_t size = 8);

template<typename T>
bool SafeRead(uint64_t addr, T* out) {
    return SafeReadMem((const void*)addr, out, sizeof(T));
}

template<typename T>
bool SafeWrite(uint64_t addr, const T& value) {
    return SafeWriteMem((void*)addr, &value, sizeof(T));
}

bool SafeReadPtr(uint64_t addr, uint64_t* out);

// A contiguous block of game memory captured for snapshot purposes.
struct MemoryRegion {
    uint64_t             addr = 0;
    std::vector<uint8_t> bytes;
};

// Capture a region of game memory into a MemoryRegion.
bool CaptureRegion(uint64_t addr, size_t size, MemoryRegion* out);

// Restore a previously captured region back to game memory.
// Returns false if the region is empty or the write fails.
// skipIfEmpty: if true, returns true for empty regions (used for optional state).
bool RestoreRegion(const MemoryRegion& region, bool skipIfEmpty = false);

// Microsecond-precision timer for benchmarking save/load performance.
uint64_t QueryPerformanceMicros();

} // namespace umvc3
