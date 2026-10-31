#pragma once

#include "window/tab.h"
#include "window/window.h"


/* The application window with its tabs, contents sidebar and header controls */
struct Window {
	GtkWidget *window, *split, *tab_view, *title, *stack, *edit_button, *outline_list, *outline_stack, *toasts;
	GtkWidget *find_bar, *find_entry, *find_count, *sidebar_button;
	GSimpleAction *edit_action, *save_action, *close_action;
	/* Tab and outline version the sidebar currently lists */
	Tab *outline_tab;
	guint outline_generation;
	gboolean shows_sidebar, is_close_confirmed;
	/* The current tab has headings for the contents sidebar */
	gboolean has_contents;
	/* The window has shown a document or the empty page, so it no longer opens blank */
	gboolean is_started;
	/* Toast showing the text size while zooming */
	AdwToast *zoom_toast;
};

Tab *get_current_tab(Window *window);
void zoom_in(GSimpleAction *action, GVariant *parameter, gpointer data);
void zoom_out(GSimpleAction *action, GVariant *parameter, gpointer data);
void reset_zoom(GSimpleAction *action, GVariant *parameter, gpointer data);
GtkWidget *create_find_bar(Window *window);
void show_find(GSimpleAction *action, GVariant *parameter, gpointer data);
void refresh_find(Window *window);
void notice_find_count(Tab *tab, gpointer data);
