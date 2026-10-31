#include <string.h>

#include "lang/en.h"
#include "common/log.h"
#include "common/startup.h"
#include "launch.h"
#include "outline-list.h"
#include "parse.h"
#include "state.h"
#include "tab.h"
#include "window-private.h"

#define SIDEBAR_WIDTH_FRACTION 0.24
#define SIDEBAR_MINIMUM_WIDTH 200
#define SIDEBAR_MAXIMUM_WIDTH 300
#define NARROW_CONDITION "max-width: 640sp"
#define BACK_BUTTON 8
/* Put before the title of a document with unsaved changes */
#define MODIFIED_MARK "\u2022 "
#define FORWARD_BUTTON 9



Tab *get_current_tab(Window *window) {
	AdwTabPage *page = adw_tab_view_get_selected_page(ADW_TAB_VIEW(window->tab_view));
	return page ? get_tab(adw_tab_page_get_child(page)) : NULL;
}


static void update_outline(Window *window) {
	Tab *tab = get_current_tab(window);
	gboolean is_new_document = tab != window->outline_tab
		|| (tab && tab->outline_generation != window->outline_generation);
	window->outline_tab = tab;
	window->outline_generation = tab ? tab->outline_generation : 0;
	show_outline_entries(window->outline_list, tab ? tab->context.outline : NULL, is_new_document);
	gtk_stack_set_visible_child_name(GTK_STACK(window->outline_stack),
		count_outline_entries(window->outline_list) ? "list" : "empty");
}


static void jump_to_heading(guint index, gpointer data) {
	Window *window = data;
	Tab *tab = get_current_tab(window);
	if (!tab || tab != window->outline_tab || index >= tab->context.outline->len)
		return;
	OutlineEntry *entry = g_ptr_array_index(tab->context.outline, index);
	show_tab_heading(tab, entry);
	if (adw_overlay_split_view_get_collapsed(ADW_OVERLAY_SPLIT_VIEW(window->split)))
		adw_overlay_split_view_set_show_sidebar(ADW_OVERLAY_SPLIT_VIEW(window->split), FALSE);
}


static char *get_display_directory(GFile *file) {
	g_autoptr(GFile) parent = g_file_get_parent(file);
	g_autofree char *path = parent ? g_file_get_parse_name(parent) : g_strdup("");
	const char *home = g_get_home_dir();
	if (g_str_has_prefix(path, home) && (path[strlen(home)] == '/' || path[strlen(home)] == 0))
		return g_strconcat("~", path + strlen(home), NULL);
	return g_steal_pointer(&path);
}


/* Shows the contents sidebar and its button only for documents with headings, keeping the reader's choice */
static void update_sidebar(Window *window, gboolean has_contents) {
	AdwOverlaySplitView *split = ADW_OVERLAY_SPLIT_VIEW(window->split);
	window->has_contents = has_contents;
	gtk_widget_set_visible(window->sidebar_button, has_contents);
	gboolean should_show = has_contents && window->shows_sidebar && !adw_overlay_split_view_get_collapsed(split);
	if (!has_contents || should_show)
		adw_overlay_split_view_set_show_sidebar(split, should_show);
}


