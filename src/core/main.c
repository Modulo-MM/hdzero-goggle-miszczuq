#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <log/log.h>
#include <lvgl/lvgl.h>
#include <minIni.h>

#include "lang/language.h"

#ifdef EMULATOR_BUILD
#include "SDLaccess.h"
SDL_mutex *global_sdl_mutex;
#endif

#include "bmi270/accel_gyro.h"
#include "core/app_state.h"
#include "core/boot_timing.h"
#include "core/common.hh"
#include "core/elrs.h"
#include "core/ht.h"
#include "core/input_device.h"
#include "core/osd.h"
#include "core/perf_monitor.h"
#include "core/scan_core.h"
#include "core/self_test.h"
#include "core/settings.h"
#include "core/sleep_mode.h"
#include "core/thread.h"
#include "platform/paths.h"
#include "driver/beep.h"
#include "driver/dm5680.h"
#include "driver/esp32.h"
#include "driver/fans.h"
#include "driver/gpadc.h"
#include "driver/gpio.h"
#include "driver/hardware.h"
#include "driver/i2c.h"
#include "driver/it66021.h"
#include "driver/it66121.h"
#include "driver/mcp3021.h"
#include "driver/rtc.h"
#include "driver/rtc6715.h"
#include "driver/screen.h"
#include "driver/tp2825.h"
#include "ui/page_power.h"
#include "ui/page_scannow.h"
#include "ui/page_source.h"
#include "ui/page_storage.h"
#include "ui/ui_image_setting.h"
#include "ui/ui_main_menu.h"
#include "ui/ui_osd_element_pos.h"
#include "ui/ui_porting.h"
#include "ui/ui_statusbar.h"
#include "util/sdcard.h"
#include "util/system.h"

int gif_cnt = 0;

// See the ELRS backpack readiness gate in main()'s loop.
#define ELRS_ENABLE_MIN_DELAY_SEC 5
#define ELRS_ENABLE_MAX_DELAY_SEC 15

// Set for the lifetime of the boot-time auto-scan menu loop below, so the
// ELRS backpack readiness gate (main()) can wait it out instead of firing
// on a fixed delay while the scan is still actively driving the UI/app
// state.
static volatile bool g_boot_autoscan_running = false;

static void *thread_autoscan(void *ptr) {
    g_boot_autoscan_running = true;
    for (;;) {
        pthread_mutex_lock(&lvgl_mutex);
        main_menu_show(true);
        app_state_push(APP_STATE_SUBMENU);
        submenu_enter();
        pthread_mutex_unlock(&lvgl_mutex);

        if (g_autoscan_exit)
            goto a_exit;

        sleep(5);

        if (g_autoscan_exit)
            goto a_exit;
    }

a_exit:
    g_boot_autoscan_running = false;
    pthread_exit(NULL);
    return NULL;
}

// In normal operation the version thread animates progress_bar (its 100ms
// loop calls progress_bar_update, page_version.c), but start_running executes
// before create_threads -- so a direct boot entry showed a frozen menu for
// the 5-10s the source bring-up takes. Tick the bar from this short-lived
// thread so Load from Boot shows the same loading animation (and the same
// per-source/Auto-BW fill rates) as the equivalent Source-page pick; hand
// off as soon as the main loop starts.
static void *thread_boot_progress(void *ptr) {
    while (progress_bar.start && !g_init_done) {
        progress_bar_update();
        usleep(100000);
    }
    if (!g_init_done)
        progress_bar_update(); // run the bar's end-of-use cleanup tick
    pthread_exit(NULL);
    return NULL;
}

static void boot_progress_start(void) {
    pthread_t pid;
    progress_bar.start = 1;
    if (pthread_create(&pid, NULL, thread_boot_progress, NULL) == 0)
        pthread_detach(pid);
}

