#include "core/perf_monitor.h"

#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/prctl.h>
#endif

#include <log/log.h>

#include "core/app_state.h"
#include "core/battery.h"
#include "core/boot_timing.h"
#include "core/common.hh"
#include "driver/nct75.h"
#include "platform/paths.h"
#include "ui/page_common.h"
#include "util/filesystem.h"

// Runtime CPU/power measurement.
//
// Every PERF_INTERVAL_S a background thread samples /proc and the counters
// the app feeds it, and appends one line to <SD>/PERF.log: CPU use of the
// whole system, of this app (and of its busiest threads) and of the record
// process, wakeups (context switches), the average CPU clock, LVGL rendering
// and framebuffer copy cost, time spent in shell commands, temperatures and
// the battery voltage. CPU figures are in % of one core (2 cores = 200 %).
// The governor scales the clock with the load, so the CPU use and the
// average MHz together are the power proxy.

#define PERF_FILE        "PERF.log"
#define PERF_INTERVAL_S  10
#define PERF_MAX_BYTES   (4 * 1024 * 1024)
#define PERF_MAX_TASKS   48
#define PERF_MAX_FREQS   32
#define PERF_TOP_THREADS 6

perf_opts_t perf_opt;

void perf_opts_init(void) {
    perf_opt.lazy_ui = !boot_opt("disable_lazy_ui");
    perf_opt.dirty_render = !boot_opt("disable_dirty_render");
    perf_opt.imu_lazy = !boot_opt("disable_imu_lazy");
    perf_opt.native_reg = !boot_opt("disable_native_reg");
    perf_opt.native_audio = !boot_opt("disable_native_audio");
    perf_opt.dvr_async = !boot_opt("disable_dvr_async");
    perf_opt.fast_switch = !boot_opt("disable_fast_switch");
}

void perf_thread_name(const char *name) {
#if defined(__linux__)
    prctl(PR_SET_NAME, (unsigned long)name, 0, 0, 0);
#else
    (void)name;
#endif
}

static uint32_t refr_count, refr_ms, refr_kpx;
static uint32_t flush_count, flush_kb, flush_us;
static uint32_t shell_count, shell_us;

static void counter_add(uint32_t *c, uint32_t v) {
    __atomic_fetch_add(c, v, __ATOMIC_RELAXED);
}

static uint32_t counter_take(uint32_t *c) {
    return __atomic_exchange_n(c, 0, __ATOMIC_RELAXED);
}

void perf_note_refresh(uint32_t ms, uint32_t px) {
    counter_add(&refr_count, 1);
    counter_add(&refr_ms, ms);
    counter_add(&refr_kpx, px >> 10);
}

void perf_note_flush(uint32_t bytes, uint32_t us) {
    counter_add(&flush_count, 1);
    counter_add(&flush_kb, bytes >> 10);
    counter_add(&flush_us, us);
}

void perf_note_shell(uint32_t us) {
    counter_add(&shell_count, 1);
    counter_add(&shell_us, us);
}

// Main-loop breakdown and peripheral-thread pace, counted only while the
// log runs (perf_running).
static bool perf_running = false;
static uint32_t loop_passes, periph_passes;
static uint32_t loop_us[PERF_LOOP_COUNT];

uint32_t perf_loop_now(void) {
    if (!perf_running)
        return 0;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000u + ts.tv_nsec / 1000;
}

uint32_t perf_loop_section(perf_loop_section_t section, uint32_t since) {
    if (!perf_running)
        return 0;
    uint32_t now = perf_loop_now();
    counter_add(&loop_us[section], now - since);
    return now;
}

void perf_note_loop_pass(void) {
    if (perf_running)
        counter_add(&loop_passes, 1);
}

void perf_note_periph_pass(void) {
    if (perf_running)
        counter_add(&periph_passes, 1);
}

#if defined(__linux__)

typedef struct {
    int tid;
    char comm[16];
    unsigned long ticks; // utime + stime
    unsigned long ctx;   // voluntary + nonvoluntary context switches
    bool seen;
} task_stat_t;

typedef struct {
    char comm[16];
    unsigned long ticks;
} comm_ticks_t;