static void update_window(Window *window) {
	Tab *tab = get_current_tab(window);
	gboolean has_tab = tab != NULL;
	window->is_started = window->is_started || has_tab;
	gtk_stack_set_visible_child_name(GTK_STACK(window->stack), has_tab ? "tabs" : window->is_started ? "empty" : "blank");
	gboolean can_switch_editing = has_tab && can_switch_tab_editing(tab);
	gtk_widget_set_visible(window->edit_button, can_switch_editing);
	g_simple_action_set_enabled(window->edit_action, can_switch_editing);
	g_simple_action_set_enabled(window->save_action, has_tab && is_tab_modified(tab));
	g_simple_action_set_enabled(window->close_action, has_tab);
	g_simple_action_set_state(window->edit_action, g_variant_new_boolean(has_tab && tab->is_editing));
	update_sidebar(window, has_tab && tab->has_contents);
	if (!has_tab) {
		adw_window_title_set_title(ADW_WINDOW_TITLE(window->title), TEXT_APPLICATION_NAME);
		adw_window_title_set_subtitle(ADW_WINDOW_TITLE(window->title), NULL);
	} else {
		g_autofree char *name = get_tab_title(tab);
		g_autofree char *title = is_tab_modified(tab) ? g_strconcat(MODIFIED_MARK, name, NULL) : g_strdup(name);
		g_autofree char *directory = get_display_directory(tab->file);
		adw_window_title_set_title(ADW_WINDOW_TITLE(window->title), title);
		adw_window_title_set_subtitle(ADW_WINDOW_TITLE(window->title), directory);
	}
	update_outline(window);
}


static void notice_outline_change(Tab *tab, gpointer data) {
	Window *window = data;
	if (tab == get_current_tab(window))
		update_outline(window);
}


static void notice_tab_state(Tab *tab, gpointer data) {
	Window *window = data;
	g_autoptr(GIcon) icon = is_tab_modified(tab) ? g_themed_icon_new("media-record-symbolic") : NULL;
	adw_tab_page_set_indicator_icon(tab->page, icon);
	adw_tab_page_set_indicator_tooltip(tab->page, TEXT_UNSAVED);
	adw_tab_page_set_indicator_activatable(tab->page, icon != NULL);
	if (tab == get_current_tab(window))
		update_window(window);
}


static void show_tab_message(Tab *tab, const char *message, gpointer data) {
	Window *window = data;
	adw_toast_overlay_add_toast(ADW_TOAST_OVERLAY(window->toasts), adw_toast_new(message));
}


static void finish_folder_question(GObject *dialog, GAsyncResult *result, gpointer data) {
	g_autoptr(GtkWidget) root = data;
	const char *response = adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(dialog), result);
	finish_tab_folder_question(get_tab(root), strcmp(response, "create") == 0);
}


static void ask_for_folder(Tab *tab, gpointer data) {
	Window *window = data;
	g_autoptr(GFile) folder = g_file_get_parent(tab->file);
	g_autofree char *name = g_file_get_parse_name(folder);
	AdwDialog *dialog = adw_alert_dialog_new(TEXT_FOLDER_MISSING_QUESTION, NULL);
	adw_alert_dialog_format_body(ADW_ALERT_DIALOG(dialog), TEXT_FOLDER_MISSING_DETAIL, name);
	adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "cancel", TEXT_CANCEL, "create", TEXT_CREATE_AND_SAVE,
		NULL);
	adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "create", ADW_RESPONSE_SUGGESTED);
	adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "create");
	adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");
	adw_alert_dialog_choose(ADW_ALERT_DIALOG(dialog), window->window, NULL, finish_folder_question,
		g_object_ref(tab->root));
}


static void finish_link_question(GObject *dialog, GAsyncResult *result, gpointer data) {
	Window *window = data;
	if (strcmp(adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(dialog), result), "open") != 0)
		return;
	launch_default(window->window, g_object_get_data(dialog, "link"));
}


/* A local link waiting to learn whether its target exists */
typedef struct {
	Window *window;
	GFile *file;
	char *anchor;
	gboolean is_wiki;
} LinkRequest;


static void free_link_request(LinkRequest *request) {
	g_object_unref(request->file);
	g_free(request->anchor);
	g_free(request);
}


static void open_local_link(GObject *source, GAsyncResult *result, gpointer data) {
	LinkRequest *request = data;
	g_autoptr(GFileInfo) info = g_file_query_info_finish(G_FILE(source), result, NULL);
	g_autofree char *name = g_file_get_basename(request->file);
	gboolean is_shown_here = info && g_file_info_get_file_type(info) == G_FILE_TYPE_REGULAR
		&& (request->is_wiki || is_document_name(name));
	if (is_shown_here) {
		Tab *tab = open_window_file(request->window, request->file);
		if (request->anchor)
			show_tab_anchor(tab, request->anchor);
	} else {
		g_autofree char *uri = g_file_get_uri(request->file);
		launch_default(request->window->window, uri);
	}
	free_link_request(request);
}


