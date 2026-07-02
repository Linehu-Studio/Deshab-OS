#ifndef UTSM_H
#define UTSM_H

#include <utsm/types.h>
#include <utsm/status.h>
#include <utsm/config.h>
#include <utsm/uuid.h>
#include <utsm/process.h>
#include <utsm/segment.h>
#include <utsm/capability.h>
#include <utsm/utrw.h>

void utsm_init(void);
int utsm_selftest_run(void);

#endif
