#include "core/boot_timing.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <log/log.h>

#include "core/common.hh"
#include "core/settings.h"
#include "platform/paths.h"
#include "util/filesystem.h"

// Boot-time measurement.
//
// boot_mark() stamps CLOCK_BOOTTIME -- seconds since kernel start, the clock
// behind /proc/uptime -- so the app's marks line up with the ones rc.sh writes
// to BOOT_TIMING_SHELL_LOG and with the dmesg timestamps. Marks are kept in
// RAM; the report is written once, from a background thread, a few seconds
// after the first valid video signal (or after BOOT_TIMING_TIMEOUT_S without
// one), so taking the measurement does not slow down the boot it measures.

#define BOOT_TIMING_FILE      "BOOT_TIMING.log"
#define BOOT_TIMING_SHELL_LOG "/tmp/boot_timing_sh.log"
#define BOOT_TIMING_MAX_MARKS 192
#define BOOT_TIMING_SETTLE_S  3  // keep marking this long after the first signal
#define BOOT_TIMING_TIMEOUT_S 60 // write the report even if no signal shows up
#define BOOT_TIMING_MAX_BYTES (2 * 1024 * 1024)
#define BOOT_TIMING_TAG_LEN   48
#define BOOT_OPT_DIR          "bootopt"
#define BOOT_OPT_MAX          16

#if defined(HDZGOGGLE2)
#define BOOT_TIMING_PLATFORM "HDZGOGGLE2"
#elif defined(HDZBOXPRO)
#define BOOT_TIMING_PLATFORM "HDZBOXPRO"
#elif defined(HDZGOGGLE)
#define BOOT_TIMING_PLATFORM "HDZGOGGLE"
#else
#define BOOT_TIMING_PLATFORM "unknown"
#endif

typedef struct {
    double t;
    const char *tag;
} boot_mark_t;

static boot_mark_t marks[BOOT_TIMING_MAX_MARKS];
static int mark_count = 0;
static int marks_dropped = 0;
static pthread_mutex_t mark_mutex = PTHREAD_MUTEX_INITIALIZER;
static atomic_bool marking_done = false;
static atomic_bool signal_seen = false;
static double signal_time = 0;
static int signal_source = -1;

typedef struct {
    const char *name;
    bool enabled;
} boot_opt_t;

static boot_opt_t opts[BOOT_OPT_MAX];
static int opt_count = 0;
static pthread_mutex_t opt_mutex = PTHREAD_MUTEX_INITIALIZER;

