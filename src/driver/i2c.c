// Goggle-only: the Linux I2C bus driver. The emulator has no bus and links
// src/emulator/i2c_emu.c instead (CMake drops this file for EMULATOR_BUILD).
#include "i2c.h"

#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h> // for close

#include <log/log.h>

#include "../core/common.hh"

pthread_mutex_t i2c_mutex;

// Background I2C users (i2c_set_background) step aside while another thread
// waits for the bus. i2c_mutex is not fair: a thread issuing transfers back
// to back re-takes it before a woken waiter gets to run. The boot-time
// HDZero receiver bring-up on a worker (HDZero_open_early) is thousands of
// transfers and starved the main thread's own I2C work at boot
// (IT66021_init 0.2 s -> 1.4 s).
static atomic_int i2c_foreground_waiting = 0;
static __thread bool i2c_background = false;

void i2c_set_background(bool on) {
    i2c_background = on;
}

static void i2c_lock(void) {
    if (i2c_background) {
        while (atomic_load(&i2c_foreground_waiting) > 0)
            usleep(100);
        pthread_mutex_lock(&i2c_mutex);
    } else {
        atomic_fetch_add(&i2c_foreground_waiting, 1);
        pthread_mutex_lock(&i2c_mutex);
        atomic_fetch_sub(&i2c_foreground_waiting, 1);
    }
}

#define IIC_PORTS 4

static char *IIC_DEVS[IIC_PORTS] = {
    "/dev/i2c-0",
    "/dev/i2c-1",
    "/dev/i2c-2",
    "/dev/i2c-3",
};

static int g_iic_fds[IIC_PORTS];
static bool g_iic_report_once[IIC_PORTS] = {false};

bool iic_is_port_ready(int port) {
    if (port < 0 || port >= IIC_PORTS) {
        LOGE("Port %d contains an invalid range [0=N/A,1=Right,2=Main,3=Left]", port);
        return false;
    } else if (g_iic_fds[port] < 0) {
        if (!g_iic_report_once[port]) {
            g_iic_report_once[port] = true;
            LOGE("Device %d:%s is not available [0=N/A,1=Right,2=Main,3=Left]", port, IIC_DEVS[port]);
        }
        return false;
    }
    return true;
}

void iic_init() {
    pthread_mutex_init(&i2c_mutex, NULL);

    // Offset starts with 1 as it is not referenced thus far.
    for (int i = 1; i < IIC_PORTS; ++i) {
        g_iic_fds[i] = open(IIC_DEVS[i], O_RDONLY);
        iic_is_port_ready(i);
    }
}

static uint8_t iic_read(int i2c_fd, uint8_t slave_address, uint16_t reg_address) {
    struct i2c_rdwr_ioctl_data work_queue;
    struct i2c_msg msgs[2];

    uint8_t val = 0;
    int ret;

    work_queue.nmsgs = 2;
    work_queue.msgs = msgs;

    val = (unsigned char)reg_address;
    (work_queue.msgs[0]).len = 1;
    (work_queue.msgs[0]).flags = 0;
    (work_queue.msgs[0]).addr = slave_address;
    (work_queue.msgs[0]).buf = &val;

    (work_queue.msgs[1]).len = 1;
    (work_queue.msgs[1]).flags = 1;
    (work_queue.msgs[1]).addr = slave_address;
    (work_queue.msgs[1]).buf = &val;

    ret = ioctl(i2c_fd, I2C_RDWR, (unsigned long)&work_queue);
    if (ret < 0) {
        // LOGI("iic_read[%x.%x] failed.",slave_address, reg_address);
        val = 0;
    }
    return val;
}

static void iic_read_n(int i2c_fd, uint8_t slave_address, uint16_t reg_address, uint8_t *reg_data, uint16_t len) {
    struct i2c_rdwr_ioctl_data work_queue;
    struct i2c_msg msgs[2];
    uint8_t val = 0;

    work_queue.nmsgs = 2;
    work_queue.msgs = msgs;

    val = (unsigned char)reg_address;
    (work_queue.msgs[0]).len = 1;
    (work_queue.msgs[0]).flags = 0;
    (work_queue.msgs[0]).addr = slave_address;
    (work_queue.msgs[0]).buf = &val;

    (work_queue.msgs[1]).len = len;
    (work_queue.msgs[1]).flags = 1;
    (work_queue.msgs[1]).addr = slave_address;
    (work_queue.msgs[1]).buf = reg_data;

    ioctl(i2c_fd, I2C_RDWR, (unsigned long)&work_queue);
    // if(ret < 0)
    //     LOGI("iic_read_n[%x.%x] failed.",slave_address, reg_address);
}

