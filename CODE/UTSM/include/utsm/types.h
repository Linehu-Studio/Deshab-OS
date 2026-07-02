#ifndef UTSM_TYPES_H
#define UTSM_TYPES_H

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;

typedef signed char i8;
typedef signed short i16;
typedef signed int i32;
typedef signed long long i64;

typedef u64 usize;
typedef i64 isize;

typedef int bool;
#define true 1
#define false 0

#ifndef NULL
#define NULL ((void *)0)
#endif

#define UTSM_UNUSED(x) ((void)(x))
#define UTSM_ARRAY_LEN(x) (sizeof(x) / sizeof((x)[0]))

static inline u64 utsm_align_up_u64(u64 value, u64 alignment) {
    return (value + alignment - 1ULL) & ~(alignment - 1ULL);
}

static inline void *utsm_align_up_ptr(void *ptr, u64 alignment) {
    return (void *)utsm_align_up_u64((u64)ptr, alignment);
}

#endif
