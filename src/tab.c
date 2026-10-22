#include <string.h>

#include "../lang/en.h"
#include "charset.h"
#include "code-editor.h"
#include "data-table.h"
#include "common/loading.h"
#include "common/log.h"
#include "common/long-lines.h"
#include "document-view.h"
#include "common/smooth-scroll.h"
#include "source-view.h"
#include "tab-places.h"
#include "tab.h"

#define CONTENT_WIDTH 880
#define CONTENT_TIGHTENING 600
#define STACK_DURATION 260

static void toggle_task(long offset, gboolean is_done, gpointer data);


static void free_tab(gpointer data) {
	Tab *tab = data;
	shut_down_tab(tab);
	g_object_unref(tab->file_cancellable);
	g_clear_object(&tab->monitor);
	g_clear_object(&tab->next_content);
	g_clear_object(&tab->editor_search);
	g_clear_weak_pointer(&tab->find_buffer);
	g_array_unref(tab->back_places);
	g_array_unref(tab->forward_places);
	g_free(tab->source);
	g_free(tab->disk_text);
	g_free(tab->etag);
	g_free(tab->charset);
	g_free(tab->pending_anchor);
	clear_render_context(&tab->context);
	clear_render_context(&tab->next_context);
	g_object_unref(tab->file);
	g_free(tab);
}


/* Stops everything that could still report to the window once the tab has left it */
void shut_down_tab(Tab *tab) {
	if (tab->is_closed)
		return;
	tab->is_closed = TRUE;
	g_clear_pointer(&tab->stream, stop_stream);
	g_cancellable_cancel(tab->file_cancellable);
	g_signal_handlers_disconnect_by_data(adw_style_manager_get_default(), tab);
	if (tab->monitor) {
		g_signal_handlers_disconnect_by_data(tab->monitor, tab);
		g_file_monitor_cancel(tab->monitor);
	}
}


static void activate_link(const char *link, gpointer data);


/* Finds the link under a point of the view, if there is one */
static GtkTextTag *find_link_tag(GtkTextView *view, double x, double y) {
	int buffer_x = 0, buffer_y = 0;
	gtk_text_view_window_to_buffer_coords(view, GTK_TEXT_WINDOW_WIDGET, (int)x, (int)y, &buffer_x, &buffer_y);
	GtkTextIter iter;
	if (!gtk_text_view_get_iter_at_location(view, &iter, buffer_x, buffer_y))
		return NULL;
	g_autoptr(GSList) tags = gtk_text_iter_get_tags(&iter);
	for (GSList *item = tags; item; item = item->next)
		if (g_object_get_data(item->data, "link"))
			return item->data;
	return NULL;
}


/* A click opens a link only when it did not end a text selection */
static void click_view(GtkGestureClick *click, int presses, double x, double y, Tab *tab) {
	GtkTextView *view = GTK_TEXT_VIEW(gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(click)));
	GtkTextTag *link = find_link_tag(view, x, y);
	if (link && !gtk_text_buffer_get_has_selection(gtk_text_view_get_buffer(view)))
		activate_link(g_object_get_data(G_OBJECT(link), "link"), tab);
}


static void hover_view(GtkEventControllerMotion *motion, double x, double y, gpointer data) {
	GtkWidget *view = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(motion));
	gtk_widget_set_cursor_from_name(view, find_link_tag(GTK_TEXT_VIEW(view), x, y) ? "pointer" : "text");
}


static gboolean tell_link_address(GtkWidget *view, int x, int y, gboolean is_keyboard, GtkTooltip *tooltip,
	gpointer data) {
	GtkTextTag *link = find_link_tag(GTK_TEXT_VIEW(view), x, y);
	if (link)
		gtk_tooltip_set_text(tooltip, g_object_get_data(G_OBJECT(link), "title"));
	return link != NULL;
}


static GtkWidget *create_content_view(Tab *tab) {
	GtkWidget *view = create_document_view();
	GtkGesture *click = gtk_gesture_click_new();
	gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_PRIMARY);
	g_signal_connect(click, "released", G_CALLBACK(click_view), tab);
	gtk_widget_add_controller(view, GTK_EVENT_CONTROLLER(click));
	GtkEventController *motion = gtk_event_controller_motion_new();
	g_signal_connect(motion, "motion", G_CALLBACK(hover_view), NULL);
	gtk_widget_add_controller(view, motion);
	gtk_widget_set_has_tooltip(view, TRUE);
	g_signal_connect(view, "query-tooltip", G_CALLBACK(tell_link_address), NULL);
	add_block_menu(tab, view);
	return view;
}


