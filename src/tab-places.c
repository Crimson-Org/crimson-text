#include "../lang/en.h"
#include <string.h>

#include "document-view.h"
#include "common/long-lines.h"
#include "settings.h"
#include "common/smooth-scroll.h"
#include "tab-places.h"

#define SCROLL_DURATION 360
#define SCROLL_MARGIN 12
#define BLOCK_EDIT_SIZE 34
#define BLOCK_EDIT_GAP 6
/* Pixels a block may start above the view and still count as starting in it */
#define VIEW_LINE_SLACK 8

static GtkTextView *get_view(Tab *tab) {
	return GTK_TEXT_VIEW(tab->content);
}


static GtkAdjustment *get_view_adjustment(Tab *tab) {
	return gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(tab->scroller));
}


static int get_mark_y(Tab *tab, GtkTextMark *mark) {
	GtkTextIter iter;
	GdkRectangle location;
	gtk_text_buffer_get_iter_at_mark(gtk_text_view_get_buffer(get_view(tab)), &iter, mark);
	gtk_text_view_get_iter_location(get_view(tab), &iter, &location);
	return location.y;
}


/* Scroll position that puts a buffer height at the top, since the scroll range also holds the top margin */
static double get_scroll_value(Tab *tab, int buffer_y) {
	GdkRectangle visible;
	gtk_text_view_get_visible_rect(get_view(tab), &visible);
	return buffer_y + gtk_adjustment_get_value(get_view_adjustment(tab)) - visible.y;
}


static BlockMark *get_block(Tab *tab, guint index) {
	return &g_array_index(tab->context.blocks, BlockMark, index);
}


/* The first block that starts in view, unless the block covering the top fills most of the page */
int get_view_line(Tab *tab) {
	GdkRectangle visible;
	gtk_text_view_get_visible_rect(get_view(tab), &visible);
	int covering_line = 0;
	for (guint index = 0; index < tab->context.blocks->len; index++) {
		BlockMark *block = get_block(tab, index);
		int y = get_mark_y(tab, block->mark);
		if (y >= visible.y - VIEW_LINE_SLACK)
			return covering_line && y > visible.y + visible.height / 2 ? covering_line : block->line;
		covering_line = block->line;
	}
	return covering_line;
}


/* Last block starting at or before the line, so a line inside a block finds that block */
static GtkTextMark *find_line_mark(Tab *tab, int line) {
	GtkTextMark *found = NULL;
	for (guint index = 0; index < tab->context.blocks->len; index++) {
		BlockMark *block = get_block(tab, index);
		if (block->line > line && found)
			break;
		found = block->mark;
	}
	return found;
}


static gboolean get_view_offset(Tab *tab, int line, double *offset) {
	GtkTextMark *mark = find_line_mark(tab, line);
	if (!mark)
		return FALSE;
	*offset = get_scroll_value(tab, get_mark_y(tab, mark)) - SCROLL_MARGIN;
	return TRUE;
}


static void scroll_view_to_line(Tab *tab, int line) {
	double offset = 0;
	if (!get_view_offset(tab, line, &offset))
		return;
	GtkAdjustment *adjustment = get_view_adjustment(tab);
	double maximum = gtk_adjustment_get_upper(adjustment) - gtk_adjustment_get_page_size(adjustment);
	gtk_adjustment_set_value(adjustment, CLAMP(offset, 0, MAX(maximum, 0)));
}


/* Waits, frame by frame, until the view is shown with a scroll range before it moves */
static gboolean land_view(GtkWidget *root, GdkFrameClock *clock, gpointer data) {
	Tab *tab = get_tab(root);
	/* A view coming back from hiding has no scroll range until its first layout, which would clamp the move */
	gboolean is_laid_out = gtk_widget_get_mapped(tab->scroller)
		&& gtk_adjustment_get_page_size(get_view_adjustment(tab)) > 0;
	if (tab->next_content || !is_laid_out)
		return G_SOURCE_CONTINUE;
	scroll_view_to_line(tab, tab->pending_line);
	tab->pending_line = 0;
	tab->landing_handler = 0;
	return G_SOURCE_REMOVE;
}