static int iic_write(int i2c_fd, uint8_t slave_address, uint16_t reg_address, uint16_t reg_val) {
    struct i2c_rdwr_ioctl_data work_queue;
    struct i2c_msg msgs;
    uint8_t obuf[2];
    int ret;

    work_queue.nmsgs = 1;
    work_queue.msgs = &msgs;
    msgs.buf = obuf;

    (work_queue.msgs[0]).len = 2;
    (work_queue.msgs[0]).flags = 0;
    (work_queue.msgs[0]).addr = slave_address;
    (work_queue.msgs[0]).buf[0] = reg_address;
    (work_queue.msgs[0]).buf[1] = reg_val;

    ret = ioctl(i2c_fd, I2C_RDWR, (unsigned long)&work_queue);
    if (ret < 0) {
        // LOGI("iic_write[%x.%x]<-%x failed.",slave_address, reg_address, reg_val);
        ret = 0;
    }
    return ret;
}

static int iic_write_n(int i2c_fd, uint8_t slave_address, uint8_t reg_address, uint8_t *reg_val, uint16_t len) {
    struct i2c_rdwr_ioctl_data work_queue;
    struct i2c_msg msgs;
    int ret;

    work_queue.nmsgs = 1;
    work_queue.msgs = &msgs;

    if ((work_queue.msgs[0].buf = (unsigned char *)malloc((len + 1) * sizeof(unsigned char))) == NULL) {
        // LOGI("buf memery alloc error...");
        return -1;
    }

    (work_queue.msgs[0]).len = len + 1;
    (work_queue.msgs[0]).flags = 0;
    (work_queue.msgs[0]).addr = slave_address;
    (work_queue.msgs[0]).buf[0] = reg_address;

    for (uint16_t i = 1; i <= len; i++)
        (work_queue.msgs[0]).buf[i] = reg_val[i - 1];

    ret = ioctl(i2c_fd, I2C_RDWR, (unsigned long)&work_queue);
    if (ret < 0) {
        // LOGI("iic_write_n[%x.%x] failed.",slave_address, reg_address);
        ret = 0;
    }
    free(work_queue.msgs[0].buf);
    return ret;
}

uint8_t i2c_read(int port, uint8_t slave_address, uint8_t addr) {
    uint8_t val = 0;

    if (!iic_is_port_ready(port)) {
        return 0;
    }

    i2c_lock();
    val = iic_read(g_iic_fds[port], slave_address, addr);
    pthread_mutex_unlock(&i2c_mutex);

    return val;
}

int8_t i2c_read_n(int port, uint8_t slave_address, uint8_t addr, uint8_t *data, uint16_t len) {
    if (!iic_is_port_ready(port)) {
        return -1;
    }

    i2c_lock();
    iic_read_n(g_iic_fds[port], slave_address, addr, data, len);
    pthread_mutex_unlock(&i2c_mutex);

    return 0;
}

int i2c_write(int port, uint8_t slave_address, uint8_t addr, uint8_t val) {
    int ret = -1;

    if (!iic_is_port_ready(port)) {
        return ret;
    }

    i2c_lock();
    ret = iic_write(g_iic_fds[port], slave_address, addr, val);
    pthread_mutex_unlock(&i2c_mutex);

    return ret;
}

int8_t i2c_write_n(int port, uint8_t slave_address, uint8_t addr, uint8_t *val, uint16_t len) {
    int ret = -1;

    if (!iic_is_port_ready(port)) {
        return ret;
    }

    i2c_lock();
    iic_write_n(g_iic_fds[port], slave_address, addr, val, len);
    pthread_mutex_unlock(&i2c_mutex);

    return 0;
}
