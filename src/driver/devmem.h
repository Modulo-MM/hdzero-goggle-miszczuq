#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

// SoC register access for the few registers the app pokes directly.
//
// The app used to run the `aww <addr> <value>` / `awr <addr>` tools for
// these: a shell plus a process per access. With native_reg (perf_opt) the
// access goes through a /dev/mem mapping in-process instead; with it off, or
// if /dev/mem cannot be mapped, reg_write32() runs `aww` as before and
// devmem_read32() fails so the caller can use its old path.
void reg_write32(uint32_t addr, uint32_t value);

// 0 and *value on success; -1 when native access is off or unavailable.
int devmem_read32(uint32_t addr, uint32_t *value);

#ifdef __cplusplus
}
#endif