void request_view_line(Tab *tab, int line) {
	if (line <= 0)
		return;
	tab->pending_line = line;
	if (!tab->landing_handler)
		tab->landing_handler = gtk_widget_add_tick_callback(tab->root, land_view, NULL, NULL);
}


static void hide_block_edit(Tab *tab) {
	gtk_widget_remove_css_class(tab->block_edit, "shown");
	gtk_widget_set_can_target(tab->block_edit, FALSE);
	tab->hover_line = 0;
}


static void place_block_edit(Tab *tab, int buffer_y, int row_height) {
	GtkTextView *view = get_view(tab);
	int x = 0, y = 0;
	gtk_text_view_buffer_to_window_coords(view, GTK_TEXT_WINDOW_WIDGET, DOCUMENT_MARGIN, buffer_y, &x, &y);
	graphene_point_t place;
	if (!gtk_widget_compute_point(GTK_WIDGET(view), tab->overlay, &GRAPHENE_POINT_INIT(x, y), &place))
		return;
	gtk_widget_set_margin_start(tab->block_edit, (int)MAX(place.x - BLOCK_EDIT_SIZE - BLOCK_EDIT_GAP, BLOCK_EDIT_GAP));
	gtk_widget_set_margin_top(tab->block_edit, (int)MAX(place.y + (row_height - BLOCK_EDIT_SIZE) / 2, 0));
	gtk_widget_add_css_class(tab->block_edit, "shown");
	gtk_widget_set_can_target(tab->block_edit, TRUE);
}


/* Index of the last block starting at or before a buffer position */
static int find_block_at(Tab *tab, const GtkTextIter *iter) {
	GtkTextBuffer *buffer = gtk_text_view_get_buffer(get_view(tab));
	int found = -1;
	for (guint index = 0; index < tab->context.blocks->len; index++) {
		GtkTextIter start;
		gtk_text_buffer_get_iter_at_mark(buffer, &start, get_block(tab, index)->mark);
		if (gtk_text_iter_compare(&start, iter) > 0)
			break;
		found = (int)index;
	}
	return found;
}


static void hide_code_copy(Tab *tab) {
	gtk_widget_remove_css_class(tab->code_copy, "shown");
	gtk_widget_set_can_target(tab->code_copy, FALSE);
	gtk_button_set_icon_name(GTK_BUTTON(tab->code_copy), "edit-copy-symbolic");
	tab->copy_start = tab->copy_end = -1;
}


static GtkTextTag *get_view_tag(Tab *tab, const char *name) {
	return gtk_text_tag_table_lookup(gtk_text_buffer_get_tag_table(gtk_text_view_get_buffer(get_view(tab))), name);
}


/* Finds the code block around a line and puts the copy button at its top right corner */
static void place_code_copy(Tab *tab, const GtkTextIter *line) {
	if (!line || !gtk_text_iter_has_tag(line, get_view_tag(tab, "code"))) {
		hide_code_copy(tab);
		return;
	}
	GtkTextIter start = *line, end = *line;
	gtk_text_iter_set_line_offset(&start, 0);
	while (!gtk_text_iter_has_tag(&start, get_view_tag(tab, "code-first")) && gtk_text_iter_backward_line(&start))
		continue;
	while (!gtk_text_iter_has_tag(&end, get_view_tag(tab, "code-last")) && gtk_text_iter_forward_line(&end))
		continue;
	gtk_text_iter_forward_to_line_end(&end);
	if (tab->copy_start != gtk_text_iter_get_offset(&start))
		gtk_button_set_icon_name(GTK_BUTTON(tab->code_copy), "edit-copy-symbolic");
	tab->copy_start = gtk_text_iter_get_offset(&start);
	tab->copy_end = gtk_text_iter_get_offset(&end);
	int top = 0, height = 0, x = 0, y = 0;
	gtk_text_view_get_line_yrange(get_view(tab), &start, &top, &height);
	int right = gtk_widget_get_width(GTK_WIDGET(get_view(tab))) - DOCUMENT_MARGIN - BLOCK_EDIT_SIZE - BLOCK_EDIT_GAP;
	gtk_text_view_buffer_to_window_coords(get_view(tab), GTK_TEXT_WINDOW_WIDGET, right, top + BLOCK_EDIT_GAP, &x, &y);
	graphene_point_t place;
	if (!gtk_widget_compute_point(GTK_WIDGET(get_view(tab)), tab->overlay, &GRAPHENE_POINT_INIT(x, y), &place))
		return;
	gtk_widget_set_margin_start(tab->code_copy, (int)MAX(place.x, 0));
	gtk_widget_set_margin_top(tab->code_copy, (int)MAX(place.y, 0));
	gtk_widget_add_css_class(tab->code_copy, "shown");
	gtk_widget_set_can_target(tab->code_copy, TRUE);
}


