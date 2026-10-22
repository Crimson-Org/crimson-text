#include "settings.h"
#include "window-private.h"

#define ZOOM_STEP 10
#define ZOOM_TOAST_SECONDS 1


/* Zooms the editor while the current tab is editing and the reading view otherwise, as Settings stores them */
static void change_zoom(Window *window, int step) {
	Tab *tab = get_current_tab(window);
	Settings *settings = get_settings();
	int *size = tab && tab->is_editing ? &settings->editor_size : &settings->reading_size;
	*size = step ? CLAMP(*size + step, MINIMUM_TEXT_SIZE, MAXIMUM_TEXT_SIZE) : DEFAULT_TEXT_SIZE;
	apply_settings();
	save_settings();
	if (window->zoom_toast)
		adw_toast_dismiss(window->zoom_toast);
	g_autofree char *text = g_strdup_printf("%d%%", *size);
	AdwToast *toast = adw_toast_new(text);
	adw_toast_set_timeout(toast, ZOOM_TOAST_SECONDS);
	g_set_weak_pointer(&window->zoom_toast, toast);
	adw_toast_overlay_add_toast(ADW_TOAST_OVERLAY(window->toasts), toast);
}


void zoom_in(GSimpleAction *action, GVariant *parameter, gpointer data) {
	change_zoom(data, ZOOM_STEP);
}


void zoom_out(GSimpleAction *action, GVariant *parameter, gpointer data) {
	change_zoom(data, -ZOOM_STEP);
}


void reset_zoom(GSimpleAction *action, GVariant *parameter, gpointer data) {
	change_zoom(data, 0);
}
