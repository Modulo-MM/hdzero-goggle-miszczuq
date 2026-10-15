#include "player_lazy.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <log/log.h>

#include "media.h"
#include "platform/paths.h"
#include "wav_test.h"

// The player code (src/player) and the ~40 vendor media libraries it needs
// (MPP, cedarx, ...) used to be linked into the app, so the dynamic loader
// mapped and relocated all of them before main() on every boot, ~1.7 s on the
// G2. Only DVR playback (and the G1's audio test) uses them, so the player is
// built as libhdzplayer.so and loaded here instead: by player_preload_start()
// shortly after boot, or by the first call into it, whichever comes first.
// The app exports the few symbols the player uses (src/player/lazy/exports.list).
// <SD>/bootopt/disable_lazy_player: main() loads it right at startup
// (player_load_now()), close to the old boot-time loading, for comparison.

#define PLAYER_PRELOAD_DELAY_S 10

static struct {
    media_t *(*media_instantiate)(char *filename, notify_cb_t notify);
    void (*media_exit)(media_t *media);
    void (*media_control)(media_t *media, player_cmd_t *cmd);
    int (*wav_test_play)(const char *path);
    int (*wav_test_record)(const char *path, int seconds);
} player;

static pthread_once_t player_once = PTHREAD_ONCE_INIT;

static double uptime_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

// One line in BOOT_TIMING.log when that file exists (boot timing enabled).
static void player_load_note(double start, const char *result) {
    char path[256];
    snprintf(path, sizeof(path), "%s/BOOT_TIMING.log", path_extsd());
    if (access(path, F_OK) != 0)
        return;
    FILE *fp = fopen(path, "a");
    if (!fp)
        return;
    fprintf(fp, "PLAYER LOAD: at %.3f s since kernel start, took %.3f s: %s\n", start, uptime_s() - start, result);
    fclose(fp);
}

static void player_load(void) {
    char path[256];
    double start = uptime_s();
    snprintf(path, sizeof(path), "%s/app/libhdzplayer.so", path_app());
    // RTLD_GLOBAL: the vendor libraries' symbols stay visible to anything
    // they load themselves, as when they were loaded with the app.
    void *lib = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
    if (!lib) {
        const char *err = dlerror();
        LOGE("player: cannot load %s: %s", path, err);
        player_load_note(start, err ? err : "dlopen failed");
        return;
    }
    player.media_instantiate = dlsym(lib, "media_instantiate");
    player.media_exit = dlsym(lib, "media_exit");
    player.media_control = dlsym(lib, "media_control");
    player.wav_test_play = dlsym(lib, "wav_test_play");
    player.wav_test_record = dlsym(lib, "wav_test_record");
    LOGI("player: %s loaded", path);
    player_load_note(start, player.media_instantiate ? "ok" : "symbols missing");
}

static bool player_ready(void) {
    pthread_once(&player_once, player_load);
    return player.media_instantiate && player.media_exit && player.media_control;
}

static void *player_preload_thread(void *arg) {
    (void)arg;
    // lowest priority: background work only (per-thread nice on Linux)
    setpriority(PRIO_PROCESS, syscall(SYS_gettid), 19);
    sleep(PLAYER_PRELOAD_DELAY_S);
    pthread_once(&player_once, player_load);
    return NULL;
}

void player_load_now(void) {
    pthread_once(&player_once, player_load);
}

void player_preload_start(void) {
    pthread_t tid;
    if (pthread_create(&tid, NULL, player_preload_thread, NULL) == 0)
        pthread_detach(tid);
}

media_t *media_instantiate(char *filename, notify_cb_t notify) {
    if (!player_ready())
        return NULL;
    return player.media_instantiate(filename, notify);
}

void media_exit(media_t *media) {
    if (player_ready())
        player.media_exit(media);
}

void media_control(media_t *media, player_cmd_t *cmd) {
    if (player_ready())
        player.media_control(media, cmd);
}

int wav_test_play(const char *path) {
    pthread_once(&player_once, player_load);
    return player.wav_test_play ? player.wav_test_play(path) : -1;
}

int wav_test_record(const char *path, int seconds) {
    pthread_once(&player_once, player_load);
    return player.wav_test_record ? player.wav_test_record(path, seconds) : -1;
}