static void ask_to_open_link(Window *window, const char *link) {
	AdwDialog *dialog = adw_alert_dialog_new(TEXT_OPEN_LINK_QUESTION, link);
	adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "cancel", TEXT_CANCEL, "open", TEXT_OPEN_LINK, NULL);
	adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "open", ADW_RESPONSE_SUGGESTED);
	adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "open");
	adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");
	g_object_set_data_full(G_OBJECT(dialog), "link", g_strdup(link), g_free);
	adw_alert_dialog_choose(ADW_ALERT_DIALOG(dialog), window->window, NULL, finish_link_question, window);
}


/* Opens documents this viewer shows as tabs, other local files in their own application and asks before the web */
static void open_link(Tab *tab, const char *link, gpointer data) {
	Window *window = data;
	gboolean is_wiki = g_str_has_prefix(link, "wiki:");
	if (!is_wiki && g_uri_peek_scheme(link)) {
		ask_to_open_link(window, link);
		return;
	}
	const char *target_text = is_wiki ? link + 5 : link;
	gsize target_length = strcspn(target_text, is_wiki ? "#|" : "#?");
	const char *anchor = target_text[target_length] == '#' ? target_text + target_length + 1 : NULL;
	g_autofree char *escaped = g_strndup(target_text, target_length);
	g_autofree char *target = is_wiki ? g_strdup(escaped) : g_uri_unescape_string(escaped, NULL);
	if (!target || !*target)
		return;
	if (is_wiki && !is_document_name(target))
		g_set_str(&target, g_strconcat(target, ".md", NULL));
	g_autoptr(GFile) parent = g_file_get_parent(tab->file);
	LinkRequest *request = g_new0(LinkRequest, 1);
	request->window = window;
	request->file = g_path_is_absolute(target) || !parent ? g_file_new_for_path(target)
		: g_file_resolve_relative_path(parent, target);
	request->anchor = anchor && *anchor ? g_uri_unescape_string(anchor, NULL) : NULL;
	request->is_wiki = is_wiki;
	g_file_query_info_async(request->file, G_FILE_ATTRIBUTE_STANDARD_TYPE, G_FILE_QUERY_INFO_NONE, G_PRIORITY_DEFAULT,
		NULL, open_local_link, request);
}


Tab *open_window_file(Window *window, GFile *file) {
	AdwTabView *view = ADW_TAB_VIEW(window->tab_view);
	for (int index = 0; index < adw_tab_view_get_n_pages(view); index++) {
		AdwTabPage *page = adw_tab_view_get_nth_page(view, index);
		Tab *open = get_tab(adw_tab_page_get_child(page));
		if (g_file_equal(open->file, file)) {
			adw_tab_view_set_selected_page(view, page);
			return open;
		}
	}
	TabEvents events = {notice_outline_change, notice_tab_state, open_link, show_tab_message, ask_for_folder,
		notice_find_count, window};
	Tab *tab = create_tab(file, &events);
	tab->page = adw_tab_view_append(view, tab->root);
	g_autofree char *title = get_tab_title(tab);
	g_autofree char *path = g_file_get_parse_name(file);
	adw_tab_page_set_title(tab->page, title);
	adw_tab_page_set_tooltip(tab->page, path);
	adw_tab_view_set_selected_page(view, tab->page);
	update_window(window);
	return tab;
}


