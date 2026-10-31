#include <string.h>

#include "view/data-table.h"
#include "view/document-view.h"
#include "window/tab-places.h"
#include "window/tab-find.h"

#define FIND_TAG "find-match"
/* A match scrolled into view sits this far down the page */
#define MATCH_VIEW_POSITION (1 / 3.0)
/* Room kept around the selected match in the editor, as a share of the view */
#define EDITOR_MATCH_MARGIN 0.1
/* The search match colours of the Adwaita style scheme the editor uses */
#define FIND_BACKGROUND "#f8e45c"
#define FIND_FOREGROUND "#3d3846"

/* One place where the search text appears, in characters of a label or a text view */
typedef struct {
	GtkWidget *widget;
	int start, end;
	/* Position in the document, from the match itself or from the embedded block holding it */
	int order, position;
} FindMatch;

/* What a search over the view collects into */
typedef struct {
	const char *query;
	const gunichar *characters;
	glong length;
	GArray *matches;
	int position;
} FindSearch;


/* Lowercases one character at a time, so match positions stay character positions of the original text */
static gunichar *lower_characters(const char *text, glong *length) {
	gunichar *characters = g_utf8_to_ucs4_fast(text, -1, length);
	for (glong index = 0; index < *length; index++)
		characters[index] = g_unichar_tolower(characters[index]);
	return characters;
}


static void add_match(FindSearch *search, GtkWidget *widget, int start, int end, int order) {
	FindMatch match = {widget, start, end, order, search->position++};
	g_array_append_val(search->matches, match);
}


static void collect_label_matches(FindSearch *search, GtkWidget *label, int order) {
	glong length = 0;
	g_autofree gunichar *text = lower_characters(gtk_label_get_text(GTK_LABEL(label)), &length);
	for (glong start = 0; start + search->length <= length; start++)
		if (memcmp(text + start, search->characters, search->length * sizeof(gunichar)) == 0)
			add_match(search, label, (int)start, (int)(start + search->length), order);
}


/* Skips collapsed sections and treats embedded blocks as absent, so a match may run across them */
static void collect_text_matches(FindSearch *search, GtkWidget *view, int order) {
	GtkTextIter cursor, match_start, match_end;
	gtk_text_buffer_get_start_iter(gtk_text_view_get_buffer(GTK_TEXT_VIEW(view)), &cursor);
	GtkTextSearchFlags flags = GTK_TEXT_SEARCH_CASE_INSENSITIVE | GTK_TEXT_SEARCH_VISIBLE_ONLY
		| GTK_TEXT_SEARCH_TEXT_ONLY;
	while (gtk_text_iter_forward_search(&cursor, search->query, flags, &match_start, &match_end, NULL)) {
		int start = gtk_text_iter_get_offset(&match_start);
		add_match(search, view, start, gtk_text_iter_get_offset(&match_end), order < 0 ? start : order);
		cursor = match_end;
	}
}


/* Searches the blocks embedded in the view, ordered by where each one is in its text */
static void collect_embedded_matches(FindSearch *search, GtkWidget *widget, GtkTextView *document, int order) {
	GtkTextChildAnchor *anchor = g_object_get_data(G_OBJECT(widget), "anchor");
	if (anchor) {
		GtkTextIter iter;
		gtk_text_buffer_get_iter_at_child_anchor(gtk_text_view_get_buffer(document), &iter, anchor);
		if (is_text_hidden(&iter))
			return;
		order = gtk_text_iter_get_offset(&iter);
	}
	if (order >= 0 && GTK_IS_LABEL(widget) && gtk_label_get_selectable(GTK_LABEL(widget))) {
		collect_label_matches(search, widget, order);
		return;
	}
	if (order >= 0 && GTK_IS_TEXT_VIEW(widget)) {
		collect_text_matches(search, widget, order);
		return;
	}
	for (GtkWidget *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child))
		collect_embedded_matches(search, child, document, order);
}


static int compare_matches(gconstpointer first, gconstpointer second) {
	const FindMatch *one = first, *other = second;
	return one->order != other->order ? one->order - other->order : one->position - other->position;
}