void start_running(void) {
    int source;
    if (g_setting.autoscan.source == SETTING_AUTOSCAN_SOURCE_LAST)
        source = g_setting.autoscan.last_source;
    else
        source = g_setting.autoscan.source;

#if defined(HDZBOXPRO) || defined(HDZGOGGLE2)
    // Booting into an explicit source must not inherit Dual mode from the
    // previous session (auto_protocol_detect persists in setting.ini): the
    // Source page's direct picks disable it, so the boot equivalents do too.
    // Without this, Default=HDZero booted with the Dual dial/channel
    // behavior after a Dual session.
    if (source != SETTING_AUTOSCAN_SOURCE_AUTO_DETECT &&
        g_setting.source.auto_protocol_detect) {
        g_setting.source.auto_protocol_detect = false;
        settings_put_bool("source", "auto_protocol_detect", false);
    }
#endif

    // Auto Scan=On + Load from Boot=No: run a Scan Now sweep for the default
    // source's protocol at boot instead of entering the source directly
    // (HDZero's historical behavior, now offered for Analog and Dual too).
    // AV In and HDMI In always load directly -- there is nothing to scan --
    // and the G1's external analog module cannot be scanned either. A dial
    // up/down during initialization (g_init_done) skips the scan, as before.
    bool boot_scan = (g_setting.autoscan.status == SETTING_AUTOSCAN_STATUS_ON) &&
                     !g_setting.autoscan.load_from_boot && (g_init_done == 0);

    if (source == SETTING_AUTOSCAN_SOURCE_HDZERO) { // HDZero
        g_source_info.source = SOURCE_HDZERO;
        if (boot_scan) {
            pthread_t pid;
            g_autoscan_exit = false;
            page_scannow_set_boot_scan_mode(0); // SCAN_MODE_HDZERO
            pthread_create(&pid, NULL, thread_autoscan, NULL);
        } else if (g_setting.autoscan.status == SETTING_AUTOSCAN_STATUS_LAST ||
                   (g_setting.autoscan.status == SETTING_AUTOSCAN_STATUS_ON &&
                    g_setting.autoscan.load_from_boot)) {
            app_state_push(APP_STATE_VIDEO);
            boot_progress_start(); // same loading bar as a Source-page pick
            app_switch_to_hdzero(true);
        } else { // auto scan disabled, go to go directly to last saved channel
            app_state_push(APP_STATE_MAINMENU);
        }
#if defined(HDZBOXPRO) || defined(HDZGOGGLE2)
    } else if (source == SETTING_AUTOSCAN_SOURCE_AUTO_DETECT) {
        if (boot_scan) {
            pthread_t pid;
            g_autoscan_exit = false;
            page_scannow_set_boot_scan_mode(2); // SCAN_MODE_AUTO (Dual)
            pthread_create(&pid, NULL, thread_autoscan, NULL);
        } else {
            // Probes both protocols at the current channel and enters video
            // on whichever responds; pushes APP_STATE_VIDEO and sets
            // g_source_info.source itself. It sets the bar's per-Auto-BW fill
            // rate; the ticker just needs to be running before it blocks.
            boot_progress_start();
            page_source_select_auto_detect();
        }
    } else if (source == SETTING_AUTOSCAN_SOURCE_AV_MODULE && boot_scan) {
        // Boot scan on the built-in analog receiver. (The G1 has no built-in
        // analog, so its AV Module default keeps the direct entry below.)
        pthread_t pid;
        g_autoscan_exit = false;
        page_scannow_set_boot_scan_mode(1); // SCAN_MODE_ANALOG
        pthread_create(&pid, NULL, thread_autoscan, NULL);
#endif
    } else {
        app_state_push(APP_STATE_VIDEO);
        if (source == SETTING_AUTOSCAN_SOURCE_AV_MODULE) { // AV Module
            g_hw_stat.av_pal[1] = g_setting.source.analog_format;
            app_switch_to_analog(0);
            g_source_info.source = SOURCE_AV_MODULE;
        } else if (source == SETTING_AUTOSCAN_SOURCE_AV_IN) { // AV in
            g_hw_stat.av_pal[0] = g_setting.source.analog_format;
            app_switch_to_analog(1);
            g_source_info.source = SOURCE_AV_IN;
        } else { // HDMI in
            sleep(2);
            // g_source_info.hdmi_in_status = IT66021_Sig_det();
            // if (g_source_info.hdmi_in_status) {
            app_switch_to_hdmi_in();
            g_source_info.source = SOURCE_HDMI_IN;
            //} else {
            //    g_source_info.source = SOURCE_HDZERO;
            //    app_state_push(APP_STATE_MAINMENU);
            //}
        }
    }

    // ELRS backpack is brought up later, once the goggle has finished booting
    // (see deferred enable in main()). Enabling it here would land mid HDZero
    // bring-up and wedge the ESP.
}

