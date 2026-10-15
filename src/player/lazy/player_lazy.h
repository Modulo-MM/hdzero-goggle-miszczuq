#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// The DVR player (src/player) is a separate library, libhdzplayer.so, next to
// the app binary, loaded on first use (see player_lazy.c). Call once boot is
// done: loads it in the background a little later, so the first playback
// does not wait for it.
void player_preload_start(void);

// Load it now, in the calling thread.
void player_load_now(void);

#ifdef __cplusplus
}
#endif