static void fill_editor(Tab *tab) {
	g_autofree char *name = get_tab_title(tab);
	prepare_source_view(GTK_SOURCE_VIEW(tab->editor), name, NULL);
	GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tab->editor));
	gtk_text_buffer_begin_irreversible_action(buffer);
	gtk_text_buffer_set_text(buffer, tab->source ? tab->source : "", -1);
	gtk_text_buffer_end_irreversible_action(buffer);
	GtkTextIter start;
	gtk_text_buffer_get_start_iter(buffer, &start);
	gtk_text_buffer_place_cursor(buffer, &start);
	gtk_text_buffer_set_modified(buffer, FALSE);
	tab->is_editor_filled = TRUE;
}


static char *get_editor_text(Tab *tab) {
	GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tab->editor));
	GtkTextIter start, end;
	gtk_text_buffer_get_bounds(buffer, &start, &end);
	return get_joined_text(buffer, &start, &end);
}


void mark_tab_read_only(Tab *tab) {
	if (tab->is_read_only)
		return;
	tab->is_read_only = TRUE;
	tab->events.show_message(tab, TEXT_READ_ONLY, tab->events.data);
	tab->events.state_changed(tab, tab->events.data);
}


static void take_source(char *source, const SourceInfo *info, gpointer data) {
	Tab *tab = data;
	if (info->etag)
		g_set_str(&tab->etag, info->etag);
	if (!tab->disk_text) {
		tab->disk_text = g_strdup(source);
		tab->charset = g_strdup(info->charset);
	}
	g_free(tab->source);
	tab->source = source;
	if (!info->is_exact)
		mark_tab_read_only(tab);
	tab->has_contents = info->kind == DOCUMENT_MARKDOWN || info->kind == DOCUMENT_TASKS;
	if (info->kind == DOCUMENT_SOURCE && !tab->is_code)
		start_code_editor(tab, info->language);
	else if (tab->is_editing && !tab->is_editor_filled)
		fill_editor(tab);
	tab->events.state_changed(tab, tab->events.data);
	if (tab->is_editing_requested) {
		tab->is_editing_requested = FALSE;
		set_tab_editing(tab, TRUE);
	}
}


static const char *get_view_page(Tab *tab) {
	return tab->data_table ? "table" : "view";
}


static void show_view(Tab *tab) {
	if (g_strcmp0(gtk_stack_get_visible_child_name(GTK_STACK(tab->root)), "loading") != 0)
		return;
	gtk_stack_set_visible_child_name(GTK_STACK(tab->root), get_view_page(tab));
	note_startup_moment("Document shown");
}


static void add_block(const Block *block, gpointer data) {
	Tab *tab = data;
	if (block->kind == BLOCK_SOURCE) {
		add_code_block(tab, block);
		return;
	}
	if (block->kind == BLOCK_TABLE && block->checked) {
		if (!tab->data_table) {
			tab->data_table = create_data_table();
			gtk_stack_add_named(GTK_STACK(tab->root), tab->data_table, "table");
		}
		add_data_rows(tab->data_table, block);
		show_view(tab);
		return;
	}
	gboolean is_offscreen = tab->next_content != NULL;
	render_block(is_offscreen ? &tab->next_context : &tab->context, block);
	if (is_offscreen)
		return;
	show_view(tab);
	if (block->kind == BLOCK_HEADING)
		tab->events.outline_changed(tab, tab->events.data);
}


/* Puts newly rendered content and its context in place and keeps the reader where they were */
static void swap_content(Tab *tab) {
	int line = tab->pending_line ? tab->pending_line : get_view_line(tab);
	tab->content = tab->next_content;
	tab->next_content = NULL;
	adw_clamp_scrollable_set_child(ADW_CLAMP_SCROLLABLE(tab->clamp), tab->content);
	g_object_unref(tab->content);
	swap_render_contexts(&tab->context, &tab->next_context);
	tab->outline_generation++;
	tab->events.outline_changed(tab, tab->events.data);
	request_view_line(tab, line);
}