#ifdef HDZGOGGLE2
// Boot speed-up (G2): the BMI270 bring-up (~0.75 s, mostly its config-file
// upload) runs on a worker thread alongside the rest of init. Nothing on the
// way to video needs the IMU; I2C transactions are serialized by i2c_mutex,
// the IMU timer skips reads until bmi270_ready(), and the thread is joined
// before ht_enable()/ht_disable().
static pthread_t bmi270_tid;
static bool bmi270_async = false;
// With the early HDZero bring-up the BMI270 waits until HDZero video is up:
// its ~0.7 s of config upload shares the one I2C bus with the receivers'.
static bool bmi270_deferred = false;
static bool hdz_early_rf = false;

static void *thread_bmi270_init(void *arg) {
    if (arg)
        i2c_set_background(true); // deferred: never in the way of the UI's I2C
    enable_bmi270();
    boot_mark("enable_bmi270");
    return NULL;
}

// Boot experiment (bootopt/analog_early_rx): does this boot go straight to the
// Built-in analog receiver? Same decision as start_running().
static bool boot_enters_builtin_analog(void) {
    int source = g_setting.autoscan.source == SETTING_AUTOSCAN_SOURCE_LAST ? g_setting.autoscan.last_source
                                                                          : g_setting.autoscan.source;
    bool boot_scan = g_setting.autoscan.status == SETTING_AUTOSCAN_STATUS_ON && !g_setting.autoscan.load_from_boot;
    return source == SETTING_AUTOSCAN_SOURCE_AV_MODULE && !boot_scan &&
           g_setting.source.analog_module == SETTING_SOURCES_ANALOG_MODULE_INTERNAL;
}
static bool analog_rx_early = false;

// Does this boot go straight into HDZero video? Same decision as
// start_running()'s HDZero branch.
static bool boot_enters_hdzero(void) {
    int source = g_setting.autoscan.source == SETTING_AUTOSCAN_SOURCE_LAST ? g_setting.autoscan.last_source
                                                                          : g_setting.autoscan.source;
    return source == SETTING_AUTOSCAN_SOURCE_HDZERO &&
           (g_setting.autoscan.status == SETTING_AUTOSCAN_STATUS_LAST ||
            (g_setting.autoscan.status == SETTING_AUTOSCAN_STATUS_ON && g_setting.autoscan.load_from_boot));
}

// rc.sh sets the vdpo mode in the background on the G2 (~1.5 s in the
// driver) and removes this file when done. Wait for it before touching the
// display or the FPGA; capped, and a no-op with an rc.sh that doesn't do it.
#define VDPO_PENDING_FILE "/tmp/vdpo_pending"
#define VDPO_MODE_FILE    "/tmp/vdpo_mode"
static void wait_for_vdpo(void) {
    for (int i = 0; i < 300 && access(VDPO_PENDING_FILE, F_OK) == 0; i++)
        usleep(10000);
    if (access(VDPO_PENDING_FILE, F_OK) == 0) {
        LOGE("vdpo still pending after 3 s, setting it here");
        system_exec("dispw -s vdpo 1080p50");
        system_exec("echo 1080p50 > " VDPO_MODE_FILE);
        unlink(VDPO_PENDING_FILE);
    }
    boot_mark("vdpo_ready");
}
#endif

static void device_init(void) {
    self_test();
    boot_mark("self_test");
#ifdef HDZGOGGLE2
    if (hdz_early_rf)
        bmi270_deferred = true; // started after start_running(), see main()
    else
        bmi270_async = pthread_create(&bmi270_tid, NULL, thread_bmi270_init, NULL) == 0;
    if (!bmi270_async && !bmi270_deferred) {
        enable_bmi270();
        boot_mark("enable_bmi270");
    }
    // IT66021_init() runs later, in main(): see IT66021_reset_early().
#else
    enable_bmi270();
    boot_mark("enable_bmi270");
    IT66021_init();
    boot_mark("IT66021_init");
#endif
    IT66121_init();
    boot_mark("IT66121_init");
    TP2825_init(1, g_setting.source.analog_format);
    boot_mark("TP2825_init");
    DM5680_req_ver();
    fans_top_setspeed(g_setting.fans.top_speed);
    boot_mark("dm5680_ver_fans");
}

void lvgl_init() {
    lv_init();
    style_init();
    lvgl_init_porting();
    lv_disp_t *dispp = lv_disp_get_default();
    lv_theme_t *theme = lv_theme_default_init(dispp, lv_color_make(0xff, 0xff, 0xff), lv_palette_main(LV_PALETTE_RED),
                                              false, LV_FONT_DEFAULT);
    lv_disp_set_theme(dispp, theme);
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_make(64, 64, 64), 0);
}

