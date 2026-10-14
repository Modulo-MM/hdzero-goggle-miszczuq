#include "system.h"

#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "core/perf_monitor.h"
#include "log/log.h"
#include "util/filesystem.h"

#ifndef EMULATOR_BUILD
// system() plus its duration, for PERF.log and the app log: every call is a
// shell (and usually another process) started from scratch.
static int timed_system(const char *command) {
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int ret = system(command);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    uint32_t us = (t1.tv_sec - t0.tv_sec) * 1000000 + (t1.tv_nsec - t0.tv_nsec) / 1000;
    perf_note_shell(us);
    LOGI("  took %u ms", us / 1000);
    return ret;
}
#endif

int system_exec(const char *command) {
    LOGI("System Execute: %s", command);
#ifdef EMULATOR_BUILD
    // The emulator stands in for the goggle hardware, not the host OS. These
    // commands are device operations (display driver `dispw`, register pokes
    // `aww`, wifi bring-up + root-password scripts, writes under /etc). Running
    // them on a dev machine ranges from useless to destructive (the passwd
    // script tried to change the host user's password), so we log and report
    // success without touching the host. Real file I/O goes through fopen/minIni,
    // not this path, so nothing the emulator legitimately needs is lost.
    LOGI("  [emulator] not executed (host-safe no-op)");
    return 0;
#else
    return timed_system(command);
#endif
}

int system_script(const char *command) {
    LOGI("System Script: %s", command);
#ifdef EMULATOR_BUILD
    LOGI("  [emulator] not executed (host-safe no-op)");
    return 0;
#else

    // basename may edit argument
    const char *script = fs_basename(command);

    // Modify command to log std out and error to a temporary file
    char buffer[256];
    snprintf(buffer,
             sizeof(buffer), "%s > /tmp/%s.log 2>&1",
             command,
             script);
    int retval = timed_system(buffer);

    // Read contents of temporary file and use logging framework
    snprintf(buffer, sizeof(buffer), "/tmp/%s.log", script);
    FILE *fp = fopen(buffer, "r");
    if (fp) {
        char *line = NULL;
        size_t len = 0;
        ssize_t read = 0;
        while ((read = getline(&line, &len, fp)) != -1) {
            line[strcspn(line, "\r\n")] = 0;
            LOGD("%s", line);
        }
        if (line) {
            free(line);
        }
        fclose(fp);
    }

    return retval;
#endif
}
