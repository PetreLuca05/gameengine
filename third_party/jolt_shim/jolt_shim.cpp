// C++ shim: isolated here so the rest of the game stays pure C.
#include <Jolt/Jolt.h>
#include "jolt_shim.h"

extern "C" int dgj_version(void) {
    return JPH_VERSION_MAJOR * 10000 + JPH_VERSION_MINOR * 100 + JPH_VERSION_PATCH;
}