static void finish_close_question(GObject *dialog, GAsyncResult *result, gpointer data) {
	g_autoptr(AdwTabPage) page = data;
	const char *response = adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(dialog), result);
	GtkWidget *view = gtk_widget_get_ancestor(adw_tab_page_get_child(page), ADW_TYPE_TAB_VIEW);
	if (!view)
		return;
	Tab *tab = get_tab(adw_tab_page_get_child(page));
	if (strcmp(response, "save") == 0) {
		save_and_close_tab(tab);
		return;
	}
	gboolean is_closing = strcmp(response, "cancel") != 0;
	if (is_closing)
		shut_down_tab(tab);
	adw_tab_view_close_page_finish(ADW_TAB_VIEW(view), page, is_closing);
}


static gboolean close_page(AdwTabView *view, AdwTabPage *page, Window *window) {
	Tab *tab = get_tab(adw_tab_page_get_child(page));
	if (!is_tab_modified(tab)) {
		shut_down_tab(tab);
		adw_tab_view_close_page_finish(view, page, TRUE);
		return GDK_EVENT_STOP;
	}
	g_autofree char *title = get_tab_title(tab);
	AdwDialog *dialog = adw_alert_dialog_new(TEXT_SAVE_QUESTION, NULL);
	adw_alert_dialog_format_body(ADW_ALERT_DIALOG(dialog), TEXT_SAVE_DETAIL, title);
	adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "cancel", TEXT_CANCEL, "discard", TEXT_DISCARD,
		"save", TEXT_SAVE, NULL);
	adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "discard", ADW_RESPONSE_DESTRUCTIVE);
	adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "save", ADW_RESPONSE_SUGGESTED);
	adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "save");
	adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");
	adw_alert_dialog_choose(ADW_ALERT_DIALOG(dialog), window->window, NULL, finish_close_question, g_object_ref(page));
	return GDK_EVENT_STOP;
}


static void save_indicated_tab(AdwTabView *view, AdwTabPage *page, Window *window) {
	save_tab(get_tab(adw_tab_page_get_child(page)));
}


static void open_chosen_files(GObject *dialog, GAsyncResult *result, gpointer data) {
	g_autoptr(GListModel) files = gtk_file_dialog_open_multiple_finish(GTK_FILE_DIALOG(dialog), result, NULL);
	for (guint index = 0; files && index < g_list_model_get_n_items(files); index++) {
		g_autoptr(GFile) file = g_list_model_get_item(files, index);
		open_window_file(data, file);
	}
}


static void choose_files(GSimpleAction *action, GVariant *parameter, gpointer data) {
	Window *window = data;
	g_autoptr(GtkFileDialog) dialog = gtk_file_dialog_new();
	gtk_file_dialog_set_title(dialog, TEXT_OPEN_TITLE);
	g_autoptr(GListStore) filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
	g_autoptr(GtkFileFilter) markdown = gtk_file_filter_new();
	gtk_file_filter_set_name(markdown, TEXT_MARKDOWN_FILTER);
	gtk_file_filter_add_mime_type(markdown, "text/markdown");
	for (guint index = 0; index < MARKDOWN_EXTENSION_COUNT; index++)
		gtk_file_filter_add_suffix(markdown, MARKDOWN_EXTENSIONS[index]);
	g_autoptr(GtkFileFilter) everything = gtk_file_filter_new();
	gtk_file_filter_set_name(everything, TEXT_ALL_FILTER);
	gtk_file_filter_add_pattern(everything, "*");
	g_list_store_append(filters, markdown);
	g_list_store_append(filters, everything);
	gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
	Tab *tab = get_current_tab(window);
	g_autoptr(GFile) folder = tab ? g_file_get_parent(tab->file) : NULL;
	gtk_file_dialog_set_initial_folder(dialog, folder);
	gtk_file_dialog_open_multiple(dialog, GTK_WINDOW(window->window), NULL, open_chosen_files, window);
}


static void save_current(GSimpleAction *action, GVariant *parameter, gpointer data) {
	Tab *tab = get_current_tab(data);
	if (tab)
		save_tab(tab);
}


