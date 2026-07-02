#ifndef UTSM_ARENA_H
#define UTSM_ARENA_H

#include <utsm/types.h>

void arena_init(void);
void *kmem_alloc(u64 size);
void *kmem_alloc_aligned(u64 size, u64 alignment);
void *kmem_alloc_page(u64 page_count);
u64 kmem_arena_virtual_base(void);
u64 kmem_arena_virtual_end(void);

#endif
