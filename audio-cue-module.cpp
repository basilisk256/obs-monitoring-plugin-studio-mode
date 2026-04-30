#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/platform.h>
#include <util/bmem.h>

#include "audio-cue-engine.h"
#include "audio-cue-dock.hpp"
#include "version.h"

OBS_DECLARE_MODULE()
OBS_MODULE_AUTHOR("basilisk256")
OBS_MODULE_USE_DEFAULT_LOCALE("obs-audio-cue", "en-US")

static obs_hotkey_id g_hk_preview = OBS_INVALID_HOTKEY_ID;
static obs_hotkey_id g_hk_program = OBS_INVALID_HOTKEY_ID;
static obs_hotkey_id g_hk_panic = OBS_INVALID_HOTKEY_ID;

static void on_preview_hotkey(void *, obs_hotkey_id, obs_hotkey_t *,
			      bool pressed)
{
	if (pressed)
		audio_cue_toggle(AUDIO_CUE_TARGET_PREVIEW);
}

static void on_program_hotkey(void *, obs_hotkey_id, obs_hotkey_t *,
			      bool pressed)
{
	if (pressed)
		audio_cue_toggle(AUDIO_CUE_TARGET_PROGRAM);
}

static void on_panic_hotkey(void *, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (pressed)
		audio_cue_panic_restore();
}

static void save_hotkeys(void)
{
	char *dir = obs_module_config_path("");
	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}
	char *path = obs_module_config_path("hotkeys.json");
	if (!path)
		return;

	obs_data_t *d = obs_data_create();

	if (g_hk_preview != OBS_INVALID_HOTKEY_ID) {
		obs_data_array_t *arr = obs_hotkey_save(g_hk_preview);
		if (arr) {
			obs_data_set_array(d, "preview", arr);
			obs_data_array_release(arr);
		}
	}
	if (g_hk_program != OBS_INVALID_HOTKEY_ID) {
		obs_data_array_t *arr = obs_hotkey_save(g_hk_program);
		if (arr) {
			obs_data_set_array(d, "program", arr);
			obs_data_array_release(arr);
		}
	}
	if (g_hk_panic != OBS_INVALID_HOTKEY_ID) {
		obs_data_array_t *arr = obs_hotkey_save(g_hk_panic);
		if (arr) {
			obs_data_set_array(d, "panic", arr);
			obs_data_array_release(arr);
		}
	}

	obs_data_save_json(d, path);
	obs_data_release(d);
	bfree(path);
}

static void load_hotkeys(void)
{
	char *path = obs_module_config_path("hotkeys.json");
	if (!path)
		return;
	obs_data_t *d = obs_data_create_from_json_file(path);
	bfree(path);
	if (!d)
		return;

	if (g_hk_preview != OBS_INVALID_HOTKEY_ID) {
		obs_data_array_t *arr = obs_data_get_array(d, "preview");
		if (arr) {
			obs_hotkey_load(g_hk_preview, arr);
			obs_data_array_release(arr);
		}
	}
	if (g_hk_program != OBS_INVALID_HOTKEY_ID) {
		obs_data_array_t *arr = obs_data_get_array(d, "program");
		if (arr) {
			obs_hotkey_load(g_hk_program, arr);
			obs_data_array_release(arr);
		}
	}
	if (g_hk_panic != OBS_INVALID_HOTKEY_ID) {
		obs_data_array_t *arr = obs_data_get_array(d, "panic");
		if (arr) {
			obs_hotkey_load(g_hk_panic, arr);
			obs_data_array_release(arr);
		}
	}
	obs_data_release(d);
}

static void on_frontend_event_module(enum obs_frontend_event ev, void *)
{
	switch (ev) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		load_hotkeys();
		/* Sources are now loaded — restore from any orphan checkpoint
		 * left by a previous crashed session before the user touches
		 * anything. */
		audio_cue_load_checkpoint_if_present();
		break;
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING:
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP:
		/* About to switch/save scene collection — release everything
		 * so OBS persists the user's untouched state, not ours. */
		audio_cue_panic_restore();
		break;
	case OBS_FRONTEND_EVENT_EXIT:
		save_hotkeys();
		audio_cue_engine_shutdown();
		break;
	default:
		break;
	}
}

bool obs_module_load(void)
{
	audio_cue_engine_init();

	g_hk_preview = obs_hotkey_register_frontend(
		"audio_cue.toggle_preview",
		"Toggle Audio Cue: Preview pane", on_preview_hotkey, nullptr);
	g_hk_program = obs_hotkey_register_frontend(
		"audio_cue.toggle_program",
		"Toggle Audio Cue: Program pane", on_program_hotkey, nullptr);
	g_hk_panic = obs_hotkey_register_frontend(
		"audio_cue.panic_restore",
		"Audio Cue: PANIC restore (force release all)",
		on_panic_hotkey, nullptr);

	audio_cue_ui_init();
	obs_frontend_add_event_callback(on_frontend_event_module, nullptr);

	blog(LOG_INFO, "[Audio Cue] Plugin loaded (version %s)",
	     PROJECT_VERSION);
	return true;
}

void obs_module_unload(void)
{
	audio_cue_engine_shutdown();
}