static void change_editing(GSimpleAction *action, GVariant *value, gpointer data) {
	Tab *tab = get_current_tab(data);
	if (tab)
		set_tab_editing(tab, g_variant_get_boolean(value));
	g_simple_action_set_state(action, value);
}


static void go_back(GSimpleAction *action, GVariant *parameter, gpointer data) {
	Tab *tab = get_current_tab(data);
	if (tab)
		go_back_in_tab(tab);
}


static void go_forward(GSimpleAction *action, GVariant *parameter, gpointer data) {
	Tab *tab = get_current_tab(data);
	if (tab)
		go_forward_in_tab(tab);
}


/* Mouse buttons eight and nine are the side buttons that browsers use for back and forward */
static void press_side_button(GtkGestureClick *gesture, int presses, double x, double y, Window *window) {
	guint button = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture));
	const char *action = button == BACK_BUTTON ? "back" : button == FORWARD_BUTTON ? "forward" : NULL;
	if (!action)
		return;
	g_action_group_activate_action(G_ACTION_GROUP(window->window), action, NULL);
	gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
}


static void close_current(GSimpleAction *action, GVariant *parameter, gpointer data) {
	Window *window = data;
	AdwTabPage *page = adw_tab_view_get_selected_page(ADW_TAB_VIEW(window->tab_view));
	if (page)
		adw_tab_view_close_page(ADW_TAB_VIEW(window->tab_view), page);
}


static gboolean drop_files(GtkDropTarget *target, const GValue *value, double x, double y, Window *window) {
	GSList *files = gdk_file_list_get_files(g_value_get_boxed(value));
	for (GSList *item = files; item; item = item->next)
		open_window_file(window, item->data);
	gboolean has_files = files != NULL;
	g_slist_free(files);
	return has_files;
}


/* F9 cannot open the sidebar for a file without headings, and hiding it then is not the reader's choice */
static void remember_sidebar(Window *window) {
	AdwOverlaySplitView *split = ADW_OVERLAY_SPLIT_VIEW(window->split);
	if (!window->has_contents) {
		if (adw_overlay_split_view_get_show_sidebar(split))
			adw_overlay_split_view_set_show_sidebar(split, FALSE);
		return;
	}
	if (!adw_overlay_split_view_get_collapsed(split))
		window->shows_sidebar = adw_overlay_split_view_get_show_sidebar(split);
}


static gboolean has_modified_tab(Window *window) {
	AdwTabView *view = ADW_TAB_VIEW(window->tab_view);
	for (int index = 0; index < adw_tab_view_get_n_pages(view); index++)
		if (is_tab_modified(get_tab(adw_tab_page_get_child(adw_tab_view_get_nth_page(view, index)))))
			return TRUE;
	return FALSE;
}


static void finish_quit_question(GObject *dialog, GAsyncResult *result, gpointer data) {
	Window *window = data;
	if (strcmp(adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(dialog), result), "discard") != 0)
		return;
	window->is_close_confirmed = TRUE;
	gtk_window_close(GTK_WINDOW(window->window));
}


static gboolean close_window(GtkWindow *gtk_window, Window *window) {
	if (!window->is_close_confirmed && has_modified_tab(window)) {
		AdwDialog *dialog = adw_alert_dialog_new(TEXT_DISCARD_QUESTION, TEXT_DISCARD_DETAIL);
		adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "cancel", TEXT_CANCEL, "discard", TEXT_DISCARD, NULL);
		adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "discard", ADW_RESPONSE_DESTRUCTIVE);
		adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");
		adw_alert_dialog_choose(ADW_ALERT_DIALOG(dialog), window->window, NULL, finish_quit_question, window);
		return GDK_EVENT_STOP;
	}
	remember_sidebar(window);
	WindowState state = {.shows_sidebar = window->shows_sidebar};
	read_window_geometry(gtk_window, &state.geometry);
	save_window_state(&state);
	hide_closing_window(gtk_window);
	AdwTabView *view = ADW_TAB_VIEW(window->tab_view);
	for (int index = 0; index < adw_tab_view_get_n_pages(view); index++)
		shut_down_tab(get_tab(adw_tab_page_get_child(adw_tab_view_get_nth_page(view, index))));
	return GDK_EVENT_PROPAGATE;
}


