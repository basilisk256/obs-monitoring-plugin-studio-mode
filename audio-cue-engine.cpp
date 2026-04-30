#include "audio-cue-engine.h"

#include <obs-frontend-api.h>
#include <util/platform.h>
#include <util/bmem.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

/*
 * Audio Cue engine — broadcast-style PFL ("pre-fade listen").
 *
 * When a cue target is active, the user should hear ONLY that target's
 * audio in their monitoring device. Stream output is left unchanged.
 *
 * Implementation per target:
 *   - Capture every audio source's prior (muted, monitoring) state on
 *     activation; restore it on deactivation.
 *   - For each audio source:
 *       * If it belongs to the cue target's scene -> route to user.
 *           - PREVIEW cue, source preview-only:
 *             unmuted + MONITOR_ONLY + obs_source_inc_active (so the
 *             source actually emits audio; preview sources are normally
 *             inactive in studio mode).
 *           - PREVIEW cue, source also in program scene:
 *             unmuted + MONITOR_AND_OUTPUT (audible to user; stream
 *             still receives it because it's a program source).
 *           - PROGRAM cue:
 *             MONITOR_AND_OUTPUT (audible to user; stream unaffected).
 *       * Otherwise -> route OFF user's headphones.
 *           - monitoring = NONE. Mute and active state untouched, so
 *             stream/recording continues unaffected.
 *
 * A 200ms re-assert thread defends against scene_mute.lua and other
 * plugins that may toggle muted/monitoring while we hold the cue.
 */

