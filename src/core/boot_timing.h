#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

// Boot-time measurement, see boot_timing.c.
//
// Opt-in: create an empty BOOT_TIMING.log in the SD card root and each boot
// appends a report to it (same convention as HDZGOGGLE.log / RECORD.log).

// Stamp `tag` (a string literal -- only the pointer is kept) with the time
// since kernel start. Cheap and thread-safe; a no-op once the report is out.
void boot_mark(const char *tag);

// Feed the per-tick "is the current source valid" result; the first valid
// one is the boot's time-to-video.
void boot_timing_signal(bool is_valid, int source);

// Call once boot is done: starts the background report writer if enabled.
void boot_timing_start(void);

// Experimental boot optimizations under test are opt-in per SD card flag
// file: true if <SD>/bootopt/<name> exists. Results are cached and listed in
// the report, so every report says which variant was measured.
bool boot_opt(const char *name);

#ifdef __cplusplus
}
#endif