static void note_window_shown(GtkWidget *widget) {
	note_startup_moment("Window shown");
	note_first_frame(widget);
}


static GtkWidget *create_sidebar(Window *window) {
	window->outline_list = create_outline_list(jump_to_heading, window);
	gtk_widget_add_css_class(window->outline_list, "navigation-sidebar");
	GtkWidget *scroller = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), window->outline_list);
	GtkWidget *empty = gtk_label_new(TEXT_NO_HEADINGS);
	gtk_widget_add_css_class(empty, "dimmed");
	window->outline_stack = gtk_stack_new();
	gtk_stack_set_transition_type(GTK_STACK(window->outline_stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
	gtk_stack_add_named(GTK_STACK(window->outline_stack), scroller, "list");
	gtk_stack_add_named(GTK_STACK(window->outline_stack), empty, "empty");
	gtk_stack_set_visible_child_name(GTK_STACK(window->outline_stack), "empty");
	GtkWidget *header = adw_header_bar_new();
	adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), adw_window_title_new(TEXT_CONTENTS, NULL));
	GtkWidget *toolbar = adw_toolbar_view_new();
	adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
	adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), window->outline_stack);
	return toolbar;
}


static GtkWidget *create_header(Window *window) {
	GtkWidget *header = adw_header_bar_new();
	window->title = adw_window_title_new(TEXT_APPLICATION_NAME, NULL);
	adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), window->title);
	window->sidebar_button = gtk_toggle_button_new();
	gtk_button_set_icon_name(GTK_BUTTON(window->sidebar_button), "sidebar-show-symbolic");
	gtk_widget_set_tooltip_text(window->sidebar_button, TEXT_SIDEBAR);
	gtk_actionable_set_action_name(GTK_ACTIONABLE(window->sidebar_button), "win.sidebar");
	adw_header_bar_pack_start(ADW_HEADER_BAR(header), window->sidebar_button);
	g_autoptr(GMenu) menu = g_menu_new();
	g_menu_append(menu, TEXT_OPEN, "win.open");
	g_menu_append(menu, TEXT_SAVE, "win.save");
	g_menu_append(menu, TEXT_FIND_MENU, "win.find");
	g_menu_append(menu, TEXT_SETTINGS, "app.settings");
	g_menu_append(menu, TEXT_ABOUT, "app.about");
	GtkWidget *menu_button = gtk_menu_button_new();
	gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(menu_button), "open-menu-symbolic");
	gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(menu_button), G_MENU_MODEL(menu));
	gtk_widget_set_tooltip_text(menu_button, TEXT_MENU);
	adw_header_bar_pack_end(ADW_HEADER_BAR(header), menu_button);
	window->edit_button = gtk_toggle_button_new();
	gtk_button_set_icon_name(GTK_BUTTON(window->edit_button), "document-edit-symbolic");
	gtk_widget_set_tooltip_text(window->edit_button, TEXT_EDIT);
	gtk_actionable_set_action_name(GTK_ACTIONABLE(window->edit_button), "win.edit");
	adw_header_bar_pack_end(ADW_HEADER_BAR(header), window->edit_button);
	return header;
}