static void copy_hovered_code(GtkButton *button, Tab *tab) {
	GtkTextBuffer *buffer = gtk_text_view_get_buffer(get_view(tab));
	if (tab->copy_start < 0)
		return;
	GtkTextIter start, end;
	gtk_text_buffer_get_iter_at_offset(buffer, &start, tab->copy_start);
	gtk_text_buffer_get_iter_at_offset(buffer, &end, tab->copy_end);
	g_autofree char *text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
	gdk_clipboard_set_text(gtk_widget_get_clipboard(GTK_WIDGET(button)), text);
	gtk_button_set_icon_name(button, "object-select-symbolic");
}


/* Finds the view line at a point of a widget over the view, failing while the view is replaced */
static gboolean get_line_at_point(Tab *tab, GtkWidget *widget, double x, double y, GtkTextIter *line) {
	GtkTextView *view = get_view(tab);
	graphene_point_t point;
	if (tab->is_editing || tab->next_content
		|| !gtk_widget_compute_point(widget, GTK_WIDGET(view), &GRAPHENE_POINT_INIT(x, y), &point))
		return FALSE;
	int buffer_x = 0, buffer_y = 0;
	gtk_text_view_window_to_buffer_coords(view, GTK_TEXT_WINDOW_WIDGET, (int)point.x, (int)point.y, &buffer_x,
		&buffer_y);
	gtk_text_view_get_line_at_y(view, line, buffer_y, NULL);
	GtkTextIter exact;
	if (gtk_text_view_get_iter_at_location(view, &exact, buffer_x, buffer_y)
		&& gtk_text_iter_get_line(&exact) == gtk_text_iter_get_line(line))
		*line = exact;
	return TRUE;
}


/* Counts the line breaks a paragraph keeps between its start and a position, one per source line */
static int count_kept_breaks(Tab *tab, BlockMark *block, const GtkTextIter *position) {
	GtkTextIter start;
	gtk_text_buffer_get_iter_at_mark(gtk_text_view_get_buffer(get_view(tab)), &start, block->mark);
	g_autofree char *text = gtk_text_iter_get_text(&start, position);
	int count = 0;
	for (const char *found = strstr(text, LINE_SEPARATOR); found; found = strstr(found + 1, LINE_SEPARATOR))
		count++;
	return count;
}


/* A paragraph spans several source lines, so the word under the pointer finds its own line among them,
 * taking the one nearest the line the kept breaks point to when the word repeats */
