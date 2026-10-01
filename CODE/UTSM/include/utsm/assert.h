#ifndef UTSM_ASSERT_H
#define UTSM_ASSERT_H

#include <utsm/log.h>
#include <utsm/panic.h>

#define UTSM_ASSERT(cond) do { if (!(cond)) { panic("assertion failed: " #cond); } } while (0)

#endif
