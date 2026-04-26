#include "Memory.h"

namespace umvc3 {

bool SafeReadMem(const void* src, void* dst, size_t size) {
    __try {
        memcpy(dst, src, size);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SafeWriteMem(void* dst, const void* src, size_t size) {
    __try {
        memcpy(dst, src, size);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SafeWriteProtected(uint64_t addr, const void* src, size_t size) {
    DWORD oldProtect = 0;
    if (!VirtualProtect((void*)addr, size, PAGE_EXECUTE_READWRITE, &oldProtect))
        return false;

    bool ok = SafeWriteMem((void*)addr, src, size);
    FlushInstructionCache(GetCurrentProcess(), (void*)addr, size);

    DWORD restoreProtect = 0;
    VirtualProtect((void*)addr, size, oldProtect, &restoreProtect);
    return ok;
}

bool IsReadable(uint64_t addr, size_t size) {
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery((void*)addr, &mbi, sizeof(mbi)) == 0) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return true;
}

bool SafeReadPtr(uint64_t addr, uint64_t* out) {
    return SafeRead<uint64_t>(addr, out);
}

bool CaptureRegion(uint64_t addr, size_t size, MemoryRegion* out) {
    out->addr = addr;
    if (out->bytes.capacity() < size) {
        out->bytes.reserve(size);
    }
    out->bytes.resize(size);
    return SafeReadMem((const void*)addr, out->bytes.data(), size);
}

bool RestoreRegion(const MemoryRegion& region, bool skipIfEmpty) {
    if (region.bytes.empty()) return skipIfEmpty;
    if (region.addr == 0) return skipIfEmpty;
    if (!IsReadable(region.addr, region.bytes.size())) return false;
    return SafeWriteMem((void*)region.addr, region.bytes.data(), region.bytes.size());
}

uint64_t QueryPerformanceMicros() {
    LARGE_INTEGER freq, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    return static_cast<uint64_t>(now.QuadPart * 1000000ULL / freq.QuadPart);
}

} // namespace umvc3