/* Top of a match in the view's buffer coordinates */
static double get_match_y(Tab *tab, FindMatch *match, double *height) {
	GtkTextView *document = GTK_TEXT_VIEW(tab->content);
	double y = 0;
	*height = gtk_widget_get_height(match->widget);
	if (GTK_IS_TEXT_VIEW(match->widget)) {
		GtkTextIter iter;
		GdkRectangle location;
		GtkTextView *view = GTK_TEXT_VIEW(match->widget);
		gtk_text_buffer_get_iter_at_offset(gtk_text_view_get_buffer(view), &iter, match->start);
		gtk_text_view_get_iter_location(view, &iter, &location);
		*height = location.height;
		if (view == document)
			return location.y;
		int x = 0, top = 0;
		gtk_text_view_buffer_to_window_coords(view, GTK_TEXT_WINDOW_WIDGET, location.x, location.y, &x, &top);
		y = top;
	}
	graphene_point_t point;
	if (!gtk_widget_compute_point(match->widget, GTK_WIDGET(document), &GRAPHENE_POINT_INIT(0, y), &point))
		return 0;
	int buffer_x = 0, buffer_y = 0;
	gtk_text_view_window_to_buffer_coords(document, GTK_TEXT_WINDOW_WIDGET, (int)point.x, (int)point.y, &buffer_x,
		&buffer_y);
	return buffer_y;
}


/* Index of the first match at or below the top of the view, so a new search starts where the reader is */
static int get_visible_match(Tab *tab, GArray *matches) {
	GdkRectangle visible;
	gtk_text_view_get_visible_rect(GTK_TEXT_VIEW(tab->content), &visible);
	for (guint index = 0; index < matches->len; index++) {
		double height = 0;
		double y = get_match_y(tab, &g_array_index(matches, FindMatch, index), &height);
		if (y + height >= visible.y)
			return (int)index;
	}
	return 0;
}


static void clear_match_mark(Tab *tab) {
	if (!tab->find_buffer)
		return;
	GtkTextIter start, end;
	gtk_text_buffer_get_bounds(tab->find_buffer, &start, &end);
	gtk_text_buffer_remove_tag_by_name(tab->find_buffer, FIND_TAG, &start, &end);
	g_clear_weak_pointer(&tab->find_buffer);
}


/* Text views draw no selection while the find entry has focus, so the match is also marked with a tag */
static void mark_match(Tab *tab, GtkTextBuffer *buffer, const GtkTextIter *start, const GtkTextIter *end) {
	if (!gtk_text_tag_table_lookup(gtk_text_buffer_get_tag_table(buffer), FIND_TAG))
		gtk_text_buffer_create_tag(buffer, FIND_TAG, "background", FIND_BACKGROUND, "foreground", FIND_FOREGROUND,
			NULL);
	gtk_text_buffer_apply_tag_by_name(buffer, FIND_TAG, start, end);
	g_set_weak_pointer(&tab->find_buffer, buffer);
}


static void select_match(Tab *tab, FindMatch *match) {
	clear_match_mark(tab);
	if (GTK_IS_LABEL(match->widget)) {
		gtk_label_select_region(GTK_LABEL(match->widget), match->start, match->end);
		return;
	}
	GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(match->widget));
	GtkTextIter start, end;
	gtk_text_buffer_get_iter_at_offset(buffer, &start, match->start);
	gtk_text_buffer_get_iter_at_offset(buffer, &end, match->end);
	gtk_text_buffer_select_range(buffer, &start, &end);
	mark_match(tab, buffer, &start, &end);
}


static int find_in_view(Tab *tab, const char *query, FindDirection direction, int *total) {
	glong length = 0;
	g_autofree gunichar *characters = lower_characters(query, &length);
	g_autoptr(GArray) matches = g_array_new(FALSE, FALSE, sizeof(FindMatch));
	FindSearch search = {query, characters, length, matches, 0};
	collect_text_matches(&search, tab->content, -1);
	collect_embedded_matches(&search, tab->content, GTK_TEXT_VIEW(tab->content), -1);
	g_array_sort(matches, compare_matches);
	*total = (int)matches->len;
	if (matches->len == 0)
		return 0;
	int count = (int)matches->len;
	int current = direction == FIND_FIRST ? get_visible_match(tab, matches)
		: direction == FIND_SAME ? CLAMP(tab->find_index, 0, count - 1)
		: (tab->find_index + (direction == FIND_NEXT ? 1 : -1) + count) % count;
	tab->find_index = current;
	FindMatch *match = &g_array_index(matches, FindMatch, current);
	select_match(tab, match);
	double height = 0;
	double y = get_match_y(tab, match, &height);
	GdkRectangle visible;
	gtk_text_view_get_visible_rect(GTK_TEXT_VIEW(tab->content), &visible);
	if (y < visible.y || y + height > visible.y + visible.height) {
		GtkAdjustment *adjustment = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(tab->scroller));
		double value = y + gtk_adjustment_get_value(adjustment) - visible.y - visible.height * MATCH_VIEW_POSITION;
		glide_scroller(GTK_SCROLLED_WINDOW(tab->scroller), value, FIND_GLIDE_DURATION);
	}
	return current + 1;
}


