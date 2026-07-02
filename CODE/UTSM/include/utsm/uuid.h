#ifndef UTSM_UUID_H
#define UTSM_UUID_H

#include <utsm/types.h>

typedef struct {
    u64 hi;
    u64 lo;
} uuid128_t;

static inline uuid128_t uuid_make_test(u64 seed) {
    uuid128_t id;
    id.hi = 0x5554534d00000000ULL ^ (seed * 0x9e3779b97f4a7c15ULL);
    id.lo = 0x4453484200000000ULL ^ (seed + 0xd1b54a32d192ed03ULL);
    return id;
}

static inline bool uuid_equal(uuid128_t a, uuid128_t b) {
    return a.hi == b.hi && a.lo == b.lo;
}

#endif
