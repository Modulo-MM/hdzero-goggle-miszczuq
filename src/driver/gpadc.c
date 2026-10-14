#include "gpadc.h"

#if defined(HDZBOXPRO) || defined(HDZGOGGLE2)

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include <log/log.h>

#include "../core/common.hh"
#include "core/perf_monitor.h"
#include "driver/devmem.h"
#include "util/system.h"

#define GPADC_DATA0 0x05070080

void gpadc_init() {
    // open gpadc clock
    reg_write32(0x030019ec, 0x00010001);

    // open adc channel 0
    reg_write32(0x05070008, 0x00000001);
}

void gpadc_on(uint8_t is_on) {
    if (is_on) {
        reg_write32(0x05070004, 0xffbd0000);
    } else {
        reg_write32(0x05070004, 0xffbc0000);
    }
}

int gpdac0_get() {
    char buf[128];

#ifdef EMULATOR_BUILD
    return -1;
#endif

    // native_reg: read the data register in-process. The analog RSSI is
    // polled every 100 ms in analog video and once per channel in a scan;
    // the `awr` path below is a shell, a process, a file and a 10 ms sleep.
    uint32_t data;
    if (devmem_read32(GPADC_DATA0, &data) == 0)
        return (int)data;

    sprintf(buf, "awr 0x%08x > %s", GPADC_DATA0, ADC0_FILE);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    system(buf); // not system_exec(): 10 log lines a second
    clock_gettime(CLOCK_MONOTONIC, &t1);
    perf_note_shell((t1.tv_sec - t0.tv_sec) * 1000000 + (t1.tv_nsec - t0.tv_nsec) / 1000);
    usleep(10 * 1000); // 10ms

    FILE *file;
    file = fopen(ADC0_FILE, "r");
    if (file == NULL) {
        LOGI("%s open failed", ADC0_FILE);
        return 1;
    }

    uint32_t addr, reg;

    if (fscanf(file, "read 0x%x:0x%x", &addr, &reg) == 2) {
        fclose(file);
        if (addr == GPADC_DATA0)
            return reg;
        else
            return -1;
    }

    fclose(file);
    return -1;
}
#else
void gpadc_init() {}
void gpadc_on(uint8_t is_on) {}
int gpdac0_get() {}
#endif
