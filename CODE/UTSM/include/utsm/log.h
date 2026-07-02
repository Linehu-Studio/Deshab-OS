#ifndef UTSM_LOG_H
#define UTSM_LOG_H

#include <utsm/types.h>

void serial_init(void);
void serial_putc(char c);
void serial_write(const char *s);

void log_info(const char *msg);
void log_warn(const char *msg);
void log_error(const char *msg);
void log_hex64(const char *prefix, u64 value);
void panic(const char *msg);

#endif
