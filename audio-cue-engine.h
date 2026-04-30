#pragma once

#include <obs-module.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Two independent cue targets, one for each Studio Mode pane. */
typedef enum {
	AUDIO_CUE_TARGET_PREVIEW = 0,
	AUDIO_CUE_TARGET_PROGRAM = 1,
} audio_cue_target;

/* Engine lifecycle */
void audio_cue_engine_init(void);
void audio_cue_engine_shutdown(void);

/* Checks for an orphan crash-checkpoint at startup; if one exists, the
 * previous session ended without releasing its cues and the user's
 * sources are still in cue-modified state. Restore each source from
 * the checkpoint, then delete it. Must be called AFTER OBS has loaded
 * the scene collection (i.e. from FINISHED_LOADING). */
void audio_cue_load_checkpoint_if_present(void);

/* Force-release everything immediately. For panic recovery. */
void audio_cue_panic_restore(void);

/* Cue state per target.
 *
 * Preview cue (when active): every audio source in the current preview
 * scene is forced to (unmuted + MONITOR_ONLY) and inc_active'd so it
 * actually emits audio. Routes to monitor only — stream is unaffected.
 *
 * Program cue (when active): every audio source in the current program
 * scene is forced to MONITOR_AND_OUTPUT. Stream still receives normal
 * program audio; user also hears it on headphones. */
bool audio_cue_is_active(audio_cue_target target);
void audio_cue_set_active(audio_cue_target target, bool active);
void audio_cue_toggle(audio_cue_target target);

/* Called by the UI debounce after any preview/program scene change.
 * Pass the current scene names (NULL allowed). The engine compares
 * against its cached prior state to detect a transition (new program
 * == old preview): when that happens with the preview cue active, the
 * cue auto-switches to the program target so the user keeps hearing
 * the same scene as it goes live. */
void audio_cue_on_scene_state(const char *program_scene_name,
			      const char *preview_scene_name);

#ifdef __cplusplus
}
#endif