static void report_find_count(Tab *tab) {
	if (!tab->is_closed)
		tab->events.find_counted(tab, tab->events.data);
}


static GtkSourceSearchContext *get_editor_search(Tab *tab) {
	if (tab->editor_search)
		return tab->editor_search;
	g_autoptr(GtkSourceSearchSettings) settings = gtk_source_search_settings_new();
	gtk_source_search_settings_set_wrap_around(settings, TRUE);
	GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tab->editor));
	tab->editor_search = gtk_source_search_context_new(GTK_SOURCE_BUFFER(buffer), settings);
	/* Matches are counted in the background, so the count comes after the first match is shown */
	g_signal_connect_swapped(tab->editor_search, "notify::occurrences-count", G_CALLBACK(report_find_count), tab);
	return tab->editor_search;
}


static int find_in_editor(Tab *tab, const char *query, FindDirection direction, int *total) {
	GtkSourceSearchContext *search = get_editor_search(tab);
	GtkSourceSearchSettings *settings = gtk_source_search_context_get_settings(search);
	if (g_strcmp0(gtk_source_search_settings_get_search_text(settings), query) != 0)
		gtk_source_search_settings_set_search_text(settings, query);
	gtk_source_search_context_set_highlight(search, TRUE);
	GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tab->editor));
	GtkTextIter selection_start, selection_end, match_start = {0}, match_end = {0};
	gtk_text_buffer_get_selection_bounds(buffer, &selection_start, &selection_end);
	gboolean is_found = FALSE;
	if (direction == FIND_SAME) {
		match_start = selection_start;
		match_end = selection_end;
		is_found = TRUE;
	} else if (direction == FIND_PREVIOUS) {
		is_found = gtk_source_search_context_backward(search, &selection_start, &match_start, &match_end, NULL);
	} else {
		is_found = gtk_source_search_context_forward(search, direction == FIND_FIRST ? &selection_start
			: &selection_end, &match_start, &match_end, NULL);
	}
	*total = gtk_source_search_context_get_occurrences_count(search);
	if (!is_found)
		return 0;
	if (direction != FIND_SAME) {
		gtk_text_buffer_select_range(buffer, &match_start, &match_end);
		GtkTextMark *cursor = gtk_text_buffer_get_insert(buffer);
		gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(tab->editor), cursor, EDITOR_MATCH_MARGIN, FALSE, 0, 0);
	}
	return MAX(gtk_source_search_context_get_occurrence_position(search, &match_start, &match_end), 0);
}


/* Takes the match marks out of the editor once the find bar closes */
void stop_find_in_tab(Tab *tab) {
	clear_match_mark(tab);
	if (tab->editor_search)
		gtk_source_search_settings_set_search_text(gtk_source_search_context_get_settings(tab->editor_search), NULL);
}


/* Selects the next match and returns its number, counting from one, with zero when nothing matches */
int find_in_tab(Tab *tab, const char *query, FindDirection direction, int *total) {
	*total = 0;
	if (!query || !*query) {
		stop_find_in_tab(tab);
		return 0;
	}
	if (tab->is_editing)
		return find_in_editor(tab, query, direction, total);
	if (tab->data_table) {
		int step = direction == FIND_NEXT ? 1 : direction == FIND_PREVIOUS ? -1 : 0;
		if (direction == FIND_FIRST)
			tab->find_index = 0;
		return find_in_data_table(tab->data_table, query, step, &tab->find_index, total);
	}
	return find_in_view(tab, query, direction, total);
}
