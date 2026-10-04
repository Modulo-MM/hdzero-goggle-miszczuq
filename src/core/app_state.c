#include "app_state.h"

#include <log/log.h>
#include <minIni.h>
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>

#include "core/boot_timing.h"
#include "core/common.hh"
#include "core/dvr.h"
#include "core/input_device.h"
#include "core/msp_displayport.h"
#include "core/osd.h"
#include "core/scan_core.h"
#include "driver/dm5680.h"
#include "driver/dm6302.h"
#include "driver/hardware.h"
#include "driver/it66121.h"
#include "driver/rtc6715.h"
#include "driver/screen.h"
#include "ui/page_common.h"
#include "ui/page_imagesettings.h"
#include "ui/ui_image_setting.h"
#include "ui/ui_main_menu.h"
#include "ui/ui_porting.h"
#include "util/system.h"

app_state_t g_app_state = APP_STATE_MAINMENU;

extern int valid_channel_tb[10];
extern int user_select_index;

void app_state_push(app_state_t state) {
    g_app_state = state;
}

void app_switch_to_menu() {
    if (g_app_state == APP_STATE_IMS) {
        ims_save();
        set_slider_value();
    }

    app_state_push(APP_STATE_MAINMENU);

    // Stop recording if switching to menu mode from video mode regardless
    dvr_cmd(DVR_STOP);

#if defined HDZGOGGLE
    dvr_update_vi_conf(VR_1080P30);
#elif defined HDZBOXPRO
    dvr_update_vi_conf(VR_720P60);
#elif defined HDZGOGGLE2
    dvr_update_vi_conf(VR_1080P30);
#endif

    Display_UI();
    image_settings_apply(SETTING_IMAGE_SOURCE_HDZERO);
    lvgl_switch_to_1080p();
    exit_tune_channel();
    osd_show(false);
    g_bShowIMS = false;
    main_menu_show(true);
    HDZero_Close();
    g_sdcard_det_req = 1;
    if (g_source_info.source == SOURCE_HDMI_IN) // HDMI
        IT66121_init();

    rtc6715.init(0, 0);
#if defined(HDZBOXPRO) || defined(HDZGOGGLE2)
    scan_core_notify_analog_powered_off();
#endif
    system_script(REC_STOP_LIVE);
}

void app_exit_menu() {
    if (SOURCE_HDZERO == g_source_info.source) {
        progress_bar.start = 1;
        app_switch_to_hdzero(true);
    } else if (SOURCE_HDMI_IN == g_source_info.source) {
        app_switch_to_hdmi_in();
    } else {
        app_switch_to_analog(g_source_info.source == SOURCE_AV_IN);
    }
}

// Power and tune the receiver feeding the analog source (shared by the
// source switch and the G2 fast wake from sleep).
static void analog_receiver_on(bool is_av_in) {
    if (is_av_in) {
        rtc6715.init(0, 0);
#if defined(HDZBOXPRO) || defined(HDZGOGGLE2)
        scan_core_notify_analog_powered_off();
#endif
    } else {
#if defined(HDZGOGGLE2)
        // Analog "Module" source. The Built-in receiver (rtc6715) and the
        // Expansion module share the bay's analog input, so only one may drive
        // it. When the user picks Expansion, powering and tuning the internal
        // receiver here drives that shared input and overrides the external
        // module -- the cause of "External is ignored, always uses Built-in".
        // Keep the internal receiver powered down; Analog_Module_Power() keeps
        // the expansion module powered while it is the active source.
        //
        // Dual (auto_protocol_detect) always uses the Built-in receiver -- it
        // probes and tunes the internal rtc6715 and cannot use the Expansion
        // module -- so it falls through to the Built-in branch regardless of
        // the Analog Module setting.
        if (g_setting.source.analog_module == SETTING_SOURCES_ANALOG_MODULE_EXTERNAL &&
            !g_setting.source.auto_protocol_detect) {
            rtc6715.init(0, 0);
            scan_core_notify_analog_powered_off();
        } else
#endif
        {
            rtc6715.init(1, g_setting.record.audio_source == SETTING_RECORD_AUDIO_SOURCE_AV_IN);
            rtc6715.set_ch(g_setting.source.analog_channel - 1);
#if defined(HDZBOXPRO) || defined(HDZGOGGLE2)
            scan_core_notify_analog_powered_on();
#endif
        }
    }
}

#ifdef HDZGOGGLE2
// Fast wake from sleep straight back into analog video (G2). Sleep entered
// from video leaves the display in the analog mode, the decoder configured
// and the audio routing as they were, and only powers the receivers down
// (go_sleep), so power the receiver back up instead of going through the
// menu and a full source re-entry (~4 s: two vdpo mode sets, sleep(1),
// audio routing).
void app_wake_to_analog(bool is_av_in) {
    dvr_update_vi_conf(VR_720P50); // go_sleep() set it for 1080p30
    analog_receiver_on(is_av_in);
}

