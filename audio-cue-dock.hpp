#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Installs the Monitor buttons under each Studio Mode viewer and hooks
 * the relevant OBS frontend events. Call once from obs_module_load. */
void audio_cue_ui_init(void);

#ifdef __cplusplus
}
#endif