static task_stat_t tasks[PERF_MAX_TASKS];
static int task_count = 0;

static unsigned long freq_khz[PERF_MAX_FREQS];
static unsigned long freq_time[PERF_MAX_FREQS];
static int freq_count = 0;

static int record_pid = -1;

static double mono_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

// utime + stime from a /proc/.../stat line; the comm field may contain
// spaces, so parse after its closing parenthesis.
static bool read_stat_ticks(const char *path, char *comm, size_t comm_size, unsigned long *ticks) {
    char line[512];
    FILE *fp = fopen(path, "r");
    if (!fp)
        return false;
    bool ok = fgets(line, sizeof(line), fp) != NULL;
    fclose(fp);
    if (!ok)
        return false;

    char *open = strchr(line, '(');
    char *close = strrchr(line, ')');
    if (!open || !close || close < open)
        return false;
    if (comm) {
        size_t len = close - open - 1;
        if (len >= comm_size)
            len = comm_size - 1;
        memcpy(comm, open + 1, len);
        comm[len] = 0;
    }

    unsigned long utime, stime;
    if (sscanf(close + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &utime, &stime) != 2)
        return false;
    *ticks = utime + stime;
    return true;
}

static unsigned long read_ctx_switches(const char *path) {
    char line[128];
    unsigned long total = 0, v;
    FILE *fp = fopen(path, "r");
    if (!fp)
        return 0;
    while (fgets(line, sizeof(line), fp)) {
        if (sscanf(line, "voluntary_ctxt_switches: %lu", &v) == 1 ||
            sscanf(line, "nonvoluntary_ctxt_switches: %lu", &v) == 1)
            total += v;
    }
    fclose(fp);
    return total;
}

// Busy and total jiffies of all CPUs from /proc/stat.
static bool read_cpu_jiffies(unsigned long long *busy, unsigned long long *total) {
    unsigned long long v[8] = {0};
    FILE *fp = fopen("/proc/stat", "r");
    if (!fp)
        return false;
    int n = fscanf(fp, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5],
                   &v[6], &v[7]);
    fclose(fp);
    if (n < 4)
        return false;
    *total = 0;
    for (int i = 0; i < 8; i++)
        *total += v[i];
    *busy = *total - v[3] - v[4]; // minus idle and iowait
    return true;
}

static int find_record_pid(void) {
    DIR *dir = opendir("/proc");
    if (!dir)
        return -1;
    struct dirent *de;
    int pid = -1;
    while ((de = readdir(dir)) != NULL) {
        if (de->d_name[0] < '1' || de->d_name[0] > '9')
            continue;
        char path[64], comm[32] = "";
        snprintf(path, sizeof(path), "/proc/%s/comm", de->d_name);
        FILE *fp = fopen(path, "r");
        if (!fp)
            continue;
        if (fgets(comm, sizeof(comm), fp) && strcmp(comm, "record\n") == 0)
            pid = atoi(de->d_name);
        fclose(fp);
        if (pid > 0)
            break;
    }
    closedir(dir);
    return pid;
}

static unsigned long record_ticks(void) {
    char path[64];
    unsigned long ticks = 0;
    char comm[16];
    for (int attempt = 0; attempt < 2; attempt++) {
        if (record_pid > 0) {
            snprintf(path, sizeof(path), "/proc/%d/stat", record_pid);
            if (read_stat_ticks(path, comm, sizeof(comm), &ticks) && strcmp(comm, "record") == 0)
                return ticks;
        }
        record_pid = find_record_pid();
    }
    return 0;
}

// Average CPU clock (MHz) since the previous call, from the cpufreq stats;
// the current clock when the kernel has no stats.
static int avg_cpu_mhz(void) {
    FILE *fp = fopen("/sys/devices/system/cpu/cpu0/cpufreq/stats/time_in_state", "r");
    if (fp) {
        unsigned long f, t;
        unsigned long long weighted = 0, total = 0;
        int i = 0;
        while (i < PERF_MAX_FREQS && fscanf(fp, "%lu %lu", &f, &t) == 2) {
            if (i < freq_count && freq_khz[i] == f) {
                weighted += (unsigned long long)f * (t - freq_time[i]);
                total += t - freq_time[i];
            }
            freq_khz[i] = f;
            freq_time[i] = t;
            i++;
        }
        fclose(fp);
        bool first = freq_count == 0;
        freq_count = i;
        if (!first && total)
            return (int)(weighted / total / 1000);
        if (!first)
            return 0;
    }

    unsigned long khz = 0;
    fp = fopen("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", "r");
    if (fp) {
        if (fscanf(fp, "%lu", &khz) != 1)
            khz = 0;
        fclose(fp);
    }
    return (int)(khz / 1000);
}