// Fast wake from sleep straight back into HDZero video (G2). The display
// stayed in the camera's mode and go_sleep() only closed the receiver
// (HDZero_Close), so reopen it and retune instead of the menu detour and a
// full source switch (two vdpo mode sets, ~1.1 s each). DM6302_init uses
// the cached EFUSE data.
void app_wake_to_hdzero(void) {
    scan_core_notify_analog_powered_off(); // go_sleep() powered the RTC6715 down
    dvr_update_vi_conf(CAM_MODE); // go_sleep() set it for 1080p30
    HDZero_open(hdzero_effective_bw());
    DM6302_SetChannel(g_setting.source.hdzero_band, (g_setting.scan.channel - 1) & 0x7f);
    DM5680_clear_vldflg();
    DM5680_req_vldflg();
}
#endif

#ifdef HDZGOGGLE2
// Boot speed-up (G2): the audio routing is several audio_sel.sh runs, each a
// shell plus a few amixer processes (~1-1.4 s in all), and the first video
// frame does not need it. During the initial bring-up it runs on a worker;
// dvr_mute/restore_live_audio wait for it (dvr_audio_route_lock).
static void *thread_boot_audio_route(void *arg) {
    dvr_select_audio_source(g_setting.record.audio_source);
    dvr_enable_line_out(true);
    dvr_audio_route_unlock();
    boot_mark("av_audio_routed");
    return NULL;
}
#endif

void app_switch_to_analog(bool is_av_in) {
    boot_mark("switch_to_analog_begin");
    dvr_update_vi_conf(VR_720P50);
    osd_fhd(0);
    osd_show(true);
    lvgl_switch_to_720p();
    osd_clear();
    lv_timer_handler();
    Display_Osd(g_setting.record.osd);

    Source_AV(is_av_in);
    image_settings_apply(SETTING_IMAGE_SOURCE_ANALOG);
    boot_mark("av_image_settings");

    analog_receiver_on(is_av_in);
    boot_mark("av_rtc6715");

    g_setting.autoscan.last_source = is_av_in ? SETTING_AUTOSCAN_SOURCE_AV_IN : SETTING_AUTOSCAN_SOURCE_AV_MODULE;
#if defined(HDZBOXPRO) || defined(HDZGOGGLE2)
    // Reached via Auto Detect (entry probe or protocol crossover): record
    // that, so Auto Scan "Last" re-enters Auto Detect instead of pinning the
    // protocol it happened to land on this session.
    if (!is_av_in && g_setting.source.auto_protocol_detect)
        g_setting.autoscan.last_source = SETTING_AUTOSCAN_SOURCE_AUTO_DETECT;
#endif
    ini_putl("autoscan", "last_source", g_setting.autoscan.last_source, SETTING_INI);
    boot_mark("av_settings_saved");

    // audio in&out
#ifdef HDZGOGGLE2
    pthread_t audio_tid;
    bool audio_async = false;
    if (!g_init_done) {
        dvr_audio_route_lock(); // released by the worker when done
        audio_async = pthread_create(&audio_tid, NULL, thread_boot_audio_route, NULL) == 0;
        if (audio_async)
            pthread_detach(audio_tid);
        else
            dvr_audio_route_unlock();
    }
    if (!audio_async)
#endif
    {
        dvr_select_audio_source(g_setting.record.audio_source);
        dvr_enable_line_out(true);
    }

    boot_mark("switch_to_analog_before_sleep");
#ifdef HDZGOGGLE2
    // Not during the initial bring-up: nothing at boot needs this second
    // (hardware-tested); source switches later keep it.
    if (g_init_done)
#endif
        sleep(1);
    system_script(REC_STOP_LIVE);
    boot_mark("switch_to_analog_end");
}

void app_switch_to_hdmi_in() {
    boot_mark("switch_to_hdmi_begin");
    rtc6715.init(0, 0);
#if defined(HDZBOXPRO) || defined(HDZGOGGLE2)
    scan_core_notify_analog_powered_off();
#endif

    Source_HDMI_in();
    IT66121_close();
    sleep(2);
    boot_mark("switch_to_hdmi_after_sleep");

    if (g_hw_stat.hdmiin_vtmg == HDMIIN_VTMG_1080P60 ||
        g_hw_stat.hdmiin_vtmg == HDMIIN_VTMG_1080P50 ||
        g_hw_stat.hdmiin_vtmg == HDMIIN_VTMG_1080Pother)
        lvgl_switch_to_1080p();
    else
        lvgl_switch_to_720p();

    osd_show(true);
    osd_clear();
    lv_timer_handler();

    // update vi conf at HDMI_in_detect()
    // dvr_update_vi_conf((g_hw_stat.hdmiin_vtmg == 1) ? VR_1080P30 : VR_720P60);

    app_state_push(APP_STATE_VIDEO);
    g_source_info.source = SOURCE_HDMI_IN;
    dvr_enable_line_out(false);
    g_setting.autoscan.last_source = SETTING_AUTOSCAN_SOURCE_HDMI_IN;
    ini_putl("autoscan", "last_source", g_setting.autoscan.last_source, SETTING_INI);

    system_script(REC_STOP_LIVE);
}