namespace {

struct SavedSource {
	bool muted;
	enum obs_monitoring_type monitoring;
	bool did_inc_active;
};

struct TargetState {
	std::atomic<bool> active{false};
	std::unordered_map<std::string, SavedSource> captured;
};

TargetState g_targets[2];
std::mutex g_mutex;

/* Cached current scene names — used to detect a transition (new program
 * == old preview) so the cue can auto-switch from preview to program. */
std::string g_cached_program;
std::string g_cached_preview;

constexpr const char *CHECKPOINT_FILE = "active-checkpoint.json";

std::atomic<bool> g_should_stop{false};
std::thread g_tick_thread;
std::mutex g_thread_lifecycle;
constexpr int TICK_INTERVAL_MS = 200;

/* ── Source enumeration ──────────────────────────────────── */

bool source_is_audio(obs_source_t *src)
{
	if (!src)
		return false;
	/* Scenes and groups are technically OBS_SOURCE_AUDIO because they
	 * aggregate child audio. Touching their muted/monitoring/active
	 * state propagates to all children and corrupts the program output
	 * (the visible symptom: program canvas goes black). Treat them as
	 * non-audio and only operate on leaf audio sources. */
	if (obs_source_is_scene(src) || obs_source_is_group(src))
		return false;
	uint32_t flags = obs_source_get_output_flags(src);
	return (flags & OBS_SOURCE_AUDIO) != 0;
}

bool enum_all_audio_cb(void *param, obs_source_t *src)
{
	auto *out = static_cast<std::unordered_set<std::string> *>(param);
	if (src && source_is_audio(src)) {
		const char *name = obs_source_get_name(src);
		if (name && *name)
			out->insert(name);
	}
	return true;
}

std::unordered_set<std::string> all_audio_source_names()
{
	std::unordered_set<std::string> out;
	obs_enum_sources(enum_all_audio_cb, &out);
	return out;
}

struct SceneEnumCtx {
	std::unordered_set<std::string> *out;
};

bool enum_scene_item_cb(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	auto *ctx = static_cast<SceneEnumCtx *>(param);
	obs_source_t *src = obs_sceneitem_get_source(item);
	if (!src)
		return true;
	if (source_is_audio(src)) {
		const char *name = obs_source_get_name(src);
		if (name && *name)
			ctx->out->insert(name);
	}
	if (obs_source_is_scene(src) || obs_source_is_group(src)) {
		obs_scene_t *child = obs_scene_from_source(src);
		if (!child)
			child = obs_group_from_source(src);
		if (child)
			obs_scene_enum_items(child, enum_scene_item_cb, ctx);
	}
	return true;
}

/* Forward declarations — defined below, called from sync/release. */
void delete_checkpoint_file();
void write_or_delete_checkpoint_locked();

std::unordered_set<std::string>
audio_sources_in_scene_source(obs_source_t *scene_src)
{
	std::unordered_set<std::string> out;
	if (!scene_src)
		return out;
	obs_scene_t *scene = obs_scene_from_source(scene_src);
	if (!scene)
		return out;
	SceneEnumCtx ctx{&out};
	obs_scene_enum_items(scene, enum_scene_item_cb, &ctx);
	return out;
}

obs_source_t *get_target_scene(audio_cue_target target)
{
	if (target == AUDIO_CUE_TARGET_PROGRAM)
		return obs_frontend_get_current_scene();
	if (obs_frontend_preview_program_mode_active())
		return obs_frontend_get_current_preview_scene();
	return obs_frontend_get_current_scene();
}

/* ── Apply / restore (caller holds g_mutex) ─────────────── */

void capture_if_new_locked(TargetState &t, const std::string &name,
			   obs_source_t *src)
{
	if (t.captured.find(name) != t.captured.end())
		return;
	SavedSource s;
	s.muted = obs_source_muted(src);
	s.monitoring = obs_source_get_monitoring_type(src);
	s.did_inc_active = false;
	t.captured[name] = s;
}

void apply_routing_locked(audio_cue_target target, const std::string &name,
			  obs_source_t *src, bool in_target,
			  bool in_program_too)
{
	auto &t = g_targets[target];
	capture_if_new_locked(t, name, src);

	if (!in_target) {
		/* Silence this source from monitoring. Leave mute/active
		 * untouched so stream/recording is unaffected. */
		if (obs_source_get_monitoring_type(src) !=
		    OBS_MONITORING_TYPE_NONE)
			obs_source_set_monitoring_type(
				src, OBS_MONITORING_TYPE_NONE);
		return;
	}

	/* In-target source: route to user's monitoring.
	 *
	 * We never call obs_source_inc_active here. Activation is a SCENE
	 * concern — when the user actually cuts, OBS activates the source
	 * properly (firing its activate callback so e.g. media restarts).
	 * If we forcibly activated for monitoring, the cut wouldn't fire a
	 * fresh activate and play_on_activate / restart-on-activate media
	 * sources wouldn't restart. Trade-off: preview-only sources that
	 * aren't active yet (e.g. a media file waiting in preview) are
	 * inaudible during cue. They become audible the moment they're cut
	 * to program. This matches the broadcast-console PFL model. */
	if (target == AUDIO_CUE_TARGET_PROGRAM || in_program_too) {
		/* Source is on stream — MONITOR_AND_OUTPUT so stream still
		 * receives it AND user hears it. */
		if (obs_source_get_monitoring_type(src) !=
		    OBS_MONITORING_TYPE_MONITOR_AND_OUTPUT)
			obs_source_set_monitoring_type(
				src,
				OBS_MONITORING_TYPE_MONITOR_AND_OUTPUT);
		if (obs_source_muted(src))
			obs_source_set_muted(src, false);
	} else {
		/* Preview-only source: needs inc_active to actually emit
		 * audio (preview-pane sources are inactive by default). On
		 * transition (preview cue → program cue) we explicitly call
		 * obs_source_media_restart for media sources with
		 * restart_on_activate, so the cut still triggers a fresh
		 * playback even though our prior inc_active prevented OBS's
		 * own activate signal from firing. */
		if (obs_source_muted(src))
			obs_source_set_muted(src, false);
		if (obs_source_get_monitoring_type(src) !=
		    OBS_MONITORING_TYPE_MONITOR_ONLY)
			obs_source_set_monitoring_type(
				src, OBS_MONITORING_TYPE_MONITOR_ONLY);
		if (!t.captured[name].did_inc_active) {
			obs_source_inc_active(src);
			t.captured[name].did_inc_active = true;
		}
	}
}

void sync_target_to_scene_locked(audio_cue_target target)
{
	auto &t = g_targets[target];
	if (!t.active.load())
		return;

	/* Enumerate scene sources for the cue target. */
	obs_source_t *target_scene = get_target_scene(target);
	auto target_sources = audio_sources_in_scene_source(target_scene);
	if (target_scene)
		obs_source_release(target_scene);

	/* For preview cue we also need the program scene's audio source set
	 * to detect "this source is already on stream" and avoid muting it. */
	std::unordered_set<std::string> program_sources;
	if (target == AUDIO_CUE_TARGET_PREVIEW) {
		obs_source_t *prog = obs_frontend_get_current_scene();
		program_sources = audio_sources_in_scene_source(prog);
		if (prog)
			obs_source_release(prog);
	}

	auto all_sources = all_audio_source_names();

	for (const auto &name : all_sources) {
		obs_source_t *src = obs_get_source_by_name(name.c_str());
		if (!src)
			continue;
		bool in_target = target_sources.count(name) > 0;
		bool in_program_too =
			(target == AUDIO_CUE_TARGET_PREVIEW) &&
			program_sources.count(name) > 0;
		apply_routing_locked(target, name, src, in_target,
				     in_program_too);
		obs_source_release(src);
	}

	/* Drop captures for sources that no longer exist (deleted). */
	for (auto it = t.captured.begin(); it != t.captured.end();) {
		if (!all_sources.count(it->first))
			it = t.captured.erase(it);
		else
			++it;
	}

	write_or_delete_checkpoint_locked();
}

void release_all_locked(audio_cue_target target)
{
	auto &t = g_targets[target];
	for (auto &kv : t.captured) {
		obs_source_t *src = obs_get_source_by_name(kv.first.c_str());
		if (!src)
			continue;
		obs_source_set_monitoring_type(src, kv.second.monitoring);
		obs_source_set_muted(src, kv.second.muted);
		if (kv.second.did_inc_active)
			obs_source_dec_active(src);
		obs_source_release(src);
	}
	t.captured.clear();
	write_or_delete_checkpoint_locked();
}

/* Force a deactivate→activate cycle on each source so its type-specific
 * activate handler runs as if the source were freshly activated. This
 * is the universal equivalent of "what happens when you click the scene
 * to make it live" — works for any source type (media, browser, video
 * capture, future plugins) without us having to know each one.
 *
 * Mechanism: dec_active until refcount hits 0, wait for a video tick
 * (so libobs observes the 0 transition and runs the source's
 * info.deactivate), then inc back to fire info.activate on the next
 * tick. Without the wait, both transitions happen before any tick and
 * libobs sees no net change → no handlers fire.
 *
 * The inc is scheduled on a detached worker that sleeps 120ms (≈ 7
 * frames at 60fps). During that window the source is fully inactive
 * → the source's video output may briefly freeze/blank. That's the
 * one-frame cost of getting a clean cut-style restart. */
void force_reactivate_promoted_locked(
	const std::vector<std::string> &previously_held_active)
{
	if (previously_held_active.empty())
		return;
	obs_source_t *prog_scene = obs_frontend_get_current_scene();
	auto in_program = audio_sources_in_scene_source(prog_scene);
	if (prog_scene)
		obs_source_release(prog_scene);

	for (const auto &name : previously_held_active) {
		if (!in_program.count(name))
			continue;
		obs_source_t *src = obs_get_source_by_name(name.c_str());
		if (!src)
			continue;

		int decs = 0;
		while (obs_source_active(src) && decs < 16) {
			obs_source_dec_active(src);
			decs++;
		}
		obs_source_release(src);

		if (decs > 0) {
			std::thread([name, decs]() {
				std::this_thread::sleep_for(
					std::chrono::milliseconds(120));
				obs_source_t *s = obs_get_source_by_name(
					name.c_str());
				if (!s)
					return;
				for (int i = 0; i < decs; i++)
					obs_source_inc_active(s);
				obs_source_release(s);
			}).detach();
		}

		blog(LOG_INFO,
		     "[Audio Cue] Cut-restart: cycling '%s' (decs=%d, inc "
		     "deferred 120ms)",
		     name.c_str(), decs);
	}
}

/* ── Checkpoint persistence (caller holds g_mutex) ──────── */

void delete_checkpoint_file()
{
	char *path = obs_module_config_path(CHECKPOINT_FILE);
	if (!path)
		return;
	os_unlink(path);
	bfree(path);
}

void write_or_delete_checkpoint_locked()
{
	/* Combine entries from both targets (mutual exclusion means at most
	 * one is non-empty in practice, but be defensive). */
	std::unordered_map<std::string, SavedSource> combined;
	for (int i = 0; i < 2; ++i)
		for (auto &kv : g_targets[i].captured)
			combined[kv.first] = kv.second;

	if (combined.empty()) {
		delete_checkpoint_file();
		return;
	}

	char *dir = obs_module_config_path("");
	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}
	char *path = obs_module_config_path(CHECKPOINT_FILE);
	if (!path)
		return;