// Walk this process's threads: per-thread CPU ticks (summed by thread name
// into `out`, which gets the deltas) and the total context switches.
static int sample_tasks(comm_ticks_t *out, int out_max, unsigned long *ctx_delta) {
    for (int i = 0; i < task_count; i++)
        tasks[i].seen = false;

    int out_count = 0;
    *ctx_delta = 0;

    DIR *dir = opendir("/proc/self/task");
    if (!dir)
        return 0;
    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        int tid = atoi(de->d_name);
        if (tid <= 0)
            continue;

        char path[64], comm[16];
        unsigned long ticks;
        snprintf(path, sizeof(path), "/proc/self/task/%d/stat", tid);
        if (!read_stat_ticks(path, comm, sizeof(comm), &ticks))
            continue;
        snprintf(path, sizeof(path), "/proc/self/task/%d/status", tid);
        unsigned long ctx = read_ctx_switches(path);

        task_stat_t *t = NULL;
        for (int i = 0; i < task_count; i++) {
            if (tasks[i].tid == tid) {
                t = &tasks[i];
                break;
            }
        }
        unsigned long prev_ticks = 0, prev_ctx = 0;
        if (t) {
            prev_ticks = t->ticks;
            prev_ctx = t->ctx;
        } else if (task_count < PERF_MAX_TASKS) {
            t = &tasks[task_count++];
            t->tid = tid;
        }
        if (t) {
            snprintf(t->comm, sizeof(t->comm), "%s", comm);
            t->ticks = ticks;
            t->ctx = ctx;
            t->seen = true;
        }

        *ctx_delta += ctx - prev_ctx;
        unsigned long dticks = ticks - prev_ticks;
        int j;
        for (j = 0; j < out_count; j++) {
            if (strcmp(out[j].comm, comm) == 0)
                break;
        }
        if (j == out_count) {
            if (out_count == out_max)
                continue;
            snprintf(out[out_count].comm, sizeof(out[out_count].comm), "%s", comm);
            out[out_count].ticks = 0;
            out_count++;
        }
        out[j].ticks += dticks;
    }
    closedir(dir);

    // Forget threads that exited.
    int n = 0;
    for (int i = 0; i < task_count; i++) {
        if (tasks[i].seen)
            tasks[n++] = tasks[i];
    }
    task_count = n;
    return out_count;
}

static int comm_ticks_desc(const void *a, const void *b) {
    const comm_ticks_t *ca = a, *cb = b;
    return (cb->ticks > ca->ticks) - (cb->ticks < ca->ticks);
}

static const char *state_name(app_state_t s) {
    switch (s) {
    case APP_STATE_MAINMENU:
        return "MENU";
    case APP_STATE_SUBMENU:
    case APP_STATE_SUBMENU_ITEM_FOCUSED:
        return "SUBM";
    case APP_STATE_PLAYBACK:
        return "PLAY";
    case APP_STATE_VIDEO:
        return "VIDEO";
    case APP_STATE_IMS:
        return "IMS";
    case APP_STATE_OSD_ELEMENT_PREV:
        return "OSDEL";
    case APP_STATE_WIFI:
        return "WIFI";
    case APP_STATE_USER_INPUT_DISABLED:
        return "BUSY";
    case APP_STATE_SLEEP:
        return "SLEEP";
    default:
        return "?";
    }
}

static const char *source_name(int source) {
    switch (source) {
    case SOURCE_HDZERO:
        return "HDZ";
    case SOURCE_HDMI_IN:
        return "HDMI";
    case SOURCE_AV_IN:
        return "AVIN";
    case SOURCE_AV_MODULE:
        return "AV";
    default:
        return "?";
    }
}