static void finish_loading(const char *error, gpointer data) {
	Tab *tab = data;
	g_clear_pointer(&tab->stream, stop_stream);
	if (tab->is_code)
		finish_code_loading(tab);
	if (tab->next_content) {
		swap_content(tab);
		return;
	}
	GtkTextBuffer *shown = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tab->content));
	if (error && !tab->is_code && gtk_text_buffer_get_char_count(shown) == 0) {
		adw_status_page_set_description(ADW_STATUS_PAGE(tab->error_page), error);
		gtk_stack_set_visible_child_name(GTK_STACK(tab->root), "error");
		return;
	}
	if (!tab->is_code)
		show_view(tab);
	if (tab->pending_anchor) {
		g_autofree char *anchor = g_steal_pointer(&tab->pending_anchor);
		show_tab_anchor(tab, anchor);
	}
}


static void start_rendering(Tab *tab, const char *text) {
	static const StreamEvents events = {take_source, add_block, finish_loading, NULL};
	StreamEvents tab_events = events;
	tab_events.data = tab;
	if (tab->is_code || tab->is_closed)
		return;
	stop_stream(tab->stream);
	g_clear_object(&tab->next_content);
	if (tab->data_table) {
		clear_data_table(tab->data_table);
	} else if (text) {
		tab->next_content = g_object_ref_sink(create_content_view(tab));
		reset_render_context(&tab->next_context, GTK_TEXT_VIEW(tab->next_content));
	} else {
		reset_render_context(&tab->context, GTK_TEXT_VIEW(tab->content));
		tab->outline_generation++;
	}
	g_autofree char *name = get_tab_title(tab);
	tab->stream = start_stream(text ? NULL : tab->file, text, name, &tab_events);
}


/* Code colours are copied from the style scheme, so a light or dark switch renders the document again */
static void follow_dark_style(AdwStyleManager *styles, GParamSpec *property, Tab *tab) {
	if (tab->source && !tab->stream)
		start_rendering(tab, tab->source);
}


/* Renders a new text in place, used after editing and when the file changes on disk */
static void render_again(Tab *tab, const char *text) {
	if (g_strcmp0(text, tab->source) == 0)
		return;
	g_free(tab->source);
	tab->source = g_strdup(text);
	start_rendering(tab, text);
}


static void reload_from_disk(GObject *source, GAsyncResult *result, gpointer data) {
	g_autofree char *bytes = NULL;
	g_autofree char *etag = NULL;
	gsize length = 0;
	if (!g_file_load_contents_finish(G_FILE(source), result, &bytes, &length, &etag, NULL))
		return;
	Tab *tab = data;
	g_autofree char *charset = NULL;
	char *contents = decode_text(bytes, length, &charset);
	if (!contents || g_strcmp0(charset, tab->charset) != 0) {
		/* A different charset than the tab writes would change the file on the next save */
		g_free(contents);
		return;
	}
	gboolean is_same = g_strcmp0(contents, tab->disk_text) == 0;
	if (is_same || is_tab_modified(tab) || tab->is_read_only) {
		if (is_same)
			g_set_str(&tab->etag, etag);
		g_free(contents);
		return;
	}
	g_set_str(&tab->etag, etag);
	gboolean is_growth = tab->is_code && tab->disk_text && g_str_has_prefix(contents, tab->disk_text);
	if (is_growth)
		append_code_text(tab, contents + strlen(tab->disk_text));
	g_free(tab->disk_text);
	tab->disk_text = contents;
	if (is_growth)
		return;
	if (tab->is_editor_filled)
		fill_editor(tab);
	render_again(tab, contents);
}


static void notice_file_change(GFileMonitor *monitor, GFile *file, GFile *other, GFileMonitorEvent event, Tab *tab) {
	if (event == G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT || event == G_FILE_MONITOR_EVENT_CREATED)
		g_file_load_contents_async(tab->file, tab->file_cancellable, reload_from_disk, tab);
}


static void notice_buffer_change(GtkTextBuffer *buffer, Tab *tab) {
	if (!tab->is_closed)
		tab->events.state_changed(tab, tab->events.data);
}


static void activate_link(const char *link, gpointer data) {
	Tab *tab = data;
	if (link[0] == '#') {
		g_autofree char *anchor = g_uri_unescape_string(link + 1, NULL);
		scroll_tab_to_anchor(tab, anchor ? anchor : link + 1);
		return;
	}
	tab->events.open_link(tab, link, tab->events.data);
}