static double boot_now(void) {
    struct timespec ts;
#ifdef CLOCK_BOOTTIME
    if (clock_gettime(CLOCK_BOOTTIME, &ts) != 0)
#endif
        clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

void boot_mark(const char *tag) {
    if (marking_done)
        return;

    double t = boot_now();

    pthread_mutex_lock(&mark_mutex);
    if (mark_count < BOOT_TIMING_MAX_MARKS) {
        marks[mark_count].t = t;
        marks[mark_count].tag = tag;
        mark_count++;
    } else {
        marks_dropped++;
    }
    pthread_mutex_unlock(&mark_mutex);
}

// Runs after the dynamic loader has mapped and relocated every shared
// library, right before main(): splits "app_launch -> main_entry" into
// loading and the rest.
__attribute__((constructor)) static void boot_timing_constructor(void) {
    boot_mark("app_libs_loaded");
}

bool boot_opt(const char *name) {
    pthread_mutex_lock(&opt_mutex);
    for (int i = 0; i < opt_count; i++) {
        if (strcmp(opts[i].name, name) == 0) {
            bool enabled = opts[i].enabled;
            pthread_mutex_unlock(&opt_mutex);
            return enabled;
        }
    }

    char path[256];
    snprintf(path, sizeof(path), "%s/%s/%s", path_extsd(), BOOT_OPT_DIR, name);
    bool enabled = fs_file_exists(path);
    if (opt_count < BOOT_OPT_MAX) {
        opts[opt_count].name = name;
        opts[opt_count].enabled = enabled;
        opt_count++;
    }
    pthread_mutex_unlock(&opt_mutex);
    return enabled;
}

void boot_opt_summary(char *buf, int size) {
    int len = 0;
    buf[0] = 0;
    pthread_mutex_lock(&opt_mutex);
    for (int i = 0; i < opt_count && len < size; i++)
        len += snprintf(buf + len, size - len, "%s%s=%s", i ? " " : "", opts[i].name, opts[i].enabled ? "ON" : "off");
    pthread_mutex_unlock(&opt_mutex);
}

///////////////////////////////////////////////////////////////////////////////
// Sleep/wake timing

#define WAKE_MAX_MARKS 24
#define WAKE_TIMEOUT_S 30

static pthread_mutex_t wake_mutex = PTHREAD_MUTEX_INITIALIZER;
static double wake_sleep_at = 0;
static boot_mark_t wake_marks[WAKE_MAX_MARKS];
static int wake_mark_count = 0;
static int wake_generation = 0; // a newer wake retires an older report
static atomic_bool wake_active = false;
static atomic_bool wake_signal_seen = false;
// Set by the analog_rx_on / source_reentered mark: only signals seen after
// the source is back up count, not a detector value still left from before.
static atomic_bool wake_source_ready = false;
static int wake_source = -1;
static bool wake_fast = false;
// The same report also times a source switch made in video (no sleep):
// switch_from is the source it came from, -1 for a wake.
static int switch_from = -1;

void wake_timing_sleep(void) {
    wake_sleep_at = boot_now();
}

void wake_timing_mark(const char *tag) {
    if (!wake_active)
        return;
    double t = boot_now();
    pthread_mutex_lock(&wake_mutex);
    if (wake_mark_count < WAKE_MAX_MARKS) {
        wake_marks[wake_mark_count].t = t;
        wake_marks[wake_mark_count].tag = tag;
        wake_mark_count++;
    }
    pthread_mutex_unlock(&wake_mutex);

    if (strcmp(tag, "analog_rx_on") == 0 || strcmp(tag, "source_reentered") == 0 ||
        strcmp(tag, "switch_done") == 0)
        wake_source_ready = true;
}

static void wake_report_write(const char *path) {
    FILE *out = fopen(path, "a");
    if (!out)
        return;

    char wall[32] = "?";
    time_t now = time(NULL);
    strftime(wall, sizeof(wall), "%F %T", gmtime(&now));

    pthread_mutex_lock(&wake_mutex);
    double t0 = wake_mark_count ? wake_marks[0].t : 0;
    const char *what = switch_from >= 0 ? "switch" : "wake";
    if (switch_from >= 0) {
        fprintf(out, "\n===== SWITCH TIMING REPORT %s (RTC) =====\n", wall);
        fprintf(out, "platform: %s, source: %d -> %d\n", BOOT_TIMING_PLATFORM, switch_from, wake_source);
    } else {
        fprintf(out, "\n===== WAKE TIMING REPORT %s (RTC) =====\n", wall);
        fprintf(out, "platform: %s, source: %d, path: %s, slept: %.1f s\n", BOOT_TIMING_PLATFORM, wake_source,
                wake_fast ? "fast (straight back to the source)" : "menu, then source re-entry",
                wake_sleep_at > 0 ? t0 - wake_sleep_at : -1.0);
    }
    char opts_buf[256];
    boot_opt_summary(opts_buf, sizeof(opts_buf));
    fprintf(out, "boot options: %s\n", opts_buf[0] ? opts_buf : "(none queried)");
    fprintf(out, "-- marks: seconds since %s --\n", what);
    double signal_at = -1;
    for (int i = 0; i < wake_mark_count; i++) {
        fprintf(out, "%8.3f  %s\n", wake_marks[i].t - t0, wake_marks[i].tag);
        if (strcmp(wake_marks[i].tag, "first_valid_signal") == 0)
            signal_at = wake_marks[i].t - t0;
    }
    pthread_mutex_unlock(&wake_mutex);

    if (signal_at >= 0)
        fprintf(out, "%s -> first valid signal: %.3f s\n", what, signal_at);
    else
        fprintf(out, "%s -> first valid signal: none within %d s\n", what, WAKE_TIMEOUT_S);
    fprintf(out, "===== END =====\n");
    fflush(out);
    fsync(fileno(out));
    fclose(out);
}

static void *wake_report_thread(void *arg) {
    int generation = (int)(intptr_t)arg;
    double start = boot_now();

    while (!wake_signal_seen && boot_now() - start < WAKE_TIMEOUT_S) {
        if (generation != wake_generation)
            return NULL; // woken again meanwhile; that wake reports
        usleep(100000);
    }
    usleep(200000); // let the caller's last marks land

    if (generation == wake_generation) {
        char path[256];
        snprintf(path, sizeof(path), "%s/%s", path_extsd(), BOOT_TIMING_FILE);
        if (fs_file_exists(path))
            wake_report_write(path);
        wake_active = false;
    }
    return NULL;
}

static void transition_begin(int source, bool fast, int from) {
    pthread_mutex_lock(&wake_mutex);
    wake_generation++;
    wake_mark_count = 0;
    wake_source = source;
    wake_fast = fast;
    switch_from = from;
    int generation = wake_generation;
    pthread_mutex_unlock(&wake_mutex);

    wake_signal_seen = false;
    wake_source_ready = false;
    wake_active = true;
    wake_timing_mark(from >= 0 ? "switch_begin" : "wake_begin");

    pthread_t tid;
    if (pthread_create(&tid, NULL, wake_report_thread, (void *)(intptr_t)generation) == 0)
        pthread_detach(tid);
    else
        wake_active = false;
}

void wake_timing_wake(int source, bool fast) {
    transition_begin(source, fast, -1);
}

void switch_timing_begin(int from, int to) {
    if (!g_init_done)
        return; // the boot report covers the initial entry
    // A wake re-enters its source through the same switch functions: that
    // wake's report is already running until its source_reentered mark.
    if (wake_active && switch_from < 0 && !wake_source_ready)
        return;
    transition_begin(to, false, from < 0 ? 0 : from);
}

void boot_timing_signal(bool is_valid, int source) {
    if (is_valid && wake_active && wake_source_ready && !wake_signal_seen) {
        wake_timing_mark("first_valid_signal");
        wake_signal_seen = true;
    }

    if (!is_valid || signal_seen || marking_done)
        return;

    signal_time = boot_now();
    signal_source = source;
    signal_seen = true;
    boot_mark("first_valid_signal");
}

///////////////////////////////////////////////////////////////////////////////
// Report

typedef struct {
    double t;
    char src; // 'S' = rc.sh, 'A' = app
    char tag[BOOT_TIMING_TAG_LEN];
} timeline_entry_t;

static int timeline_compare(const void *a, const void *b) {
    const timeline_entry_t *ea = a, *eb = b;
    if (ea->t < eb->t)
        return -1;
    return ea->t > eb->t;
}

static void report_file(FILE *out, const char *path) {
    fprintf(out, "$ cat %s\n", path);
    FILE *in = fopen(path, "r");
    if (!in) {
        fprintf(out, "(not available)\n");
        return;
    }
    char line[512];
    while (fgets(line, sizeof(line), in))
        fputs(line, out);
    fclose(in);
}

static void report_cmd(FILE *out, const char *cmd) {
    fprintf(out, "$ %s\n", cmd);
    char full[256];
    snprintf(full, sizeof(full), "%s 2>&1", cmd);
    FILE *in = popen(full, "r");
    if (!in) {
        fprintf(out, "(popen failed)\n");
        return;
    }
    char line[512];
    while (fgets(line, sizeof(line), in))
        fputs(line, out);
    pclose(in);
}

static void report_timeline(FILE *out) {
    static timeline_entry_t entries[BOOT_TIMING_MAX_MARKS + 64];
    int n = 0;
    int max = sizeof(entries) / sizeof(entries[0]);

    // rc.sh marks: "<uptime> <tag>" per line
    FILE *in = fopen(BOOT_TIMING_SHELL_LOG, "r");
    bool have_shell_marks = in != NULL;
    if (in) {
        char line[128];
        while (n < max && fgets(line, sizeof(line), in)) {
            timeline_entry_t *e = &entries[n];
            if (sscanf(line, "%lf %47s", &e->t, e->tag) == 2) {
                e->src = 'S';
                n++;
            }
        }
        fclose(in);
    }

    pthread_mutex_lock(&mark_mutex);
    for (int i = 0; i < mark_count && n < max; i++) {
        entries[n].t = marks[i].t;
        entries[n].src = 'A';
        snprintf(entries[n].tag, sizeof(entries[n].tag), "%s", marks[i].tag);
        n++;
    }
    int dropped = marks_dropped;
    pthread_mutex_unlock(&mark_mutex);

    qsort(entries, n, sizeof(entries[0]), timeline_compare);

    fprintf(out, "-- timeline: seconds since kernel start, +delta to previous mark; S = rc.sh, A = app --\n");
    if (!have_shell_marks)
        fprintf(out, "(no rc.sh marks: %s missing -- firmware without the rc.sh marks, or app started by hand)\n",
                BOOT_TIMING_SHELL_LOG);
    for (int i = 0; i < n; i++) {
        double delta = i ? entries[i].t - entries[i - 1].t : 0;
        fprintf(out, "%9.3f  %+8.3f  %c %s\n", entries[i].t, delta, entries[i].src, entries[i].tag);
    }
    if (dropped)
        fprintf(out, "(%d app marks dropped: buffer full)\n", dropped);
}

static void report_write(const char *path) {
    if (fs_filesize(path) >= BOOT_TIMING_MAX_BYTES) {
        char prev[256];
        snprintf(prev, sizeof(prev), "%s.1", path);
        rename(path, prev);
    }

    FILE *out = fopen(path, "a");
    if (!out) {
        LOGE("boot timing: cannot open %s", path);
        return;
    }

    char wall[32] = "?";
    time_t now = time(NULL);
    strftime(wall, sizeof(wall), "%F %T", gmtime(&now));

    fprintf(out, "\n===== BOOT TIMING REPORT %s (RTC) =====\n", wall);
    fprintf(out, "platform: %s\n", BOOT_TIMING_PLATFORM);
    char version_path[256];
    snprintf(version_path, sizeof(version_path), "%s/version", path_app());
    report_file(out, version_path);
    report_file(out, "/proc/sys/kernel/random/boot_id");
    fprintf(out,
            "settings: autoscan.status=%d autoscan.source=%d autoscan.last_source=%d load_from_boot=%d "
            "hdzero_bw=%d hdzero_band=%d scan.channel=%d analog_module=%d elrs=%d ht=%d wifi=%d "
            "record.mode_manual=%d\n",
            g_setting.autoscan.status, g_setting.autoscan.source, g_setting.autoscan.last_source,
            g_setting.autoscan.load_from_boot, g_setting.source.hdzero_bw, g_setting.source.hdzero_band,
            g_setting.scan.channel, g_setting.source.analog_module, g_setting.elrs.enable, g_setting.ht.enable,
            g_setting.wifi.enable, g_setting.record.mode_manual);
    fprintf(out, "boot options (%s/%s/<name>):", path_extsd(), BOOT_OPT_DIR);
    pthread_mutex_lock(&opt_mutex);
    for (int i = 0; i < opt_count; i++)
        fprintf(out, " %s=%s", opts[i].name, opts[i].enabled ? "ON" : "off");
    pthread_mutex_unlock(&opt_mutex);
    fprintf(out, "%s\n", opt_count ? "" : " (none queried)");
    if (signal_seen)
        fprintf(out, "first valid signal: %.3f s since kernel start (source %d)\n", signal_time, signal_source);
    else
        fprintf(out, "first valid signal: none within %d s of app init\n", BOOT_TIMING_TIMEOUT_S);

    report_timeline(out);

    fprintf(out, "-- system --\n");
    report_file(out, "/proc/uptime");
    report_file(out, "/proc/loadavg");
    report_file(out, "/proc/cmdline");
    report_file(out, "/proc/mtd");
    report_file(out, "/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq");
    report_cmd(out, "fw_printenv bootdelay bootcmd");
    report_cmd(out, "mount");
    report_cmd(out, "zcat /proc/config.gz | grep -E 'JFFS2|SQUASHFS|KERNEL_(GZIP|LZO|LZ4|XZ)|PRINTK_TIME|PREEMPT'");
    report_cmd(out, "grep -c ^processor /proc/cpuinfo");
    report_cmd(out, "ps");
    report_cmd(out, "ls /mnt/extsd/log | wc -l");
    // What runs before rc.sh: init configuration and the startup scripts.
    report_cmd(out, "cat /proc/1/cmdline | tr '\\0' ' '; echo");
    report_cmd(out, "ls -la /pseudo_init /etc/init.d");
    report_cmd(out, "head -c 2 /pseudo_init | grep -q '#!' && head -n 120 /pseudo_init");
    report_file(out, "/etc/inittab");
    report_cmd(out, "for f in /etc/init.d/rcS /etc/init.d/S*; do [ -f \"$f\" ] && { echo \"### $f\"; head -n 150 \"$f\"; }; done");
    // Kernel log as of rc.sh start: the ring buffer is small and the display
    // driver's messages overwrite the early boot by the time of this report.
    report_file(out, "/tmp/boot_dmesg_rc_start.log");
    report_cmd(out, "dmesg");
    fprintf(out, "===== END =====\n");

    fflush(out);
    fsync(fileno(out));
    fclose(out);
    LOGI("boot timing: report appended to %s", path);
}

static void *boot_timing_thread(void *arg) {
    const char *path = arg;
    double start = boot_now();

    for (;;) {
        double now = boot_now();
        if (signal_seen && now - signal_time >= BOOT_TIMING_SETTLE_S)
            break;
        if (now - start >= BOOT_TIMING_TIMEOUT_S)
            break;
        usleep(250000);
    }

    marking_done = true;
    report_write(path);
    free(arg);
    return NULL;
}

void boot_timing_start(void) {
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", path_extsd(), BOOT_TIMING_FILE);

    if (!fs_file_exists(path)) {
        marking_done = true; // not enabled: stop collecting
        return;
    }

    char *arg = strdup(path);
    pthread_t tid;
    if (!arg || pthread_create(&tid, NULL, boot_timing_thread, arg) != 0) {
        free(arg);
        marking_done = true;
        return;
    }
    pthread_detach(tid);
}
