// C++ shim: isolated here so the rest of the game stays pure C.
#include <Jolt/Jolt.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Constraints/ContactConstraintManager.h>
#include <atomic>
#include <cstdlib>
#include "jolt_shim.h"

extern "C" int dgj_version(void) {
    return JPH_VERSION_MAJOR * 10000 + JPH_VERSION_MINOR * 100 + JPH_VERSION_PATCH;
}

extern "C" uint32_t dgj_contact_constraint_bytes(void) {
    return JPH::ContactConstraintManager::cMaxConstraintSize;
}

namespace {
// TempAllocatorImplWithMallocFallback, except that a failed fallback allocation stops with a message
// instead of handing Jolt a null buffer (Jolt never checks: the step then writes through it).
class DgTempAllocator final : public JPH::TempAllocator {
public:
    explicit DgTempAllocator(uint32_t size) : mFixed(size) {}
    void *Allocate(JPH::uint size) override {
        if (mFixed.CanAllocate(size)) return mFixed.Allocate(size);
        mFallbacks.fetch_add(1, std::memory_order_relaxed);
        void *p = mMalloc.Allocate(size);
        if (!p && size) {
            JPH::Trace("TempAllocator: out of memory allocating %u bytes", (unsigned)size);
            std::abort();
        }
        return p;
    }
    void Free(void *addr, JPH::uint size) override {
        if (!addr) return;
        if (mFixed.OwnsMemory(addr)) mFixed.Free(addr, size);
        else mMalloc.Free(addr, size);
    }
    JPH::TempAllocatorImpl mFixed;
    JPH::TempAllocatorMalloc mMalloc;
    std::atomic<uint32_t> mFallbacks{ 0 };
};
}

extern "C" JPH_TempAllocator *dgj_temp_allocator_create(uint32_t size) {
    DgTempAllocator *a = new DgTempAllocator(size);
    // TempAllocatorImpl does not report a failed block allocation; its first allocation then sits at null.
    void *probe = a->mFixed.Allocate(16);
    a->mFixed.Free(probe, 16);
    if (!probe) { delete a; return nullptr; }
    return reinterpret_cast<JPH_TempAllocator *>(static_cast<JPH::TempAllocator *>(a));
}

extern "C" void dgj_temp_allocator_destroy(JPH_TempAllocator *a) {
    delete static_cast<DgTempAllocator *>(reinterpret_cast<JPH::TempAllocator *>(a));
}

extern "C" uint32_t dgj_temp_allocator_fallbacks(const JPH_TempAllocator *a) {
    const JPH::TempAllocator *t = reinterpret_cast<const JPH::TempAllocator *>(a);
    return static_cast<const DgTempAllocator *>(t)->mFallbacks.load(std::memory_order_relaxed);
}