static int find_source_line(Tab *tab, guint index, const GtkTextIter *position) {
	BlockMark *block = get_block(tab, index);
	GtkTextIter word_start = *position, word_end = *position;
	if (!tab->source || !gtk_text_iter_inside_word(position))
		return block->line;
	if (!gtk_text_iter_starts_word(&word_start))
		gtk_text_iter_backward_word_start(&word_start);
	gtk_text_iter_forward_word_end(&word_end);
	g_autofree char *word = gtk_text_iter_get_text(&word_start, &word_end);
	int last = index + 1 < tab->context.blocks->len ? get_block(tab, index + 1)->line - 1 : G_MAXINT;
	int estimate = block->line + count_kept_breaks(tab, block, position);
	int best = block->line, best_distance = G_MAXINT, number = 1;
	for (const char *line = tab->source; line && number <= last; number++) {
		const char *end = strchr(line, '\n');
		gsize length = end ? (gsize)(end - line) : strlen(line);
		if (number >= block->line && g_strstr_len(line, length, word) && ABS(number - estimate) < best_distance) {
			best = number;
			best_distance = ABS(number - estimate);
		}
		line = end ? end + 1 : NULL;
	}
	return best;
}


/* Finds the block under the pointer and puts the pencil beside its first line when Settings shows it */
static void place_pencil_at_pointer(Tab *tab) {
	GtkTextView *view = get_view(tab);
	GtkTextIter iter;
	gboolean has_position = tab->has_pointer && get_line_at_point(tab, tab->overlay, tab->pointer_x,
		tab->pointer_y, &iter);
	place_code_copy(tab, has_position ? &iter : NULL);
	int index = has_position && get_settings()->shows_edit_pencil ? find_block_at(tab, &iter) : -1;
	if (index < 0) {
		hide_block_edit(tab);
		return;
	}
	BlockMark *block = get_block(tab, (guint)index);
	GtkTextIter start;
	gtk_text_buffer_get_iter_at_mark(gtk_text_view_get_buffer(view), &start, block->mark);
	GdkRectangle location;
	gtk_text_view_get_iter_location(view, &start, &location);
	tab->hover_line = block->line;
	place_block_edit(tab, location.y, MIN(location.height, BLOCK_EDIT_SIZE));
}


static void follow_pointer(GtkEventControllerMotion *motion, double x, double y, Tab *tab) {
	tab->pointer_x = x;
	tab->pointer_y = y;
	tab->has_pointer = TRUE;
	place_pencil_at_pointer(tab);
}


static void leave_view(GtkEventControllerMotion *motion, Tab *tab) {
	tab->has_pointer = FALSE;
	hide_block_edit(tab);
	hide_code_copy(tab);
}


/* Opens the editor at one chosen block and flashes its first line, so the reader sees which text they picked */
static void edit_block(Tab *tab, int line) {
	remember_place(tab);
	change_tab_editing(tab, TRUE, line);
	if (line <= 0 || !tab->is_editing)
		return;
	GtkTextIter iter;
	get_iter_at_joined_line(gtk_text_view_get_buffer(GTK_TEXT_VIEW(tab->editor)), &iter, line - 1);
	flash_text_line(GTK_TEXT_VIEW(tab->editor), &iter);
}


static void edit_hovered_block(GtkButton *button, Tab *tab) {
	int line = tab->hover_line;
	hide_block_edit(tab);
	edit_block(tab, line);
}


/* A small round button that fades in over the view, such as the pencil or the code copy button */
static GtkWidget *create_hover_button(const char *icon, const char *tooltip, GCallback clicked, Tab *tab) {
	GtkWidget *button = gtk_button_new_from_icon_name(icon);
	gtk_widget_set_tooltip_text(button, tooltip);
	gtk_widget_set_halign(button, GTK_ALIGN_START);
	gtk_widget_set_valign(button, GTK_ALIGN_START);
	gtk_widget_set_size_request(button, BLOCK_EDIT_SIZE, BLOCK_EDIT_SIZE);
	gtk_widget_set_can_target(button, FALSE);
	gtk_widget_add_css_class(button, "flat");
	gtk_widget_add_css_class(button, "circular");
	gtk_widget_add_css_class(button, "block-edit");
	g_signal_connect(button, "clicked", clicked, tab);
	return button;
}


