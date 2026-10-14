#include "driver/mixer.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <log/log.h>

#include "core/perf_monitor.h"
#include "ui/page_common.h"
#include "util/system.h"

#ifndef EMULATOR_BUILD
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <sound/asound.h>

#define MIXER_CTL_DEV "/dev/snd/controlC0"

static pthread_mutex_t mixer_mutex = PTHREAD_MUTEX_INITIALIZER;
static int ctl_fd = -1;
static bool ctl_broken = false;

// The index of the enumerated item named at *pp (as alsa-lib matches it),
// advancing *pp past the name; -1 if no item has that name.
static int enum_item_by_name(struct snd_ctl_elem_info *info, const char **pp) {
    const char *p = *pp;
    for (unsigned i = 0; i < info->value.enumerated.items; i++) {
        struct snd_ctl_elem_info item = *info;
        item.value.enumerated.item = i;
        if (ioctl(ctl_fd, SNDRV_CTL_IOCTL_ELEM_INFO, &item) < 0)
            return -1;
        size_t len = strlen(item.value.enumerated.name);
        if (strncmp(item.value.enumerated.name, p, len) == 0 && (!p[len] || p[len] == ',')) {
            *pp = p + len;
            return i;
        }
    }
    return -1;
}

// Caller holds mixer_mutex. 0 on success.
static int native_cset(const char *name, const char *values) {
    if (ctl_fd < 0) {
        ctl_fd = open(MIXER_CTL_DEV, O_RDWR | O_CLOEXEC);
        if (ctl_fd < 0) {
            LOGE("mixer: open %s failed (%m), using amixer", MIXER_CTL_DEV);
            ctl_broken = true;
            return -1;
        }
    }

    struct snd_ctl_elem_info info;
    memset(&info, 0, sizeof(info));
    info.id.iface = SNDRV_CTL_ELEM_IFACE_MIXER;
    snprintf((char *)info.id.name, sizeof(info.id.name), "%s", name);
    if (ioctl(ctl_fd, SNDRV_CTL_IOCTL_ELEM_INFO, &info) < 0) {
        LOGE("mixer: no control '%s' (%m)", name);
        return -1;
    }

    struct snd_ctl_elem_value val;
    memset(&val, 0, sizeof(val));
    val.id = info.id;
    if (ioctl(ctl_fd, SNDRV_CTL_IOCTL_ELEM_READ, &val) < 0) {
        LOGE("mixer: read '%s' failed (%m)", name);
        return -1;
    }

    // alsa-lib's snd_ctl_ascii_value_parse(): without a comma the one value
    // goes to every channel; an empty field leaves its channel unchanged.
    bool per_channel = strchr(values, ',') != NULL;
    const char *p = values;
    for (unsigned i = 0; i < info.count && i < 128 && *p; i++) {
        if (*p != ',') {
            char *end;
            long v;
            switch (info.type) {
            case SNDRV_CTL_ELEM_TYPE_BOOLEAN:
                v = strtol(p, &end, 0);
                if (end == p)
                    return -1;
                val.value.integer.value[i] = v ? 1 : 0;
                p = end;
                break;
            case SNDRV_CTL_ELEM_TYPE_INTEGER:
                v = strtol(p, &end, 0);
                if (end == p)
                    return -1;
                if (v < info.value.integer.min)
                    v = info.value.integer.min;
                if (v > info.value.integer.max)
                    v = info.value.integer.max;
                val.value.integer.value[i] = v;
                p = end;
                break;
            case SNDRV_CTL_ELEM_TYPE_ENUMERATED:
                v = enum_item_by_name(&info, &p);
                if (v < 0) {
                    v = strtol(p, &end, 0);
                    if (end == p || info.value.enumerated.items == 0)
                        return -1;
                    if (v < 0)
                        v = 0;
                    if (v > (long)info.value.enumerated.items - 1)
                        v = info.value.enumerated.items - 1;
                    p = end;
                }
                val.value.enumerated.item[i] = v;
                break;
            default:
                LOGE("mixer: '%s' has an unsupported type %d", name, (int)info.type);
                return -1;
            }
        }
        if (!per_channel)
            p = values;
        else if (*p == ',')
            p++;
    }

    if (ioctl(ctl_fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &val) < 0) {
        LOGE("mixer: write '%s' failed (%m)", name);
        return -1;
    }
    return 0;
}

// 0 if the control was set natively.
static int mixer_native(const char *name, const char *values) {
    if (!perf_opt.native_audio)
        return -1;
    pthread_mutex_lock(&mixer_mutex);
    int ret = ctl_broken ? -1 : native_cset(name, values);
    pthread_mutex_unlock(&mixer_mutex);
    return ret;
}

#else // EMULATOR_BUILD: no codec; the commands below are logged and skipped

static int mixer_native(const char *name, const char *values) {
    return -1;
}

#endif

void mixer_cset(const char *name, const char *values) {
    if (mixer_native(name, values) == 0)
        return;
    char cmd[160];
    snprintf(cmd, sizeof(cmd), "amixer cset name='%s' %s", name, values);
    system_exec(cmd);
}

void mixer_set(const char *name, int value) {
    char values[16];
    snprintf(values, sizeof(values), "%d", value);
    mixer_cset(name, values);
}

void mixer_set2(const char *name, int left, int right) {
    char values[32];
    snprintf(values, sizeof(values), "%d,%d", left, right);
    mixer_cset(name, values);
}