static void perf_append(const char *path, const char *text) {
    int fd = open(path, O_WRONLY | O_APPEND);
    if (fd < 0)
        return;
    size_t len = strlen(text);
    if (write(fd, text, len) != (ssize_t)len)
        LOGE("perf: short write to %s", path);
    fsync(fd);
    close(fd);
}

static void perf_write_header(const char *path) {
    char buf[2048];
    int len = 0;

    char wall[32] = "?";
    time_t now = time(NULL);
    strftime(wall, sizeof(wall), "%F %T", gmtime(&now));

    char version[64] = "?";
    char version_path[256];
    snprintf(version_path, sizeof(version_path), "%s/version", path_app());
    FILE *fp = fopen(version_path, "r");
    if (fp) {
        if (fgets(version, sizeof(version), fp))
            version[strcspn(version, "\r\n")] = 0;
        fclose(fp);
    }

    char governor[32] = "?";
    fp = fopen("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor", "r");
    if (fp) {
        if (fgets(governor, sizeof(governor), fp))
            governor[strcspn(governor, "\r\n")] = 0;
        fclose(fp);
    }

    char opts[256];
    boot_opt_summary(opts, sizeof(opts));

    len += snprintf(buf + len, sizeof(buf) - len, "\n===== PERF LOG %s (RTC) =====\n", wall);
    len += snprintf(buf + len, sizeof(buf) - len, "version: %s, cpus: %ld, governor: %s, interval: %d s\n", version,
                    sysconf(_SC_NPROCESSORS_ONLN), governor, PERF_INTERVAL_S);
    len += snprintf(buf + len, sizeof(buf) - len, "boot options: %s\n", opts[0] ? opts : "(none queried)");
    len += snprintf(buf + len, sizeof(buf) - len,
                    "columns: uptime s | app state | source | cpu = whole system, app = this app, rec = record "
                    "process (all in %% of one core) | MHz = average CPU clock | wk = app context switches/s | "
                    "refr = LVGL refreshes/s, rend = ms/s spent in them, Mpx = rendered Mpixel/s | fb = KB/s "
                    "copied to the framebuffer, ms/s spent copying | sh = shell commands, ms | T = temperature "
                    "top/left/right C | bat = mV | loop = main-loop passes/s, then ms/s in main_menu_update, "
                    "statubar_update, osd_hdzero_update, lv_timer_handler and the rest | per = peripheral "
                    "thread work passes/s | busiest app threads (%% of one core)\n");
    perf_append(path, buf);
}