/* Scrolls once the editor has its real size, since it may have been hidden or sliding in before */
static gboolean land_editor(GtkWidget *root, GdkFrameClock *clock, gpointer data) {
	Tab *tab = get_tab(root);
	if (!gtk_widget_get_mapped(tab->editor) || gtk_widget_get_height(tab->editor) == 0)
		return G_SOURCE_CONTINUE;
	tab->editor_landing_handler = 0;
	GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tab->editor));
	GtkTextMark *mark = gtk_text_buffer_get_mark(buffer, "landing");
	if (!mark)
		return G_SOURCE_REMOVE;
	GtkTextIter iter;
	GdkRectangle location;
	gtk_text_buffer_get_iter_at_mark(buffer, &iter, mark);
	gtk_text_view_get_iter_location(GTK_TEXT_VIEW(tab->editor), &iter, &location);
	/* The line lands where get_editor_line reads, so leaving and coming back never drifts */
	GdkRectangle visible;
	gtk_text_view_get_visible_rect(GTK_TEXT_VIEW(tab->editor), &visible);
	GtkAdjustment *adjustment = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(tab->editor));
	double value = location.y - EDITOR_MARGIN + gtk_adjustment_get_value(adjustment) - visible.y;
	double maximum = gtk_adjustment_get_upper(adjustment) - gtk_adjustment_get_page_size(adjustment);
	gtk_adjustment_set_value(adjustment, CLAMP(value, 0, MAX(maximum, 0)));
	return G_SOURCE_REMOVE;
}


void scroll_editor_to_line(Tab *tab, int line) {
	GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tab->editor));
	GtkTextIter iter;
	get_iter_at_joined_line(buffer, &iter, MAX(line - 1, 0));
	gtk_text_buffer_place_cursor(buffer, &iter);
	GtkTextMark *mark = gtk_text_buffer_get_mark(buffer, "landing");
	if (!mark)
		mark = gtk_text_buffer_create_mark(buffer, "landing", &iter, TRUE);
	gtk_text_buffer_move_mark(buffer, mark, &iter);
	if (!tab->editor_landing_handler)
		tab->editor_landing_handler = gtk_widget_add_tick_callback(tab->root, land_editor, NULL, NULL);
}


int get_editor_line(Tab *tab) {
	GdkRectangle visible;
	gtk_text_view_get_visible_rect(GTK_TEXT_VIEW(tab->editor), &visible);
	GtkTextIter iter;
	gtk_text_view_get_line_at_y(GTK_TEXT_VIEW(tab->editor), &iter, visible.y + EDITOR_MARGIN, NULL);
	return get_joined_line(gtk_text_view_get_buffer(GTK_TEXT_VIEW(tab->editor)), gtk_text_iter_get_line(&iter)) + 1;
}


/* The editor stays as the reader left it, caret and all, unless the view has moved to another block since */
void follow_view_in_editor(Tab *tab) {
	int view_line = get_view_line(tab);
	GtkTextMark *editor_block = find_line_mark(tab, get_editor_line(tab));
	if (!editor_block || editor_block != find_line_mark(tab, view_line))
		scroll_editor_to_line(tab, view_line);
}


static Place get_current_place(Tab *tab) {
	return (Place){tab->is_editing, tab->is_editing ? get_editor_line(tab) : get_view_line(tab)};
}


void remember_place(Tab *tab) {
	Place place = get_current_place(tab);
	g_array_append_val(tab->back_places, place);
	g_array_set_size(tab->forward_places, 0);
}


static void go_to_place(Tab *tab, Place place) {
	double offset = 0;
	if (place.is_editing != tab->is_editing)
		change_tab_editing(tab, place.is_editing, place.line);
	else if (place.is_editing)
		scroll_editor_to_line(tab, place.line);
	else if (get_view_offset(tab, place.line, &offset))
		glide_scroller(GTK_SCROLLED_WINDOW(tab->scroller), offset, SCROLL_DURATION);
}