/* Centres a text view at the reading width, staying the scrolled child so only the part in sight is laid out */
static GtkWidget *create_page_clamp(GtkWidget *view) {
	GtkWidget *clamp = adw_clamp_scrollable_new();
	adw_clamp_scrollable_set_maximum_size(ADW_CLAMP_SCROLLABLE(clamp), CONTENT_WIDTH);
	adw_clamp_scrollable_set_tightening_threshold(ADW_CLAMP_SCROLLABLE(clamp), CONTENT_TIGHTENING);
	adw_clamp_scrollable_set_child(ADW_CLAMP_SCROLLABLE(clamp), view);
	return clamp;
}


/* Ctrl and the wheel zoom the page, which the shared smooth scrolling passes on untouched */
static gboolean zoom_with_wheel(GtkEventControllerScroll *controller, double delta_x, double delta_y,
	GtkWidget *scroller) {
	GdkEvent *event = gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(controller));
	if (!event || delta_y == 0 || !(gdk_event_get_modifier_state(event) & GDK_CONTROL_MASK))
		return GDK_EVENT_PROPAGATE;
	gtk_widget_activate_action(scroller, delta_y < 0 ? "win.zoom-in" : "win.zoom-out", NULL);
	return GDK_EVENT_STOP;
}


static GtkWidget *create_page_scroller(GtkWidget *child) {
	GtkWidget *scroller = gtk_scrolled_window_new();
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), child);
	gtk_widget_add_css_class(scroller, "document");
	add_smooth_scrolling(GTK_SCROLLED_WINDOW(scroller));
	GtkEventController *zoom = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
	gtk_event_controller_set_propagation_phase(zoom, GTK_PHASE_CAPTURE);
	g_signal_connect(zoom, "scroll", G_CALLBACK(zoom_with_wheel), scroller);
	gtk_widget_add_controller(scroller, zoom);
	return scroller;
}


static GtkWidget *create_view_page(Tab *tab) {
	tab->content = create_content_view(tab);
	tab->clamp = create_page_clamp(tab->content);
	tab->scroller = create_page_scroller(tab->clamp);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(tab->scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	return add_block_edit(tab);
}


static GtkWidget *create_edit_page(Tab *tab) {
	tab->editor = create_source_view();
	gtk_widget_add_css_class(tab->editor, "editor");
	gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(tab->editor), GTK_WRAP_WORD_CHAR);
	gtk_text_view_set_top_margin(GTK_TEXT_VIEW(tab->editor), EDITOR_MARGIN);
	gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(tab->editor), EDITOR_MARGIN);
	gtk_text_view_set_left_margin(GTK_TEXT_VIEW(tab->editor), EDITOR_MARGIN);
	gtk_text_view_set_right_margin(GTK_TEXT_VIEW(tab->editor), EDITOR_MARGIN);
	GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tab->editor));
	g_signal_connect(buffer, "modified-changed", G_CALLBACK(notice_buffer_change), tab);
	/* Documents are edited at the same width they are read at */
	tab->editor_clamp = create_page_clamp(tab->editor);
	return create_page_scroller(tab->editor_clamp);
}


static void set_up_context(RenderContext *context, GFile *file, Tab *tab) {
	g_autoptr(GFile) parent = g_file_get_parent(file);
	context->base_directory = parent ? g_file_get_path(parent) : NULL;
	context->activate_link = activate_link;
	context->toggle_task = toggle_task;
	context->data = tab;
}