static void *perf_thread(void *arg) {
    char *path = arg;
    perf_thread_name("perfmon");

    long hz = sysconf(_SC_CLK_TCK);
    if (hz <= 0)
        hz = 100;

    comm_ticks_t threads[PERF_MAX_TASKS];
    unsigned long ctx_delta;
    unsigned long long cpu_busy0 = 0, cpu_busy, cpu_total;
    unsigned long app0 = 0, app, rec0, rec;

    // Baselines; the first line covers the first full interval.
    read_cpu_jiffies(&cpu_busy0, &cpu_total);
    read_stat_ticks("/proc/self/stat", NULL, 0, &app0);
    rec0 = record_ticks();
    sample_tasks(threads, PERF_MAX_TASKS, &ctx_delta);
    avg_cpu_mhz();
    uint32_t *counters[] = {&refr_count, &refr_ms, &refr_kpx, &flush_count, &flush_kb, &flush_us,
                            &shell_count, &shell_us, &loop_passes, &periph_passes};
    for (unsigned i = 0; i < sizeof(counters) / sizeof(counters[0]); i++)
        counter_take(counters[i]);
    double t0 = mono_now();

    perf_write_header(path);

    for (;;) {
        sleep(PERF_INTERVAL_S);

        double t1 = mono_now();
        double dt = t1 - t0;
        t0 = t1;
        if (dt <= 0)
            continue;
        double wall_ticks = dt * hz;

        if (!read_cpu_jiffies(&cpu_busy, &cpu_total))
            cpu_busy = cpu_busy0;
        if (!read_stat_ticks("/proc/self/stat", NULL, 0, &app))
            app = app0;
        rec = record_ticks();
        int n = sample_tasks(threads, PERF_MAX_TASKS, &ctx_delta);
        int mhz = avg_cpu_mhz();

        double cpu_pct = 100.0 * (cpu_busy - cpu_busy0) / wall_ticks;
        double app_pct = 100.0 * (app - app0) / wall_ticks;
        double rec_pct = rec >= rec0 ? 100.0 * (rec - rec0) / wall_ticks : 0;
        cpu_busy0 = cpu_busy;
        app0 = app;
        rec0 = rec;

        uint32_t r_count = counter_take(&refr_count), r_ms = counter_take(&refr_ms), r_kpx = counter_take(&refr_kpx);
        counter_take(&flush_count);
        uint32_t f_kb = counter_take(&flush_kb), f_us = counter_take(&flush_us);
        uint32_t s_count = counter_take(&shell_count), s_us = counter_take(&shell_us);
        uint32_t l_passes = counter_take(&loop_passes), p_passes = counter_take(&periph_passes);
        double l_ms[PERF_LOOP_COUNT];
        for (int i = 0; i < PERF_LOOP_COUNT; i++)
            l_ms[i] = counter_take(&loop_us[i]) / 1000.0 / dt;

        struct timespec up;
        clock_gettime(CLOCK_MONOTONIC, &up);

        char line[1024];
        int len = snprintf(line, sizeof(line),
                           "%7.0f %-5s %-4s cpu=%5.1f app=%5.1f rec=%4.1f MHz=%4d wk=%5.0f refr=%5.1f rend=%4.0f "
                           "Mpx=%5.2f fb=%6.0f/%3.0f sh=%u/%u T=%.1f/%.1f/%.1f bat=%d "
                           "loop=%3.0f/%.0f/%.0f/%.0f/%.0f/%.0f per=%.1f |",
                           (double)up.tv_sec, state_name(g_app_state), source_name(g_source_info.source), cpu_pct,
                           app_pct, rec_pct, mhz, ctx_delta / dt, r_count / dt, r_ms / dt,
                           r_kpx * 1024.0 / 1e6 / dt, f_kb / dt, f_us / 1000.0 / dt, s_count, s_us / 1000,
                           g_temperature.top / 10.0, g_temperature.left / 10.0, g_temperature.right / 10.0,
                           battery_get_millivolts(false), l_passes / dt, l_ms[PERF_LOOP_MENU],
                           l_ms[PERF_LOOP_STATUSBAR], l_ms[PERF_LOOP_OSD], l_ms[PERF_LOOP_LVGL],
                           l_ms[PERF_LOOP_OTHER], p_passes / dt);

        qsort(threads, n, sizeof(threads[0]), comm_ticks_desc);
        for (int i = 0; i < n && i < PERF_TOP_THREADS && len < (int)sizeof(line) - 32; i++) {
            if (threads[i].ticks == 0)
                break;
            len += snprintf(line + len, sizeof(line) - len, " %s=%.1f", threads[i].comm,
                            100.0 * threads[i].ticks / wall_ticks);
        }
        snprintf(line + len, sizeof(line) - len, "\n");

        if (fs_filesize(path) >= PERF_MAX_BYTES) {
            char prev[256];
            snprintf(prev, sizeof(prev), "%s.1", path);
            rename(path, prev);
            close(open(path, O_WRONLY | O_CREAT, 0644));
            perf_write_header(path);
        }
        perf_append(path, line);
    }
    return NULL;
}

void perf_monitor_start(void) {
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", path_extsd(), PERF_FILE);
    if (!fs_file_exists(path))
        return;

    char *arg = strdup(path);
    pthread_t tid;
    if (!arg || pthread_create(&tid, NULL, perf_thread, arg) != 0) {
        free(arg);
        return;
    }
    pthread_detach(tid);
    perf_running = true;
    LOGI("perf: logging to %s every %d s", path, PERF_INTERVAL_S);
}

#else // !__linux__: no /proc to sample

void perf_monitor_start(void) {
}

#endif