int main(int argc, char *argv[]) {
    boot_mark("main_entry");
    pthread_mutex_init(&lvgl_mutex, NULL);

#ifdef EMULATOR_BUILD
    global_sdl_mutex = SDL_CreateMutex();
    if (global_sdl_mutex == NULL) {
        LOGE("Failed to create an SDL mutex!");
    }
#endif

    // Resolve emulator filesystem roots before settings/resources are read.
    // On hardware this keeps the compiled-in /mnt/app and /mnt/extsd paths.
    paths_init();

    // 1. Recall configuration
    settings_init();
    settings_load();
    perf_opts_init();
#ifdef HDZGOGGLE2
    HDZero_restore_cam_mode(); // the last HDZero camera mode, for the first Display_* call
#endif
    boot_mark("settings_load");
#if defined(HDZBOXPRO) || defined(HDZGOGGLE2) || defined(HDZGOGGLE)
    scan_core_self_check();
    boot_mark("scan_core_self_check");
#endif
    language_init();
    boot_mark("language_init");
#ifdef HDZGOGGLE2
    vclk_phase_init();
#endif
    pclk_phase_init();
    boot_mark("pclk_phase_init");

    // 2. Initialize communications.
    rtc_init();
    iic_init();
    gpio_init();
    uart_init();

#if defined(HDZGOGGLE2) || defined(HDZBOXPRO)
    gpadc_init();
#endif
    boot_mark("comm_init");
#ifdef HDZGOGGLE2
    wait_for_vdpo();
    // Start the HDMI-in receiver's reset hold now so it overlaps the init
    // below; self_test toggles the same reset, so not when it is enabled.
    if (!g_setting.storage.selftest)
        IT66021_reset_early();
#endif

    // 3. Initialize core devices.
    mcp3021_init();
    boot_mark("mcp3021_init");
    input_device_init();
    boot_mark("input_device_init");
    hw_stat_init();
    boot_mark("hw_stat_init");
#ifdef HDZGOGGLE2
    // Early HDZero receiver bring-up (see HDZero_open_early);
    // bootopt/disable_hdzero_early_rf turns it off for a boot.
    hdz_early_rf = boot_enters_hdzero() && !boot_opt("disable_hdzero_early_rf");
#endif
    device_init();
#ifdef HDZGOGGLE2
    if (boot_enters_builtin_analog() && boot_opt("analog_early_rx")) {
        rtc6715_power_on_early();
        analog_rx_early = true;
        boot_mark("analog_rx_power_on");
    }
    // Bring the HDZero receivers up (~2 s, mostly I2C to the FPGA's SPI
    // bridge) on a worker while the UI is built; app_switch_to_hdzero()'s
    // HDZero_open() waits for it. The DM5680 UART commands of device_init()
    // are done by now. The analog receiver goes off first, as
    // app_switch_to_hdzero() does before opening.
    if (hdz_early_rf) {
        rtc6715.init(0, 0);
        scan_core_notify_analog_powered_off();
        HDZero_open_early(hdzero_effective_bw());
    }
#endif
    esp32_init();
    boot_mark("esp32_init");
    elrs_init();
    boot_mark("elrs_init");
    ht_init();
    boot_mark("ht_init");
    beep_init();
    boot_mark("beep_init");

    // 4. Initilize UI
    lvgl_init();
    boot_mark("lvgl_init");
    main_menu_init();
    boot_mark("main_menu_init");
#ifdef HDZGOGGLE2
    IT66021_init(); // its reset hold has been running since comm_init
    boot_mark("IT66021_init");
    if (analog_rx_early) {
        rtc6715_tune_early(g_setting.source.analog_channel - 1,
                           g_setting.record.audio_source == SETTING_RECORD_AUDIO_SOURCE_AV_IN);
        boot_mark("analog_rx_tuned");
    }
#endif
    statusbar_init();
    boot_mark("statusbar_init");
    lv_timer_handler();
    boot_mark("first_lv_timer_handler");

    // 5. Prepare Display
    screen.start_up();
    boot_mark("screen_start_up");
    Display_UI_init();
    boot_mark("Display_UI_init");
    screen.pattern(0, 0, 0);
    boot_mark("screen_pattern");
    osd_init();
    boot_mark("osd_init");
    ims_init();
    boot_mark("ims_init");
    ui_osd_element_pos_init();
    boot_mark("osd_element_pos_init");

    // 6. Enable functionality
#ifdef HDZGOGGLE2
    if (bmi270_async) {
        pthread_join(bmi270_tid, NULL);
        boot_mark("bmi270_joined");
    }
#endif
    if (g_setting.ht.enable) {
        ht_enable();
    } else {
        ht_disable();
    }
    boot_mark("ht_enable");

    // 7. Start threads
    g_bootup_sdcard_state = sdcard_mounted() ? 1 : 0;
    boot_mark("start_running_begin");
    start_running();
    boot_mark("start_running_end");
#ifdef HDZGOGGLE2
    // The IMU is read only once bmi270_ready() (the IMU timer checks), and
    // ht_enable()/ht_disable() above do not touch the chip.
    if (bmi270_deferred) {
        if (pthread_create(&bmi270_tid, NULL, thread_bmi270_init, (void *)1) == 0) {
            pthread_detach(bmi270_tid);
        } else {
            enable_bmi270();
            boot_mark("enable_bmi270");
        }
    }
#endif
    create_threads();
    boot_mark("create_threads");

    // 8. Synthetic counter for gif refresh
    gif_cnt = 0;

    // 8.1 set initial analog module power state
    Analog_Module_Power(0);

    // Head alarm
    head_alarm_init();

    // 10. Execute main loop
    g_init_done = 1;
    boot_mark("init_done");
    // rc.sh holds back the SD log backup and the services until this exists,
    // so they don't compete with the bring-up.
    close(open("/tmp/app_init_done", O_WRONLY | O_CREAT, 0644));
    boot_timing_start();
    perf_monitor_start();
    bool first_loop_done = false;

#ifdef EMULATOR_BUILD
    input_device_print_help();
#endif

    // Bring the ELRS backpack up once boot has genuinely settled, not just
    // after a fixed delay: enabling it during the busy HDZero bring-up (or
    // while the boot-time auto-scan menu loop is still actively driving the
    // UI/app state) wedges the ESP. ELRS_ENABLE_MIN_DELAY_SEC gives the
    // synchronous HDZero switch time to finish; ELRS_ENABLE_MAX_DELAY_SEC
    // caps how long an idle boot-scan screen can hold the backpack off.
    // (The analog path settles well before the floor, so it is unaffected.)
    // A watchdog (elrs_watchdog_start) backstops whatever this gate misses.
    time_t boot_time = time(NULL);
    bool elrs_backpack_started = false;

    for (;;) {
        uint32_t t = perf_loop_now(); // PERF.log main-loop breakdown
        pthread_mutex_lock(&lvgl_mutex);
        t = perf_loop_section(PERF_LOOP_OTHER, t);
        main_menu_update();
        t = perf_loop_section(PERF_LOOP_MENU, t);
        rtc_persist_tick();
        sleep_reminder();
        statubar_update();
        t = perf_loop_section(PERF_LOOP_STATUSBAR, t);
        osd_hdzero_update();
        t = perf_loop_section(PERF_LOOP_OSD, t);
#ifdef EMULATOR_BUILD
        if (gif_cnt % 200 == 0)
            osd_inject_mock_fc();
#endif
        ims_update();
        ui_osd_element_pos_update();
        ht_detect_motion();
        lvgl_orbit_tick();
        t = perf_loop_section(PERF_LOOP_OTHER, t);
        lv_timer_handler();
        t = perf_loop_section(PERF_LOOP_LVGL, t);
        source_status_timer();
        pthread_mutex_unlock(&lvgl_mutex);
        perf_loop_section(PERF_LOOP_OTHER, t);
        perf_note_loop_pass();

        if (!first_loop_done) {
            first_loop_done = true;
            boot_mark("main_loop_first_pass");
        }

        if (!elrs_backpack_started) {
            time_t elapsed = time(NULL) - boot_time;
            bool settled = !g_boot_autoscan_running && elapsed >= ELRS_ENABLE_MIN_DELAY_SEC;
            if (settled || elapsed >= ELRS_ENABLE_MAX_DELAY_SEC) {
                elrs_backpack_started = true;
                if (g_setting.elrs.enable) {
                    enable_esp32();
                    elrs_watchdog_start();
                    boot_mark("elrs_backpack_enabled");
                }
            }
        }

#if defined(EMULATOR_BUILD) && (defined(__APPLE__) || defined(_WIN32))
        // Cocoa and Windows require SDL events to be pumped on the main thread.
        input_device_pump();
#endif

        usleep(5000);
        gif_cnt++;
    }
    return 0;
}