Tab *create_tab(GFile *file, const TabEvents *events) {
	Tab *tab = g_new0(Tab, 1);
	tab->events = *events;
	tab->file = g_object_ref(file);
	tab->file_cancellable = g_cancellable_new();
	tab->back_places = g_array_new(FALSE, FALSE, sizeof(Place));
	tab->forward_places = g_array_new(FALSE, FALSE, sizeof(Place));
	set_up_context(&tab->context, file, tab);
	set_up_context(&tab->next_context, file, tab);
	tab->root = gtk_stack_new();
	gtk_stack_set_transition_type(GTK_STACK(tab->root), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
	gtk_stack_set_transition_duration(GTK_STACK(tab->root), STACK_DURATION);
	gtk_stack_add_named(GTK_STACK(tab->root), create_loading_spinner(), "loading");
	gtk_stack_add_named(GTK_STACK(tab->root), create_view_page(tab), "view");
	gtk_stack_add_named(GTK_STACK(tab->root), create_edit_page(tab), "edit");
	tab->error_page = adw_status_page_new();
	adw_status_page_set_icon_name(ADW_STATUS_PAGE(tab->error_page), "dialog-error-symbolic");
	adw_status_page_set_title(ADW_STATUS_PAGE(tab->error_page), TEXT_LOAD_FAILED);
	gtk_stack_add_named(GTK_STACK(tab->root), tab->error_page, "error");
	gtk_stack_set_visible_child_name(GTK_STACK(tab->root), "loading");
	g_object_set_data_full(G_OBJECT(tab->root), "tab", tab, free_tab);
	g_signal_connect_swapped(tab->root, "destroy", G_CALLBACK(shut_down_tab), tab);
	start_rendering(tab, NULL);
	g_signal_connect(adw_style_manager_get_default(), "notify::dark", G_CALLBACK(follow_dark_style), tab);
	tab->monitor = g_file_monitor_file(file, G_FILE_MONITOR_NONE, NULL, NULL);
	if (tab->monitor)
		g_signal_connect(tab->monitor, "changed", G_CALLBACK(notice_file_change), tab);
	return tab;
}


Tab *get_tab(GtkWidget *root) {
	return root ? g_object_get_data(G_OBJECT(root), "tab") : NULL;
}


char *get_tab_title(Tab *tab) {
	return g_file_get_basename(tab->file);
}


gboolean is_tab_modified(Tab *tab) {
	return tab->is_editor_filled && gtk_text_buffer_get_modified(gtk_text_view_get_buffer(GTK_TEXT_VIEW(tab->editor)));
}


/* Code files never leave the editor, and editing waits until the text is known and safe to save */
gboolean can_switch_tab_editing(Tab *tab) {
	return tab->source && !tab->is_code && !tab->is_read_only;
}


/* Switches between view and editor, keeping the same part of the document in front of the reader */
void change_tab_editing(Tab *tab, gboolean is_editing, int line) {
	if (tab->is_editing == is_editing || !can_switch_tab_editing(tab))
		return;
	tab->is_editing = is_editing;
	if (is_editing) {
		if (!tab->is_editor_filled)
			fill_editor(tab);
		gtk_stack_set_visible_child_full(GTK_STACK(tab->root), "edit", GTK_STACK_TRANSITION_TYPE_SLIDE_LEFT);
		gtk_widget_grab_focus(tab->editor);
		scroll_editor_to_line(tab, line > 0 ? line : get_view_line(tab));
	} else {
		int editor_line = tab->is_editor_filled ? get_editor_line(tab) : 0;
		if (tab->is_editor_filled) {
			g_autofree char *text = get_editor_text(tab);
			render_again(tab, text);
		}
		gtk_stack_set_visible_child_full(GTK_STACK(tab->root), get_view_page(tab), GTK_STACK_TRANSITION_TYPE_SLIDE_RIGHT);
		request_view_line(tab, line > 0 ? line : editor_line);
	}
	tab->events.state_changed(tab, tab->events.data);
}


/* Opens the editor now, or as soon as the text is known and safe to edit */
void request_tab_editing(Tab *tab) {
	tab->is_editing_requested = !can_switch_tab_editing(tab) && !tab->is_code;
	if (!tab->is_editing_requested)
		set_tab_editing(tab, TRUE);
}


void set_tab_editing(Tab *tab, gboolean is_editing) {
	if (tab->is_editing != is_editing && can_switch_tab_editing(tab))
		remember_place(tab);
	change_tab_editing(tab, is_editing, 0);
}


/* Ends a tab close that waited for its save, keeping the tab open when the save failed */
static void finish_waiting_close(Tab *tab, gboolean is_saved) {
	if (!tab->is_closing_after_save)
		return;
	tab->is_closing_after_save = FALSE;
	GtkWidget *view = gtk_widget_get_ancestor(tab->root, ADW_TYPE_TAB_VIEW);
	if (!view)
		return;
	if (is_saved)
		shut_down_tab(tab);
	adw_tab_view_close_page_finish(ADW_TAB_VIEW(view), tab->page, is_saved);
}


static void finish_saving(GObject *source, GAsyncResult *result, gpointer data) {
	g_autoptr(GtkWidget) root = data;
	g_autoptr(GError) error = NULL;
	g_autofree char *etag = NULL;
	gboolean is_saved = g_file_replace_contents_finish(G_FILE(source), result, &etag, &error);
	g_application_release(g_application_get_default());
	Tab *tab = get_tab(root);
	if (is_saved)
		g_set_str(&tab->etag, etag);
	if (!is_saved && tab->is_closed) {
		g_warning("Saving %s failed: %s", g_file_peek_path(G_FILE(source)), error->message);
		return;
	}
	if (!is_saved) {
		gboolean is_changed_on_disk = g_error_matches(error, G_IO_ERROR, G_IO_ERROR_WRONG_ETAG);
		g_autofree char *message = is_changed_on_disk ? g_strdup(TEXT_CHANGED_ON_DISK)
			: g_strdup_printf(TEXT_SAVE_FAILED, error->message);
		if (is_changed_on_disk)
			g_clear_pointer(&tab->etag, g_free);
		gtk_text_buffer_set_modified(gtk_text_view_get_buffer(GTK_TEXT_VIEW(tab->editor)), TRUE);
		tab->events.show_message(tab, message, tab->events.data);
	}
	finish_waiting_close(tab, is_saved);
}


/* Writes the text unless the file changed on disk since it was read, keeping the application alive meanwhile */
static void write_tab_text(Tab *tab, const char *text) {
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) bytes = encode_text(text, tab->charset, &error);
	if (!bytes) {
		g_autofree char *message = g_strdup_printf(TEXT_SAVE_FAILED, error->message);
		gtk_text_buffer_set_modified(gtk_text_view_get_buffer(GTK_TEXT_VIEW(tab->editor)), TRUE);
		tab->events.show_message(tab, message, tab->events.data);
		finish_waiting_close(tab, FALSE);
		return;
	}
	g_free(tab->disk_text);
	tab->disk_text = g_strdup(text);
	g_application_hold(g_application_get_default());
	g_file_replace_contents_bytes_async(tab->file, bytes, tab->etag, FALSE, G_FILE_CREATE_NONE, NULL, finish_saving,
		g_object_ref(tab->root));
}