///////////////////////////////////////////////////////////////////////////////
// audio_sel.sh

typedef struct {
    const char *name;
    const char *value;
} mixer_step_t;

#define STEPS_END {NULL, NULL}

// amixer_clear
#define CLEAR_STEPS                                     \
    {"AIF1 AD0L Mixer ADCL Switch", "0"},               \
        {"AIF1 AD0R Mixer ADCR Switch", "0"},           \
        {"LADC input Mixer MIC1 boost Switch", "0"},    \
        {"RADC input Mixer MIC1 boost Switch", "0"},    \
        {"LADC input Mixer MIC2 boost Switch", "0"},    \
        {"RADC input Mixer MIC2 boost Switch", "0"},    \
        {"LADC input Mixer LINEINL Switch", "0"},       \
        {"RADC input Mixer LINEINR Switch", "0"}

// amixer_adc 1
#define ADC_ON_STEPS                          \
    {"AIF1 AD0L Mixer ADCL Switch", "1"},     \
        {"AIF1 AD0R Mixer ADCR Switch", "1"}

static const mixer_step_t in_mic1[] = {CLEAR_STEPS,
                                       {"LADC input Mixer MIC1 boost Switch", "1"},
                                       {"RADC input Mixer MIC1 boost Switch", "1"},
                                       ADC_ON_STEPS,
                                       STEPS_END};
static const mixer_step_t in_mic2[] = {CLEAR_STEPS,
                                       {"LADC input Mixer MIC2 boost Switch", "1"},
                                       {"RADC input Mixer MIC2 boost Switch", "1"},
                                       ADC_ON_STEPS,
                                       STEPS_END};
static const mixer_step_t in_linein[] = {CLEAR_STEPS,
                                         {"LADC input Mixer LINEINL Switch", "1"},
                                         {"RADC input Mixer LINEINR Switch", "1"},
                                         ADC_ON_STEPS,
                                         STEPS_END};
static const mixer_step_t out_on[] = {{"Lineout Switch", "1"},
                                      {"LINEOUTL Mux", "1"},
                                      {"LINEOUTR Mux", "1"},
                                      {"lineout volume", "31"},
                                      STEPS_END};
static const mixer_step_t out_off[] = {{"Lineout Switch", "0"},
                                       {"LINEOUTL Mux", "0"},
                                       {"LINEOUTR Mux", "0"},
                                       STEPS_END};
#define LINEOUT_MIXER_STEPS(var, left, right, onoff) \
    static const mixer_step_t var[] = {{left, onoff}, {right, onoff}, STEPS_END}
LINEOUT_MIXER_STEPS(out_dac_on, "Left Output Mixer DACL Switch", "Right Output Mixer DACR Switch", "1");
LINEOUT_MIXER_STEPS(out_dac_off, "Left Output Mixer DACL Switch", "Right Output Mixer DACR Switch", "0");
LINEOUT_MIXER_STEPS(out_mic1_on, "Left Output Mixer MIC1Booststage Switch", "Right Output Mixer MIC1Booststage Switch", "1");
LINEOUT_MIXER_STEPS(out_mic1_off, "Left Output Mixer MIC1Booststage Switch", "Right Output Mixer MIC1Booststage Switch", "0");
LINEOUT_MIXER_STEPS(out_mic2_on, "Left Output Mixer MIC2Booststage Switch", "Right Output Mixer MIC2Booststage Switch", "1");
LINEOUT_MIXER_STEPS(out_mic2_off, "Left Output Mixer MIC2Booststage Switch", "Right Output Mixer MIC2Booststage Switch", "0");
LINEOUT_MIXER_STEPS(out_linein_on, "Left Output Mixer LINEINL Switch", "Right Output Mixer LINEINR Switch", "1");
LINEOUT_MIXER_STEPS(out_linein_off, "Left Output Mixer LINEINL Switch", "Right Output Mixer LINEINR Switch", "0");

static const struct {
    const char *cmd;
    const mixer_step_t *steps;
} audio_sel_cmds[] = {
    {"in_mic1", in_mic1},
    {"in_mic2", in_mic2},
    {"in_linein", in_linein},
    {"out_on", out_on},
    {"out_off", out_off},
    {"out_dac_on", out_dac_on},
    {"out_dac_off", out_dac_off},
    {"out_mic1_on", out_mic1_on},
    {"out_mic1_off", out_mic1_off},
    {"out_mic2_on", out_mic2_on},
    {"out_mic2_off", out_mic2_off},
    {"out_linein_on", out_linein_on},
    {"out_linein_off", out_linein_off},
};

void audio_sel(const char *cmd) {
    if (perf_opt.native_audio) {
        for (unsigned i = 0; i < sizeof(audio_sel_cmds) / sizeof(audio_sel_cmds[0]); i++) {
            if (strcmp(audio_sel_cmds[i].cmd, cmd) != 0)
                continue;
            const mixer_step_t *step = audio_sel_cmds[i].steps;
            while (step->name && mixer_native(step->name, step->value) == 0)
                step++;
            if (!step->name)
                return; // all set
            break;      // a control failed: the script redoes the whole command
        }
    }

    char buf[128];
    snprintf(buf, sizeof(buf), "%s %s", AUDIO_SEL_SH, cmd);
    system_exec(buf);
}
