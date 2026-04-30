#include "audio-cue-dock.hpp"
#include "audio-cue-engine.h"

#include <obs-frontend-api.h>

#include <QHBoxLayout>
#include <QLabel>
#include <QMainWindow>
#include <QObject>
#include <QPointer>
#include <QPushButton>
#include <QString>
#include <QTimer>
#include <QVariant>
#include <QVBoxLayout>
#include <QWidget>

/*
 * Inline UI for the Audio Cue plugin.
 *
 * Inserts a "Monitor" QPushButton below the Preview viewer
 * (`previewContainer`) and below the Program viewer (`programWidget`,
 * which OBS only creates while Studio Mode is on). Each button toggles
 * its target's cue in the engine.
 *
 * `previewContainer` has objectName so we find it by name.
 * `programWidget` does NOT have objectName, so we walk `previewLayout`
 * (the QHBoxLayout in canvasEditor) and look for a child widget whose
 * QVBoxLayout contains a QLabel with class="label-preview-title", which
 * uniquely identifies it.
 */

namespace {

QPointer<QPushButton> g_preview_btn;
QPointer<QPushButton> g_program_btn;
QPointer<QTimer> g_poll_timer;
QPointer<QTimer> g_scene_change_timer;

QString current_program_scene_name()
{
	obs_source_t *s = obs_frontend_get_current_scene();
	if (!s)
		return {};
	const char *n = obs_source_get_name(s);
	QString out = n ? QString::fromUtf8(n) : QString{};
	obs_source_release(s);
	return out;
}

QString current_preview_scene_name()
{
	obs_source_t *s = obs_frontend_get_current_preview_scene();
	if (!s)
		return {};
	const char *n = obs_source_get_name(s);
	QString out = n ? QString::fromUtf8(n) : QString{};
	obs_source_release(s);
	return out;
}

void on_scene_state_debounce()
{
	QByteArray p = current_program_scene_name().toUtf8();
	QByteArray v = current_preview_scene_name().toUtf8();
	audio_cue_on_scene_state(p.constData(), v.constData());
}

void schedule_scene_state_update()
{
	if (!g_scene_change_timer) {
		QMainWindow *win =
			static_cast<QMainWindow *>(obs_frontend_get_main_window());
		g_scene_change_timer = new QTimer(win);
		g_scene_change_timer->setSingleShot(true);
		QObject::connect(g_scene_change_timer, &QTimer::timeout,
				 on_scene_state_debounce);
	}
	/* Restart so multiple events within 50ms collapse into one snapshot
	 * (necessary because PREVIEW_SCENE_CHANGED can fire before
	 * SCENE_CHANGED in swap-scenes mode). */
	g_scene_change_timer->start(50);
}

QMainWindow *main_window()
{
	return static_cast<QMainWindow *>(obs_frontend_get_main_window());
}

void apply_button_style(QPushButton *btn, bool active, const char *baseLabel)
{
	if (!btn)
		return;
	if (active) {
		btn->setText(QString::fromUtf8("\xF0\x9F\x94\x8A  ") +
			     QObject::tr(baseLabel) +
			     QString::fromUtf8(": ON"));
		btn->setStyleSheet(
			"QPushButton {"
			"  background-color: #c64545;"
			"  color: white;"
			"  border: none;"
			"  border-radius: 3px;"
			"  padding: 4px 8px;"
			"}"
			"QPushButton:hover { background-color: #d65555; }");
	} else {
		btn->setText(QString::fromUtf8("\xF0\x9F\x94\x87  ") +
			     QObject::tr(baseLabel));
		btn->setStyleSheet(
			"QPushButton {"
			"  background-color: #404040;"
			"  color: #ddd;"
			"  border: none;"
			"  border-radius: 3px;"
			"  padding: 4px 8px;"
			"}"
			"QPushButton:hover { background-color: #505050; }");
	}
}

void inject_preview_button()
{
	if (g_preview_btn)
		return;
	QMainWindow *win = main_window();
	if (!win)
		return;
	QWidget *container = win->findChild<QWidget *>("previewContainer");
	if (!container)
		return;
	QVBoxLayout *layout = qobject_cast<QVBoxLayout *>(container->layout());
	if (!layout)
		return;

	auto *btn = new QPushButton(container);
	btn->setMinimumHeight(28);
	btn->setMaximumHeight(36);
	layout->addWidget(btn);
	QObject::connect(btn, &QPushButton::clicked, btn, [btn]() {
		audio_cue_toggle(AUDIO_CUE_TARGET_PREVIEW);
		apply_button_style(
			btn, audio_cue_is_active(AUDIO_CUE_TARGET_PREVIEW),
			"Monitor");
	});
	apply_button_style(btn, audio_cue_is_active(AUDIO_CUE_TARGET_PREVIEW),
			   "Monitor");
	g_preview_btn = btn;
}

QWidget *find_program_widget()
{
	QMainWindow *win = main_window();
	if (!win)
		return nullptr;
	QHBoxLayout *previewLayout =
		win->findChild<QHBoxLayout *>("previewLayout");
	if (!previewLayout)
		return nullptr;

	for (int i = previewLayout->count() - 1; i >= 0; --i) {
		QLayoutItem *it = previewLayout->itemAt(i);
		if (!it)
			continue;
		QWidget *w = it->widget();
		if (!w)
			continue;
		QString n = w->objectName();
		if (n == "previewContainer" || n == "previewDisabledWidget")
			continue;
		QVBoxLayout *vl = qobject_cast<QVBoxLayout *>(w->layout());
		if (!vl)
			continue;
		for (int j = 0; j < vl->count(); ++j) {
			QLayoutItem *jit = vl->itemAt(j);
			if (!jit)
				continue;
			QLabel *lbl = qobject_cast<QLabel *>(jit->widget());
			if (!lbl)
				continue;
			QVariant v = lbl->property("class");
			if (v.isValid() &&
			    v.toString() == "label-preview-title")
				return w;
		}
	}
	return nullptr;
}

void inject_program_button()
{
	if (g_program_btn)
		return;
	QWidget *progWidget = find_program_widget();
	if (!progWidget)
		return;
	QVBoxLayout *layout = qobject_cast<QVBoxLayout *>(progWidget->layout());
	if (!layout)
		return;

	auto *btn = new QPushButton(progWidget);
	btn->setMinimumHeight(28);
	btn->setMaximumHeight(36);
	layout->addWidget(btn);
	QObject::connect(btn, &QPushButton::clicked, btn, [btn]() {
		audio_cue_toggle(AUDIO_CUE_TARGET_PROGRAM);
		apply_button_style(
			btn, audio_cue_is_active(AUDIO_CUE_TARGET_PROGRAM),
			"Monitor");
	});
	apply_button_style(btn, audio_cue_is_active(AUDIO_CUE_TARGET_PROGRAM),
			   "Monitor");
	g_program_btn = btn;
}

void update_preview_visibility()
{
	if (!g_preview_btn)
		return;
	g_preview_btn->setVisible(obs_frontend_preview_program_mode_active());
}

void try_inject_all()
{
	inject_preview_button();
	inject_program_button();
	update_preview_visibility();
}

void poll_button_state()
{
	if (g_preview_btn)
		apply_button_style(
			g_preview_btn,
			audio_cue_is_active(AUDIO_CUE_TARGET_PREVIEW),
			"Monitor");
	if (g_program_btn)
		apply_button_style(
			g_program_btn,
			audio_cue_is_active(AUDIO_CUE_TARGET_PROGRAM),
			"Monitor");
}

void on_frontend_event(enum obs_frontend_event ev, void *)
{
	switch (ev) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		QTimer::singleShot(0, try_inject_all);
		QTimer::singleShot(500, try_inject_all);
		QTimer::singleShot(1500, try_inject_all);
		if (!g_poll_timer) {
			QMainWindow *win = main_window();
			g_poll_timer = new QTimer(win);
			QObject::connect(g_poll_timer, &QTimer::timeout,
					 poll_button_state);
			g_poll_timer->start(250);
		}
		break;
	case OBS_FRONTEND_EVENT_STUDIO_MODE_ENABLED:
		QTimer::singleShot(0, try_inject_all);
		QTimer::singleShot(200, try_inject_all);
		break;
	case OBS_FRONTEND_EVENT_STUDIO_MODE_DISABLED:
		/* OBS deletes programWidget; our QPushButton goes with it.
		 * Drop the dangling pointer and turn off any active cue
		 * (cue is meaningless without studio mode — and we hide the
		 * preview button to match). */
		g_program_btn.clear();
		if (audio_cue_is_active(AUDIO_CUE_TARGET_PREVIEW))
			audio_cue_set_active(AUDIO_CUE_TARGET_PREVIEW, false);
		if (audio_cue_is_active(AUDIO_CUE_TARGET_PROGRAM))
			audio_cue_set_active(AUDIO_CUE_TARGET_PROGRAM, false);
		update_preview_visibility();
		break;
	case OBS_FRONTEND_EVENT_PREVIEW_SCENE_CHANGED:
	case OBS_FRONTEND_EVENT_SCENE_CHANGED:
		schedule_scene_state_update();
		break;
	case OBS_FRONTEND_EVENT_EXIT:
		if (g_poll_timer) {
			g_poll_timer->stop();
			g_poll_timer.clear();
		}
		break;
	default:
		break;
	}
}

} // namespace

extern "C" void audio_cue_ui_init(void)
{
	obs_frontend_add_event_callback(on_frontend_event, nullptr);
}