/* Moves one place along the history, keeping the place left behind for the opposite direction */
static gboolean step_through_places(Tab *tab, GArray *from, GArray *to) {
	if (from->len == 0)
		return FALSE;
	Place current = get_current_place(tab);
	Place place = g_array_index(from, Place, from->len - 1);
	g_array_set_size(from, from->len - 1);
	g_array_append_val(to, current);
	go_to_place(tab, place);
	return TRUE;
}


gboolean go_back_in_tab(Tab *tab) {
	return step_through_places(tab, tab->back_places, tab->forward_places);
}


gboolean go_forward_in_tab(Tab *tab) {
	return step_through_places(tab, tab->forward_places, tab->back_places);
}


/* The heading flashes as the glide starts, so it is lit when the view gets there */
void scroll_tab_to(Tab *tab, GtkTextMark *target) {
	remember_place(tab);
	glide_scroller(GTK_SCROLLED_WINDOW(tab->scroller), get_scroll_value(tab, get_mark_y(tab, target)) - SCROLL_MARGIN,
		SCROLL_DURATION);
	GtkTextIter line;
	gtk_text_buffer_get_iter_at_mark(gtk_text_view_get_buffer(get_view(tab)), &line, target);
	flash_text_line(get_view(tab), &line);
}


/* Glides to a heading from the contents list, in the editor at its source line while editing */
void show_tab_heading(Tab *tab, const OutlineEntry *entry) {
	if (!tab->is_editing) {
		scroll_tab_to(tab, entry->mark);
		return;
	}
	if (entry->line <= 0)
		return;
	remember_place(tab);
	scroll_editor_to_line(tab, entry->line);
	GtkTextIter line;
	get_iter_at_joined_line(gtk_text_view_get_buffer(GTK_TEXT_VIEW(tab->editor)), &line, entry->line - 1);
	flash_text_line(GTK_TEXT_VIEW(tab->editor), &line);
}


gboolean scroll_tab_to_anchor(Tab *tab, const char *anchor) {
	g_autofree char *lowered = g_utf8_strdown(anchor, -1);
	GtkTextMark *target = g_hash_table_lookup(tab->context.anchors, lowered);
	if (target)
		scroll_tab_to(tab, target);
	return target != NULL;
}


/* Opens the editor at the block the menu was opened on, or at the top of the view outside any block */
static void edit_menu_block(GSimpleAction *action, GVariant *parameter, gpointer data) {
	edit_block(data, ((Tab *)data)->menu_line);
}


static void copy_selection(GSimpleAction *action, GVariant *parameter, gpointer data) {
	Tab *tab = data;
	gtk_text_buffer_copy_clipboard(gtk_text_view_get_buffer(get_view(tab)), gtk_widget_get_clipboard(tab->content));
}


static void select_everything(GSimpleAction *action, GVariant *parameter, gpointer data) {
	GtkTextBuffer *buffer = gtk_text_view_get_buffer(get_view(data));
	GtkTextIter start, end;
	gtk_text_buffer_get_bounds(buffer, &start, &end);
	gtk_text_buffer_select_range(buffer, &start, &end);
}


/* Shows the view's own menu in place of the text view's, which would list editing commands a page cannot use
 * and put Edit Here last */
static void open_view_menu(GtkGestureClick *click, int presses, double x, double y, Tab *tab) {
	GtkWidget *view = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(click));
	gtk_gesture_set_state(GTK_GESTURE(click), GTK_EVENT_SEQUENCE_CLAIMED);
	GtkTextIter line;
	int index = view == tab->content && get_line_at_point(tab, view, x, y, &line) ? find_block_at(tab, &line) : -1;
	tab->menu_line = index < 0 ? 0 : find_source_line(tab, (guint)index, &line);
	gboolean has_selection = gtk_text_buffer_get_has_selection(gtk_text_view_get_buffer(GTK_TEXT_VIEW(view)));
	GAction *copy = g_action_map_lookup_action(G_ACTION_MAP(tab->view_actions), "copy");
	g_simple_action_set_enabled(G_SIMPLE_ACTION(copy), has_selection);
	graphene_point_t point;
	if (!gtk_widget_compute_point(view, tab->overlay, &GRAPHENE_POINT_INIT(x, y), &point))
		return;
	gtk_popover_set_pointing_to(GTK_POPOVER(tab->view_menu), &(GdkRectangle){(int)point.x, (int)point.y, 1, 1});
	gtk_popover_popup(GTK_POPOVER(tab->view_menu));
}


