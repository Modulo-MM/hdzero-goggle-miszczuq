#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Audio codec mixer controls.
//
// The app used to run `amixer cset name='<name>' <values>` per control and
// audio_sel.sh (a shell running 2-10 amixer processes) per routing change:
// ~1-1.4 s for the routing of one source switch. With native_audio
// (perf_opt) the same controls are written in-process through the ALSA
// control ioctls on card 0, amixer's default card; with it off, or if a
// control can't be set that way, the old command runs.

// amixer cset semantics: `values` is one value for every channel, or a
// comma-separated value per channel; integers are clamped to the control's
// range, an enumerated value is an item name or index.
void mixer_cset(const char *name, const char *values);
void mixer_set(const char *name, int value);           // every channel
void mixer_set2(const char *name, int left, int right); // "left,right"

// One audio_sel.sh command ("in_mic1", "out_on", "out_linein_off", ...).
void audio_sel(const char *cmd);

#ifdef __cplusplus
}
#endif