static GtkWidget *create_empty_page(void) {
	GtkWidget *button = gtk_button_new_with_label(TEXT_OPEN);
	gtk_actionable_set_action_name(GTK_ACTIONABLE(button), "win.open");
	gtk_widget_set_halign(button, GTK_ALIGN_CENTER);
	gtk_widget_add_css_class(button, "pill");
	gtk_widget_add_css_class(button, "suggested-action");
	GtkWidget *page = adw_status_page_new();
	adw_status_page_set_icon_name(ADW_STATUS_PAGE(page), "text-x-generic-symbolic");
	adw_status_page_set_title(ADW_STATUS_PAGE(page), TEXT_EMPTY_TITLE);
	adw_status_page_set_description(ADW_STATUS_PAGE(page), TEXT_EMPTY_DESCRIPTION);
	adw_status_page_set_child(ADW_STATUS_PAGE(page), button);
	return page;
}


static GtkWidget *create_content(Window *window) {
	window->tab_view = GTK_WIDGET(adw_tab_view_new());
	g_signal_connect(window->tab_view, "close-page", G_CALLBACK(close_page), window);
	g_signal_connect(window->tab_view, "indicator-activated", G_CALLBACK(save_indicated_tab), window);
	g_signal_connect_swapped(window->tab_view, "notify::selected-page", G_CALLBACK(update_window), window);
	g_signal_connect_swapped(window->tab_view, "notify::selected-page", G_CALLBACK(refresh_find), window);
	g_signal_connect_swapped(window->tab_view, "notify::n-pages", G_CALLBACK(update_window), window);
	AdwTabBar *tab_bar = adw_tab_bar_new();
	adw_tab_bar_set_view(tab_bar, ADW_TAB_VIEW(window->tab_view));
	adw_tab_bar_set_autohide(tab_bar, TRUE);
	window->stack = gtk_stack_new();
	gtk_stack_set_transition_type(GTK_STACK(window->stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
	/* The window opens blank, so a document opened with it never shows the empty page first */
	gtk_stack_add_named(GTK_STACK(window->stack), gtk_box_new(GTK_ORIENTATION_VERTICAL, 0), "blank");
	gtk_stack_add_named(GTK_STACK(window->stack), create_empty_page(), "empty");
	gtk_stack_add_named(GTK_STACK(window->stack), window->tab_view, "tabs");
	GtkWidget *toolbar = adw_toolbar_view_new();
	adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), create_header(window));
	adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), GTK_WIDGET(tab_bar));
	adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), create_find_bar(window));
	adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), window->stack);
	return toolbar;
}


static void add_actions(Window *window) {
	static const GActionEntry ENTRIES[] = {
		{"open", choose_files, NULL, NULL, NULL, {0}},
		{"save", save_current, NULL, NULL, NULL, {0}},
		{"edit", NULL, NULL, "false", change_editing, {0}},
		{"close-tab", close_current, NULL, NULL, NULL, {0}},
		{"back", go_back, NULL, NULL, NULL, {0}},
		{"forward", go_forward, NULL, NULL, NULL, {0}},
		{"zoom-in", zoom_in, NULL, NULL, NULL, {0}},
		{"zoom-out", zoom_out, NULL, NULL, NULL, {0}},
		{"zoom-reset", reset_zoom, NULL, NULL, NULL, {0}},
		{"find", show_find, NULL, NULL, NULL, {0}},
	};
	GActionMap *map = G_ACTION_MAP(window->window);
	g_action_map_add_action_entries(map, ENTRIES, G_N_ELEMENTS(ENTRIES), window);
	g_autoptr(GPropertyAction) sidebar = g_property_action_new("sidebar", window->split, "show-sidebar");
	g_action_map_add_action(map, G_ACTION(sidebar));
	window->edit_action = G_SIMPLE_ACTION(g_action_map_lookup_action(map, "edit"));
	window->save_action = G_SIMPLE_ACTION(g_action_map_lookup_action(map, "save"));
	window->close_action = G_SIMPLE_ACTION(g_action_map_lookup_action(map, "close-tab"));
}


static void free_window(gpointer data) {
	Window *window = data;
	g_clear_weak_pointer(&window->zoom_toast);
	g_free(window);
}