void save_tab(Tab *tab) {
	if (!tab->is_editor_filled || tab->is_read_only) {
		finish_waiting_close(tab, !tab->is_read_only);
		return;
	}
	g_autofree char *text = get_editor_text(tab);
	gtk_text_buffer_set_modified(gtk_text_view_get_buffer(GTK_TEXT_VIEW(tab->editor)), FALSE);
	if (!tab->is_editing)
		render_again(tab, text);
	write_tab_text(tab, text);
}


void save_and_close_tab(Tab *tab) {
	tab->is_closing_after_save = TRUE;
	save_tab(tab);
}


/* Flips one task mark in the editable text, leaving the file alone until the reader saves */
static void toggle_task(long offset, gboolean is_done, gpointer data) {
	Tab *tab = data;
	gboolean is_valid = tab->source && !tab->is_read_only && offset >= 0 && (gsize)offset < strlen(tab->source)
		&& strchr(" xX", tab->source[offset]);
	if (!is_valid)
		return;
	if (!tab->is_editor_filled)
		fill_editor(tab);
	char mark = is_done ? 'x' : ' ';
	GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tab->editor));
	GtkTextIter start, end;
	gtk_text_buffer_get_iter_at_offset(buffer, &start, (int)g_utf8_pointer_to_offset(tab->source, tab->source + offset));
	end = start;
	gtk_text_iter_forward_char(&end);
	gtk_text_buffer_begin_user_action(buffer);
	gtk_text_buffer_delete(buffer, &start, &end);
	gtk_text_buffer_insert(buffer, &start, &mark, 1);
	gtk_text_buffer_end_user_action(buffer);
	tab->source[offset] = mark;
}


/* Shows a heading by its anchor now, or once the document has loaded */
void show_tab_anchor(Tab *tab, const char *anchor) {
	if (tab->stream || !scroll_tab_to_anchor(tab, anchor))
		g_set_str(&tab->pending_anchor, tab->stream ? anchor : NULL);
}