	obs_data_t *d = obs_data_create();
	obs_data_array_t *arr = obs_data_array_create();
	for (auto &kv : combined) {
		obs_data_t *item = obs_data_create();
		obs_data_set_string(item, "name", kv.first.c_str());
		obs_data_set_bool(item, "muted", kv.second.muted);
		obs_data_set_int(item, "monitoring", (int)kv.second.monitoring);
		obs_data_array_push_back(arr, item);
		obs_data_release(item);
	}
	obs_data_set_array(d, "captured", arr);
	obs_data_array_release(arr);
	obs_data_save_json(d, path);
	obs_data_release(d);
	bfree(path);
}

/* ── Re-assert thread ────────────────────────────────────── */

void tick_thread_fn()
{
	while (!g_should_stop.load()) {
		{
			std::lock_guard<std::mutex> lk(g_mutex);
			for (int i = 0; i < 2; ++i) {
				if (g_targets[i].active.load())
					sync_target_to_scene_locked(
						(audio_cue_target)i);
			}
		}
		for (int i = 0;
		     i < TICK_INTERVAL_MS / 20 && !g_should_stop.load(); ++i)
			std::this_thread::sleep_for(
				std::chrono::milliseconds(20));
	}
}

void start_tick_thread()
{
	std::lock_guard<std::mutex> lk(g_thread_lifecycle);
	if (g_tick_thread.joinable())
		return;
	g_should_stop.store(false);
	g_tick_thread = std::thread(tick_thread_fn);
}