Window *create_window(GtkApplication *application) {
	Window *window = g_new0(Window, 1);
	WindowState state;
	load_window_state(&state);
	window->shows_sidebar = state.shows_sidebar;
	window->window = adw_application_window_new(application);
	g_object_set_data_full(G_OBJECT(window->window), "window", window, free_window);
	gtk_window_set_title(GTK_WINDOW(window->window), TEXT_APPLICATION_NAME);
	note_startup_moment("Window created");
	apply_window_geometry(GTK_WINDOW(window->window), &state.geometry);
	window->split = adw_overlay_split_view_new();
	adw_overlay_split_view_set_sidebar(ADW_OVERLAY_SPLIT_VIEW(window->split), create_sidebar(window));
	adw_overlay_split_view_set_content(ADW_OVERLAY_SPLIT_VIEW(window->split), create_content(window));
	adw_overlay_split_view_set_sidebar_width_fraction(ADW_OVERLAY_SPLIT_VIEW(window->split), SIDEBAR_WIDTH_FRACTION);
	adw_overlay_split_view_set_min_sidebar_width(ADW_OVERLAY_SPLIT_VIEW(window->split), SIDEBAR_MINIMUM_WIDTH);
	adw_overlay_split_view_set_max_sidebar_width(ADW_OVERLAY_SPLIT_VIEW(window->split), SIDEBAR_MAXIMUM_WIDTH);
	/* The sidebar opens once a document with headings arrives */
	adw_overlay_split_view_set_show_sidebar(ADW_OVERLAY_SPLIT_VIEW(window->split), FALSE);
	g_signal_connect_swapped(window->split, "notify::show-sidebar", G_CALLBACK(remember_sidebar), window);
	window->toasts = adw_toast_overlay_new();
	adw_toast_overlay_set_child(ADW_TOAST_OVERLAY(window->toasts), window->split);
	adw_application_window_set_content(ADW_APPLICATION_WINDOW(window->window), window->toasts);
	AdwBreakpoint *breakpoint = adw_breakpoint_new(adw_breakpoint_condition_parse(NARROW_CONDITION));
	GValue collapsed = G_VALUE_INIT;
	g_value_set_boolean(g_value_init(&collapsed, G_TYPE_BOOLEAN), TRUE);
	adw_breakpoint_add_setter(breakpoint, G_OBJECT(window->split), "collapsed", &collapsed);
	adw_application_window_add_breakpoint(ADW_APPLICATION_WINDOW(window->window), breakpoint);
	add_actions(window);
	GtkGesture *side_buttons = gtk_gesture_click_new();
	gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(side_buttons), 0);
	gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(side_buttons), GTK_PHASE_CAPTURE);
	g_signal_connect(side_buttons, "pressed", G_CALLBACK(press_side_button), window);
	gtk_widget_add_controller(window->window, GTK_EVENT_CONTROLLER(side_buttons));
	GtkDropTarget *drop = gtk_drop_target_new(GDK_TYPE_FILE_LIST, GDK_ACTION_COPY);
	g_signal_connect(drop, "drop", G_CALLBACK(drop_files), window);
	gtk_widget_add_controller(window->window, GTK_EVENT_CONTROLLER(drop));
	g_signal_connect(window->window, "close-request", G_CALLBACK(close_window), window);
	g_signal_connect(window->window, "map", G_CALLBACK(note_window_shown), NULL);
	update_window(window);
	return window;
}


GtkWindow *get_gtk_window(Window *window) {
	return GTK_WINDOW(window->window);
}


static gboolean fade_in_empty_page(GtkWidget *widget, GdkFrameClock *clock, gpointer data) {
	Window *window = data;
	window->is_started = TRUE;
	update_window(window);
	return G_SOURCE_REMOVE;
}


/* Fades the empty page in on the next frame when the window was opened without a document */
void show_window_start(Window *window) {
	if (!window->is_started)
		gtk_widget_add_tick_callback(window->window, fade_in_empty_page, window, NULL);
}
