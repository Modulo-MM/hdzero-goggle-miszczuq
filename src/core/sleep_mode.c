#include "sleep_mode.h"

#include "core/app_state.h"
#include "core/boot_timing.h"
#include "core/common.hh"
#include "core/dvr.h"
#include "core/settings.h"
#include "driver/beep.h"
#include "driver/dm5680.h"
#include "driver/fans.h"
#include "driver/hardware.h"
#include "driver/rtc6715.h"
#include "driver/screen.h"
#include "log/log.h"
#include "ui/page_fans.h"

static app_state_t previousState;
static int fans_auto_mode_save;
static fan_speed_t fan_speed_save;

// Sleep reminder beep: every SLEEP_REMINDER_MS. It was 4000 main-loop passes,
// "about five minutes" when a pass redrew the whole screen; a pass is far
// shorter now, so count wall-clock time instead.
#define SLEEP_REMINDER_MS (5 * 60 * 1000)
static uint32_t sleep_reminder_last = 0;

bool isSleeping = false;

void go_sleep() {
    LOGI("Entering sleep mode");
    wake_timing_sleep();
    previousState = g_app_state;
    app_state_push(APP_STATE_SLEEP);

    // Stop DVR
    dvr_cmd(DVR_STOP);
#if defined(HDZGOGGLE) || defined(HDZGOGGLE2)
    dvr_update_vi_conf(VR_1080P30);
#elif defined(HDZBOXPRO)
    dvr_update_vi_conf(VR_720P60);
#endif

    screen_set_inhibited(SCREEN_INHIBIT_SLEEP, true);

    // Turn off HDZero Receiver
    HDZero_Close();

    // Turn off Analog Receiver
    if (getHwRevision() == HW_REV_2) {
        DM5680_ExternalAnalog_Power(1);
    }
    rtc6715.init(0, 0);

    // Minimum fan
    fans_auto_mode_save = g_setting.fans.auto_mode;
    fan_speed_save.top = fan_speed.top;
    fan_speed_save.left = fan_speed.left;
    fan_speed_save.right = fan_speed.right;
    g_setting.fans.top_speed = MIN_FAN_TOP;
    g_setting.fans.left_speed = MIN_FAN_SIDE;
    g_setting.fans.right_speed = MIN_FAN_SIDE;
    g_setting.fans.auto_mode = 0;
    fans_top_setspeed(MIN_FAN_TOP);
    fans_left_setspeed(MIN_FAN_SIDE);
    fans_right_setspeed(MIN_FAN_SIDE);
    isSleeping = true;
    sleep_reminder_last = lv_tick_get();
}

void wake_up() {
    LOGI("Exiting sleep mode");
    // G2: sleep entered from analog video goes straight back to it (see
    // app_wake_to_analog); everything else re-enters through the menu.
    bool fast_analog = false;
    bool fast_hdzero = false;
#ifdef HDZGOGGLE2
    fast_analog = previousState == APP_STATE_VIDEO &&
                  (g_source_info.source == SOURCE_AV_MODULE || g_source_info.source == SOURCE_AV_IN);
    fast_hdzero = previousState == APP_STATE_VIDEO && g_source_info.source == SOURCE_HDZERO;
#endif
    wake_timing_wake(g_source_info.source, fast_analog || fast_hdzero);

    isSleeping = false;
    // HDZero fast wake: screens stay dark until the receiver is back (~2 s)
    if (!fast_hdzero) {
        screen_set_inhibited(SCREEN_INHIBIT_SLEEP, false);
        wake_timing_mark("screen_on");
    }

    Analog_Module_Power(1);
    g_setting.fans.right_speed = fan_speed_save.right;
    g_setting.fans.left_speed = fan_speed_save.left;
    fans_right_setspeed(fan_speed_save.right);
    fans_left_setspeed(fan_speed_save.left);

    g_setting.fans.top_speed = fan_speed_save.top;
    g_setting.fans.auto_mode = fans_auto_mode_save;
    fans_top_setspeed(fan_speed_save.top);

    app_state_push(previousState);
    if (previousState == APP_STATE_SUBMENU) {
        submenu_exit();
    } else if (fast_analog) {
        app_wake_to_analog(g_source_info.source == SOURCE_AV_IN);
        wake_timing_mark("analog_rx_on");
    } else if (fast_hdzero) {
        app_wake_to_hdzero();
        screen_set_inhibited(SCREEN_INHIBIT_SLEEP, false);
        wake_timing_mark("screen_on");
        wake_timing_mark("source_reentered");
    } else if (previousState == APP_STATE_VIDEO) {
        app_switch_to_menu();          // Necessary to display the progress bar
        app_state_push(previousState); // Because app_switch_to_menu() pushes main menu state
        app_exit_menu();
        wake_timing_mark("source_reentered");
    }
}

void sleep_reminder() {
    if (isSleeping == false) {
        return;
    }

    if (lv_tick_elaps(sleep_reminder_last) >= SLEEP_REMINDER_MS) {
        beep_dur(BEEP_VERY_SHORT);
        sleep_reminder_last = lv_tick_get();
    }
}