void stop_tick_thread_if_idle()
{
	if (g_targets[0].active.load() || g_targets[1].active.load())
		return;
	std::lock_guard<std::mutex> lk(g_thread_lifecycle);
	if (!g_tick_thread.joinable())
		return;
	g_should_stop.store(true);
	g_tick_thread.join();
}

void stop_tick_thread_force()
{
	std::lock_guard<std::mutex> lk(g_thread_lifecycle);
	if (!g_tick_thread.joinable())
		return;
	g_should_stop.store(true);
	g_tick_thread.join();
}

} // namespace

extern "C" {

void audio_cue_engine_init(void) {}

void audio_cue_engine_shutdown(void)
{
	{
		std::lock_guard<std::mutex> lk(g_mutex);
		for (int i = 0; i < 2; ++i) {
			if (g_targets[i].active.load()) {
				release_all_locked((audio_cue_target)i);
				g_targets[i].active.store(false);
			}
		}
	}
	stop_tick_thread_force();
}

bool audio_cue_is_active(audio_cue_target target)
{
	if ((int)target < 0 || (int)target > 1)
		return false;
	return g_targets[target].active.load();
}

void audio_cue_set_active(audio_cue_target target, bool active)
{
	if ((int)target < 0 || (int)target > 1)
		return;
	{
		std::lock_guard<std::mutex> lk(g_mutex);
		if (g_targets[target].active.load() == active)
			return;
		if (active) {
			/* Seed cached scene names so the next transition can
			 * be detected (compares new program against the
			 * preview-at-activation). */
			obs_source_t *p = obs_frontend_get_current_scene();
			if (p) {
				const char *n = obs_source_get_name(p);
				g_cached_program = n ? n : "";
				obs_source_release(p);
			}
			obs_source_t *v =
				obs_frontend_get_current_preview_scene();
			if (v) {
				const char *n = obs_source_get_name(v);
				g_cached_preview = n ? n : "";
				obs_source_release(v);
			}

			/* Mutual exclusion: only one cue at a time. */
			audio_cue_target other =
				(target == AUDIO_CUE_TARGET_PREVIEW)
					? AUDIO_CUE_TARGET_PROGRAM
					: AUDIO_CUE_TARGET_PREVIEW;
			if (g_targets[other].active.load()) {
				release_all_locked(other);
				g_targets[other].active.store(false);
			}
			g_targets[target].active.store(true);
			sync_target_to_scene_locked(target);
		} else {
			release_all_locked(target);
			g_targets[target].active.store(false);
		}
	}
	if (active)
		start_tick_thread();
	else
		stop_tick_thread_if_idle();
}

void audio_cue_toggle(audio_cue_target target)
{
	audio_cue_set_active(target, !audio_cue_is_active(target));
}

void audio_cue_load_checkpoint_if_present(void)
{
	char *path = obs_module_config_path(CHECKPOINT_FILE);
	if (!path)
		return;
	obs_data_t *d = obs_data_create_from_json_file(path);
	if (!d) {
		bfree(path);
		return;
	}

	blog(LOG_WARNING,
	     "[Audio Cue] Found orphan checkpoint — previous session ended "
	     "without releasing cues. Restoring source state.");

	obs_data_array_t *arr = obs_data_get_array(d, "captured");
	if (arr) {
		size_t n = obs_data_array_count(arr);
		for (size_t i = 0; i < n; ++i) {
			obs_data_t *item = obs_data_array_item(arr, i);
			if (!item)
				continue;
			const char *name = obs_data_get_string(item, "name");
			if (name && *name) {
				obs_source_t *src =
					obs_get_source_by_name(name);
				if (src) {
					bool muted = obs_data_get_bool(
						item, "muted");
					int mon = (int)obs_data_get_int(
						item, "monitoring");
					obs_source_set_monitoring_type(
						src,
						(enum obs_monitoring_type)mon);
					obs_source_set_muted(src, muted);
					obs_source_release(src);
					blog(LOG_INFO,
					     "[Audio Cue] Restored '%s' "
					     "(muted=%d monitoring=%d)",
					     name, (int)muted, mon);
				} else {
					blog(LOG_WARNING,
					     "[Audio Cue] Checkpoint source "
					     "'%s' not found in current scene "
					     "collection — skipping",
					     name);
				}
			}
			obs_data_release(item);
		}
		obs_data_array_release(arr);
	}
	obs_data_release(d);

	os_unlink(path);
	bfree(path);
}

void audio_cue_panic_restore(void)
{
	{
		std::lock_guard<std::mutex> lk(g_mutex);
		for (int i = 0; i < 2; ++i) {
			if (g_targets[i].active.load() ||
			    !g_targets[i].captured.empty()) {
				release_all_locked((audio_cue_target)i);
				g_targets[i].active.store(false);
			}
		}
	}
	stop_tick_thread_force();
	blog(LOG_INFO, "[Audio Cue] PANIC RESTORE: all cues released");
}

void audio_cue_on_scene_state(const char *program_scene_name,
			      const char *preview_scene_name)
{
	std::lock_guard<std::mutex> lk(g_mutex);
	std::string new_program = program_scene_name ? program_scene_name : "";
	std::string new_preview = preview_scene_name ? preview_scene_name : "";

	/* Detect a transition: program changed AND new program is what
	 * preview *was* before this round of changes. Means the user just
	 * promoted preview → program. If a preview cue is active, switch
	 * it to program so the user keeps hearing the same scene as it
	 * goes live. */
	bool was_transition = !g_cached_program.empty() &&
			      new_program != g_cached_program &&
			      !g_cached_preview.empty() &&
			      new_program == g_cached_preview;

	if (was_transition &&
	    g_targets[AUDIO_CUE_TARGET_PREVIEW].active.load()) {
		/* Switch cue: release preview, activate program on the same
		 * scene. Brief routing flicker as state is re-applied.
		 *
		 * Snapshot which sources we were holding inc_active'd before
		 * releasing — those are the ones whose activate signal got
		 * suppressed by our prior ref. After re-syncing for program,
		 * manually trigger each source type's activate behavior
		 * (media restart, browser refresh, etc.) so the cut feels
		 * like a normal scene activation. */
		std::vector<std::string> held_active;
		for (auto &kv :
		     g_targets[AUDIO_CUE_TARGET_PREVIEW].captured) {
			if (kv.second.did_inc_active)
				held_active.push_back(kv.first);
		}

		release_all_locked(AUDIO_CUE_TARGET_PREVIEW);
		g_targets[AUDIO_CUE_TARGET_PREVIEW].active.store(false);
		g_targets[AUDIO_CUE_TARGET_PROGRAM].active.store(true);
		sync_target_to_scene_locked(AUDIO_CUE_TARGET_PROGRAM);
		force_reactivate_promoted_locked(held_active);
	} else {
		if (g_targets[AUDIO_CUE_TARGET_PROGRAM].active.load())
			sync_target_to_scene_locked(AUDIO_CUE_TARGET_PROGRAM);
		if (g_targets[AUDIO_CUE_TARGET_PREVIEW].active.load())
			sync_target_to_scene_locked(AUDIO_CUE_TARGET_PREVIEW);
	}

	g_cached_program = new_program;
	g_cached_preview = new_preview;
}

} // extern "C"
