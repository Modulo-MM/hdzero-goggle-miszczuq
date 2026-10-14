#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

// Runtime CPU/power measurement, see perf_monitor.c.
//
// Opt-in: create an empty PERF.log in the SD card root and the app appends
// one line of statistics every PERF_INTERVAL_S seconds while it runs.

// Runtime optimizations, all on by default. Each can be turned off for a
// boot by an SD card flag file <SD>/bootopt/disable_<name> (boot_opt()), read
// once at startup, as a field fallback and to measure the difference;
// PERF.log and BOOT_TIMING.log list the flags that were set.
typedef struct {
    bool lazy_ui;      // "lazy_ui": touch LVGL objects only when a value changes
    bool dirty_render; // "dirty_render": redraw and copy only the changed areas
    bool imu_lazy;     // "imu_lazy": with head tracking off, read the IMU at 1 Hz
    bool native_reg;   // "native_reg": register writes via /dev/mem, not `aww`
    bool native_audio; // "native_audio": mixer controls via ioctl, not amixer
    bool dvr_async;    // "dvr_async": DVR start/stop on a worker thread
    bool fast_switch;  // "fast_switch": no fixed sleep(1) when switching to analog
} perf_opts_t;

extern perf_opts_t perf_opt;

// Read the disable flags. Call once, early in main() (after paths_init()).
void perf_opts_init(void);

// Start the background writer if PERF.log exists. Call once after init.
void perf_monitor_start(void);

// Name the calling thread (max 15 chars): shows up per thread in PERF.log,
// /proc/<pid>/task/*/comm and top -H.
void perf_thread_name(const char *name);

// Counters fed by the display driver (ui_porting.c): one LVGL refresh that
// took `ms` and rendered `px` pixels; one flush to the framebuffer.
void perf_note_refresh(uint32_t ms, uint32_t px);
void perf_note_flush(uint32_t bytes, uint32_t us);

// One shell command (system_exec/system_script) that took `us`.
void perf_note_shell(uint32_t us);

// Main-loop breakdown: t = perf_loop_now(); work(); t = perf_loop_section(s, t);
// adds the time since t to section s and returns the new start. Both return
// 0 and cost nothing while PERF.log is off.
typedef enum {
    PERF_LOOP_MENU,      // main_menu_update
    PERF_LOOP_STATUSBAR, // statubar_update
    PERF_LOOP_OSD,       // osd_hdzero_update
    PERF_LOOP_LVGL,      // lv_timer_handler: LVGL timers and rendering
    PERF_LOOP_OTHER,     // the rest of the pass, lvgl_mutex wait included
    PERF_LOOP_COUNT,
} perf_loop_section_t;

uint32_t perf_loop_now(void);
uint32_t perf_loop_section(perf_loop_section_t section, uint32_t since);
void perf_note_loop_pass(void);
void perf_note_periph_pass(void); // one work pass of the peripheral thread

#ifdef __cplusplus
}
#endif