void add_block_menu(Tab *tab, GtkWidget *view) {
	GtkGesture *click = gtk_gesture_click_new();
	gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_SECONDARY);
	gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(click), GTK_PHASE_CAPTURE);
	g_signal_connect(click, "pressed", G_CALLBACK(open_view_menu), tab);
	gtk_widget_add_controller(view, GTK_EVENT_CONTROLLER(click));
}


/* One menu and its actions on the overlay, shared by every view the tab renders */
static void create_view_menu(Tab *tab) {
	static const GActionEntry ACTIONS[] = {
		{"edit-here", edit_menu_block, NULL, NULL, NULL, {0}},
		{"copy", copy_selection, NULL, NULL, NULL, {0}},
		{"select-all", select_everything, NULL, NULL, NULL, {0}},
	};
	tab->view_actions = g_simple_action_group_new();
	g_action_map_add_action_entries(G_ACTION_MAP(tab->view_actions), ACTIONS, G_N_ELEMENTS(ACTIONS), tab);
	gtk_widget_insert_action_group(tab->overlay, "document", G_ACTION_GROUP(tab->view_actions));
	g_object_unref(tab->view_actions);
	g_autoptr(GMenu) menu = g_menu_new();
	g_autoptr(GMenu) editing = g_menu_new();
	g_autoptr(GMenu) selection = g_menu_new();
	g_menu_append(editing, TEXT_EDIT_HERE_MENU, "document.edit-here");
	g_menu_append(selection, TEXT_COPY_MENU, "document.copy");
	g_menu_append(selection, TEXT_SELECT_ALL_MENU, "document.select-all");
	g_menu_append_section(menu, NULL, G_MENU_MODEL(editing));
	g_menu_append_section(menu, NULL, G_MENU_MODEL(selection));
	tab->view_menu = gtk_popover_menu_new_from_model(G_MENU_MODEL(menu));
	gtk_popover_set_has_arrow(GTK_POPOVER(tab->view_menu), FALSE);
	gtk_widget_set_halign(tab->view_menu, GTK_ALIGN_START);
	gtk_overlay_add_overlay(GTK_OVERLAY(tab->overlay), tab->view_menu);
}


/* Puts the view in an overlay with the pencil that follows the pointer */
GtkWidget *add_block_edit(Tab *tab) {
	GtkAdjustment *adjustment = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(tab->scroller));
	g_signal_connect_swapped(adjustment, "value-changed", G_CALLBACK(place_pencil_at_pointer), tab);
	tab->block_edit = create_hover_button("document-edit-symbolic", TEXT_EDIT_HERE, G_CALLBACK(edit_hovered_block),
		tab);
	tab->code_copy = create_hover_button("edit-copy-symbolic", TEXT_COPY_CODE, G_CALLBACK(copy_hovered_code), tab);
	tab->copy_start = tab->copy_end = -1;
	tab->overlay = gtk_overlay_new();
	gtk_overlay_set_child(GTK_OVERLAY(tab->overlay), tab->scroller);
	gtk_overlay_add_overlay(GTK_OVERLAY(tab->overlay), tab->block_edit);
	gtk_overlay_add_overlay(GTK_OVERLAY(tab->overlay), tab->code_copy);
	create_view_menu(tab);
	GtkEventController *motion = gtk_event_controller_motion_new();
	g_signal_connect(motion, "motion", G_CALLBACK(follow_pointer), tab);
	g_signal_connect(motion, "leave", G_CALLBACK(leave_view), tab);
	gtk_widget_add_controller(tab->overlay, motion);
	return tab->overlay;
}
