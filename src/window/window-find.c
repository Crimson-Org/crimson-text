#include "lang/en.h"
#include "window/tab-find.h"
#include "window/window-private.h"
#include "common/widgets/spacing.h"

#define FIND_BAR_WIDTH 600
/* The count sits inside the end of the field, left of the clear button and its padding */
#define CLEAR_BUTTON_ROOM 32


static void run_find(Window *window, FindDirection direction) {
	Tab *tab = get_current_tab(window);
	const char *query = gtk_editable_get_text(GTK_EDITABLE(window->find_entry));
	int total = 0;
	int current = tab ? find_in_tab(tab, query, direction, &total) : 0;
	/* A total below zero means the editor is still counting in the background */
	g_autofree char *count = total > 0 && current > 0 ? g_strdup_printf(TEXT_FIND_COUNT, current, total)
		: g_strdup(*query && total == 0 ? TEXT_FIND_NONE : "");
	gtk_label_set_text(GTK_LABEL(window->find_count), count);
	/* The typed text stops short of the count, which lies over the end of the field */
	int count_width = 0;
	gtk_widget_measure(window->find_count, GTK_ORIENTATION_HORIZONTAL, -1, NULL, &count_width, NULL, NULL);
	GtkWidget *text = GTK_WIDGET(gtk_editable_get_delegate(GTK_EDITABLE(window->find_entry)));
	gtk_widget_set_margin_end(text, *count ? count_width + SPACING_SMALL : 0);
}


static void change_find_text(GtkSearchEntry *entry, Window *window) {
	run_find(window, FIND_FIRST);
}


static void find_next(Window *window) {
	run_find(window, FIND_NEXT);
}


static void find_previous(Window *window) {
	run_find(window, FIND_PREVIOUS);
}


static gboolean press_find_key(GtkEventControllerKey *keys, guint key, guint code, GdkModifierType modifiers,
	Window *window) {
	if ((key != GDK_KEY_Return && key != GDK_KEY_KP_Enter) || !(modifiers & GDK_SHIFT_MASK))
		return GDK_EVENT_PROPAGATE;
	find_previous(window);
	return GDK_EVENT_STOP;
}


void refresh_find(Window *window) {
	if (gtk_search_bar_get_search_mode(GTK_SEARCH_BAR(window->find_bar)))
		run_find(window, FIND_FIRST);
}


void notice_find_count(Tab *tab, gpointer data) {
	Window *window = data;
	if (tab == get_current_tab(window) && gtk_search_bar_get_search_mode(GTK_SEARCH_BAR(window->find_bar)))
		run_find(window, FIND_SAME);
}


static void notice_find_closing(GtkSearchBar *bar, GParamSpec *property, Window *window) {
	if (gtk_search_bar_get_search_mode(bar))
		return;
	AdwTabView *view = ADW_TAB_VIEW(window->tab_view);
	for (int index = 0; index < adw_tab_view_get_n_pages(view); index++)
		stop_find_in_tab(get_tab(adw_tab_page_get_child(adw_tab_view_get_nth_page(view, index))));
}


void show_find(GSimpleAction *action, GVariant *parameter, gpointer data) {
	Window *window = data;
	gtk_search_bar_set_search_mode(GTK_SEARCH_BAR(window->find_bar), TRUE);
	gtk_widget_grab_focus(window->find_entry);
	gtk_editable_select_region(GTK_EDITABLE(window->find_entry), 0, -1);
	run_find(window, FIND_FIRST);
}


static GtkWidget *create_find_button(const char *icon, const char *tooltip, GCallback callback, Window *window) {
	GtkWidget *button = gtk_button_new_from_icon_name(icon);
	gtk_widget_set_tooltip_text(button, tooltip);
	gtk_widget_add_css_class(button, "flat");
	g_signal_connect_swapped(button, "clicked", callback, window);
	return button;
}


GtkWidget *create_find_bar(Window *window) {
	window->find_entry = gtk_search_entry_new();
	gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(window->find_entry), TEXT_FIND);
	gtk_widget_set_hexpand(window->find_entry, TRUE);
	g_signal_connect(window->find_entry, "search-changed", G_CALLBACK(change_find_text), window);
	g_signal_connect_swapped(window->find_entry, "activate", G_CALLBACK(find_next), window);
	g_signal_connect_swapped(window->find_entry, "next-match", G_CALLBACK(find_next), window);
	g_signal_connect_swapped(window->find_entry, "previous-match", G_CALLBACK(find_previous), window);
	GtkEventController *keys = gtk_event_controller_key_new();
	gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
	g_signal_connect(keys, "key-pressed", G_CALLBACK(press_find_key), window);
	gtk_widget_add_controller(window->find_entry, keys);
	window->find_count = gtk_label_new(NULL);
	gtk_widget_set_halign(window->find_count, GTK_ALIGN_END);
	gtk_widget_set_margin_end(window->find_count, CLEAR_BUTTON_ROOM);
	gtk_widget_set_can_target(window->find_count, FALSE);
	gtk_widget_add_css_class(window->find_count, "dimmed");
	gtk_widget_add_css_class(window->find_count, "numeric");
	GtkWidget *field = gtk_overlay_new();
	gtk_overlay_set_child(GTK_OVERLAY(field), window->find_entry);
	gtk_overlay_add_overlay(GTK_OVERLAY(field), window->find_count);
	GtkWidget *arrows = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_box_append(GTK_BOX(arrows), create_find_button("go-up-symbolic", TEXT_FIND_PREVIOUS,
		G_CALLBACK(find_previous), window));
	gtk_box_append(GTK_BOX(arrows), create_find_button("go-down-symbolic", TEXT_FIND_NEXT, G_CALLBACK(find_next),
		window));
	/* An empty box as wide as the arrows balances them, so the field itself sits in the middle of the page */
	GtkWidget *balance = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	GtkSizeGroup *sides = gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL);
	gtk_size_group_add_widget(sides, balance);
	gtk_size_group_add_widget(sides, arrows);
	g_object_unref(sides);
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, SPACING_SMALL);
	gtk_box_append(GTK_BOX(row), balance);
	gtk_box_append(GTK_BOX(row), field);
	gtk_box_append(GTK_BOX(row), arrows);
	GtkWidget *clamp = adw_clamp_new();
	adw_clamp_set_maximum_size(ADW_CLAMP(clamp), FIND_BAR_WIDTH);
	adw_clamp_set_child(ADW_CLAMP(clamp), row);
	window->find_bar = gtk_search_bar_new();
	gtk_search_bar_set_child(GTK_SEARCH_BAR(window->find_bar), clamp);
	gtk_search_bar_connect_entry(GTK_SEARCH_BAR(window->find_bar), GTK_EDITABLE(window->find_entry));
	gtk_search_bar_set_show_close_button(GTK_SEARCH_BAR(window->find_bar), TRUE);
	g_signal_connect(window->find_bar, "notify::search-mode-enabled", G_CALLBACK(notice_find_closing), window);
	return window->find_bar;
}
