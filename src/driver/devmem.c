#include "driver/devmem.h"

#include <stdbool.h>
#include <stdio.h>

#include <log/log.h>

#include "core/perf_monitor.h"
#include "util/system.h"

#ifndef EMULATOR_BUILD
#include <fcntl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>

#define DEVMEM_MAX_PAGES 8

static pthread_mutex_t devmem_mutex = PTHREAD_MUTEX_INITIALIZER;
static int devmem_fd = -1;
static bool devmem_broken = false; // stop trying after the first failure
static struct {
    uint32_t base;
    volatile uint8_t *map;
} pages[DEVMEM_MAX_PAGES];
static int page_count = 0;

// The register's address in a (cached) mapping of its page; NULL if native
// access is off or unavailable. Caller holds devmem_mutex.
static volatile uint32_t *devmem_reg(uint32_t addr) {
    if (!perf_opt.native_reg || devmem_broken)
        return NULL;

    uint32_t page_size = (uint32_t)sysconf(_SC_PAGESIZE);
    uint32_t base = addr & ~(page_size - 1);
    for (int i = 0; i < page_count; i++) {
        if (pages[i].base == base)
            return (volatile uint32_t *)(pages[i].map + (addr - base));
    }
    if (page_count == DEVMEM_MAX_PAGES)
        return NULL;

    if (devmem_fd < 0) {
        devmem_fd = open("/dev/mem", O_RDWR | O_SYNC);
        if (devmem_fd < 0) {
            LOGE("devmem: open /dev/mem failed (%m), using aww/awr");
            devmem_broken = true;
            return NULL;
        }
    }
    void *map = mmap(NULL, page_size, PROT_READ | PROT_WRITE, MAP_SHARED, devmem_fd, base);
    if (map == MAP_FAILED) {
        LOGE("devmem: mmap 0x%08x failed (%m), using aww/awr", base);
        devmem_broken = true;
        return NULL;
    }
    LOGI("devmem: mapped 0x%08x", base);
    pages[page_count].base = base;
    pages[page_count].map = map;
    page_count++;
    return (volatile uint32_t *)((volatile uint8_t *)map + (addr - base));
}

void reg_write32(uint32_t addr, uint32_t value) {
    pthread_mutex_lock(&devmem_mutex);
    volatile uint32_t *reg = devmem_reg(addr);
    if (reg)
        *reg = value;
    pthread_mutex_unlock(&devmem_mutex);
    if (reg)
        return;

    char cmd[48];
    snprintf(cmd, sizeof(cmd), "aww 0x%08x 0x%08x", addr, value);
    system_exec(cmd);
}

int devmem_read32(uint32_t addr, uint32_t *value) {
    pthread_mutex_lock(&devmem_mutex);
    volatile uint32_t *reg = devmem_reg(addr);
    if (reg)
        *value = *reg;
    pthread_mutex_unlock(&devmem_mutex);
    return reg ? 0 : -1;
}

#else // EMULATOR_BUILD: no SoC; system_exec() logs and skips the command

void reg_write32(uint32_t addr, uint32_t value) {
    char cmd[48];
    snprintf(cmd, sizeof(cmd), "aww 0x%08x 0x%08x", addr, value);
    system_exec(cmd);
}

int devmem_read32(uint32_t addr, uint32_t *value) {
    return -1;
}

#endif