//////////////////////////////////////////////////////////////////////
// is_default:
//    true = load from g_settings
//    false = user selected from auto scan page
void app_switch_to_hdzero(bool is_default) {
    int ch;

    boot_mark("switch_to_hdzero_begin");

    rtc6715.init(0, 0);
#if defined(HDZBOXPRO) || defined(HDZGOGGLE2)
    scan_core_notify_analog_powered_off();
#endif

    if (is_default) {
        ch = g_setting.scan.channel - 1;
    } else {
        ch = valid_channel_tb[user_select_index];
        g_setting.scan.channel = ch + 1;
        ini_putl("scan", "channel", g_setting.scan.channel, SETTING_INI);
    }

    // With BW=Auto, tag the channel OSD "Detecting..." for the switch sequence
    // -- the bandwidth is still settling (the bw-reacquire watchdog corrects a
    // wrong guess after entry).
    if (g_setting.source.hdzero_bw == SETTING_SOURCES_HDZERO_BW_BOTH)
        osd_detecting_show(true);

    HDZero_open(hdzero_effective_bw());
    boot_mark("hdzero_open");
    ch &= 0x7f;

    LOGI("switch to bw:%d, band:%d, ch:%d, CAM_MODE=%d 4:3=%d", hdzero_effective_bw(), g_setting.source.hdzero_band, g_setting.scan.channel, CAM_MODE, cam_4_3);
    DM6302_SetChannel(g_setting.source.hdzero_band, ch);
    DM5680_clear_vldflg();
    DM5680_req_vldflg();
    boot_mark("hdzero_set_channel");
    osd_fonts_wait(); // boot: the FC OSD fonts may still be loading (osd_init)
    progress_bar.start = 0;

#if defined(HDZGOGGLE) || defined(HDZGOGGLE2)
    switch (CAM_MODE) {
    case VR_720P50:
    case VR_720P60:
    case VR_960x720P60:
    case VR_540P60:
        Display_720P60_50(CAM_MODE, cam_4_3);
        break;

    case VR_540P90:
    case VR_540P90_CROP:
        Display_720P90(CAM_MODE);
        break;

    case VR_1080P30:
        Display_1080P30(CAM_MODE);
        break;
    case VR_1080P24:
        Display_1080P24(CAM_MODE);
        break;
    default:
        perror("switch_to_video CaM_MODE error");
    }

    channel_osd_mode = CHANNEL_SHOWTIME;

    if (CAM_MODE == VR_1080P30 || CAM_MODE == VR_1080P24)
        lvgl_switch_to_1080p();
    else
        lvgl_switch_to_720p();
    osd_fhd(CAM_MODE == VR_1080P30 || CAM_MODE == VR_1080P24);
#elif defined HDZBOXPRO
    Display_HDZ(CAM_MODE, cam_4_3);
    channel_osd_mode = CHANNEL_SHOWTIME;
    lvgl_switch_to_720p();
    LOGI("lvgl_switch_to_720p");
#endif

    image_settings_apply(SETTING_IMAGE_SOURCE_HDZERO);
    osd_clear();
    osd_show(true);
    lv_timer_handler();
    Display_Osd(g_setting.record.osd);

    g_setting.autoscan.last_source = SETTING_AUTOSCAN_SOURCE_HDZERO;
#if defined(HDZBOXPRO) || defined(HDZGOGGLE2)
    // Reached via Auto Detect: record that (see app_switch_to_analog).
    if (g_setting.source.auto_protocol_detect)
        g_setting.autoscan.last_source = SETTING_AUTOSCAN_SOURCE_AUTO_DETECT;
#endif
    ini_putl("autoscan", "last_source", g_setting.autoscan.last_source, SETTING_INI);

    dvr_update_vi_conf(CAM_MODE);
    system_script(REC_STOP_LIVE);

    osd_detecting_show(false); // restore the normal channel tag
    boot_mark("switch_to_hdzero_end");
}

void hdzero_switch_channel(int channel) {
    channel &= 0x7f;

    LOGI("hdzero_switch_channel to bw:%d, band:%d, ch:%d, CAM_MODE=%d 4:3=%d", g_setting.source.hdzero_bw, g_setting.source.hdzero_band, channel, CAM_MODE, cam_4_3);
    DM6302_SetChannel(g_setting.source.hdzero_band, channel);
    DM5680_clear_vldflg();
    DM5680_req_vldflg();
    channel_osd_mode = CHANNEL_SHOWTIME;

    osd_clear();
    osd_show(true);
    lv_timer_handler();

    system_script(REC_STOP_LIVE);
}
