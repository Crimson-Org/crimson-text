#include <stdlib.h>
#include <string.h>

#include "lang/en.h"
#include "view/document-view.h"
#include "view/images.h"
#include "document/inline.h"
#include "view/picture-flow.h"
#include "view/render.h"
#include "common/smooth-scroll.h"
#include "editor/source-view.h"

#define LIST_SPACING 6
#define MAXIMUM_CELL_CHARACTERS 48
#define BULLET_COUNT 3
/* Unzoomed widths a list level and a quote level indent their text by */
#define LIST_INDENT 26
#define QUOTE_INDENT 20
#define SUBSCRIPT_RISE (-3 * PANGO_SCALE)
#define SUPERSCRIPT_RISE (5 * PANGO_SCALE)
#define SMALL_SCALE 0.833
#define BIG_SCALE 1.2
#define SCRIPT_SCALE 0.8

/* Bullet, white bullet and small square, by list depth */
static const char *const BULLETS[BULLET_COUNT] = {LIST_BULLET, "\u25E6", "\u25AA"};

/* Alert kinds from GitHub and Obsidian with the look they share */
static const struct {
	const char *kind, *family, *icon;
} ALERT_KINDS[] = {
	{"note", "note", "dialog-information-symbolic"},
	{"info", "note", "dialog-information-symbolic"},
	{"todo", "note", "object-select-symbolic"},
	{"abstract", "note", "view-list-symbolic"},
	{"summary", "note", "view-list-symbolic"},
	{"tldr", "note", "view-list-symbolic"},
	{"tip", "tip", "starred-symbolic"},
	{"hint", "tip", "starred-symbolic"},
	{"success", "tip", "object-select-symbolic"},
	{"check", "tip", "object-select-symbolic"},
	{"done", "tip", "object-select-symbolic"},
	{"important", "important", "emblem-important-symbolic"},
	{"question", "important", "help-about-symbolic"},
	{"help", "important", "help-about-symbolic"},
	{"faq", "important", "help-about-symbolic"},
	{"example", "important", "view-list-bullet-symbolic"},
	{"warning", "warning", "dialog-warning-symbolic"},
	{"attention", "warning", "dialog-warning-symbolic"},
	{"caution", "caution", "dialog-error-symbolic"},
	{"danger", "caution", "dialog-error-symbolic"},
	{"error", "caution", "dialog-error-symbolic"},
	{"failure", "caution", "dialog-error-symbolic"},
	{"fail", "caution", "dialog-error-symbolic"},
	{"bug", "caution", "dialog-error-symbolic"},
	{"quote", "quiet", "format-text-italic-symbolic"},
	{"cite", "quiet", "format-text-italic-symbolic"},
};

/* Walks one run of inline markup, writing its text with a tag for each open element */
typedef struct {
	RenderContext *context;
	GPtrArray *tags;
	/* How many tags each open element added */
	GArray *counts;
} MarkupWriter;


static void free_outline_entry(gpointer data) {
	OutlineEntry *entry = data;
	g_free(entry->title);
	g_free(entry);
}


void reset_render_context(RenderContext *context, GtkTextView *view) {
	if (!context->anchors) {
		context->anchors = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
		context->outline = g_ptr_array_new_with_free_func(free_outline_entry);
		context->blocks = g_array_new(FALSE, FALSE, sizeof(BlockMark));
		context->task_counts = g_ptr_array_new_with_free_func(g_free);
		context->line_tags = g_ptr_array_new();
	}
	context->view = view;
	g_hash_table_remove_all(context->anchors);
	g_ptr_array_set_size(context->outline, 0);
	g_array_set_size(context->blocks, 0);
	g_ptr_array_set_size(context->task_counts, 0);
	g_ptr_array_set_size(context->line_tags, 0);
	context->indent = 0;
	context->list_depth = 0;
	context->task_count = NULL;
	g_clear_pointer(&context->pending_marker, g_free);
	g_clear_object(&context->pending_check);
}


#define SWAP_FIELD(type, field) do { type kept = shown->field; shown->field = next->field; next->field = kept; } \
	while (0)

/* Hands the off screen view and what was learned rendering it to the shown context once it is in place */
void swap_render_contexts(RenderContext *shown, RenderContext *next) {
	SWAP_FIELD(GtkTextView *, view);
	SWAP_FIELD(GHashTable *, anchors);
	SWAP_FIELD(GPtrArray *, outline);
	SWAP_FIELD(GArray *, blocks);
	SWAP_FIELD(GPtrArray *, task_counts);
	reset_render_context(next, NULL);
}


void clear_render_context(RenderContext *context) {
	reset_render_context(context, NULL);
	g_clear_pointer(&context->anchors, g_hash_table_unref);
	g_clear_pointer(&context->outline, g_ptr_array_unref);
	g_clear_pointer(&context->blocks, g_array_unref);
	g_clear_pointer(&context->task_counts, g_ptr_array_unref);
	g_clear_pointer(&context->line_tags, g_ptr_array_unref);
	g_clear_pointer(&context->base_directory, g_free);
}


static GtkTextBuffer *get_buffer(RenderContext *context) {
	return gtk_text_view_get_buffer(context->view);
}


static int get_end_offset(RenderContext *context) {
	return gtk_text_buffer_get_char_count(get_buffer(context));
}


static void apply_tag(RenderContext *context, GtkTextTag *tag, int start, int end) {
	GtkTextBuffer *buffer = get_buffer(context);
	GtkTextIter start_iter, end_iter;
	gtk_text_buffer_get_iter_at_offset(buffer, &start_iter, start);
	gtk_text_buffer_get_iter_at_offset(buffer, &end_iter, end);
	gtk_text_buffer_apply_tag(buffer, tag, &start_iter, &end_iter);
}


static GtkTextTag *get_named_tag(RenderContext *context, const char *name) {
	return gtk_text_tag_table_lookup(gtk_text_buffer_get_tag_table(get_buffer(context)), name);
}


/* Writes text at the end, with line breaks kept inside the current paragraph */
static void insert_text(RenderContext *context, const char *text, gssize length, GtkTextTag *const *tags,
	guint tag_count) {
	g_autofree char *copy = length < 0 ? g_strdup(text) : g_strndup(text, length);
	g_auto(GStrv) lines = g_strsplit(copy, "\n", -1);
	g_autofree char *joined = g_strjoinv(LINE_SEPARATOR, lines);
	int start = get_end_offset(context);
	GtkTextIter end;
	gtk_text_buffer_get_end_iter(get_buffer(context), &end);
	gtk_text_buffer_insert(get_buffer(context), &end, joined, -1);
	for (guint index = 0; index < tag_count; index++)
		apply_tag(context, tags[index], start, get_end_offset(context));
}


static void insert_widget(RenderContext *context, GtkWidget *widget) {
	GtkTextIter end;
	gtk_text_buffer_get_end_iter(get_buffer(context), &end);
	GtkTextChildAnchor *anchor = gtk_text_buffer_create_child_anchor(get_buffer(context), &end);
	gtk_text_view_add_child_at_anchor(context->view, widget, anchor);
	/* Find uses it to put matches inside the widget in document order */
	g_object_set_data(G_OBJECT(widget), "anchor", anchor);
}


/* Starts a line, putting the list marker or checkbox waiting for it first */
static int begin_line(RenderContext *context) {
	int start = get_end_offset(context);
	if (context->pending_marker) {
		GtkTextTag *dimmed = get_named_tag(context, "dimmed");
		insert_text(context, context->pending_marker, -1, &dimmed, 1);
		insert_text(context, "\t", -1, NULL, 0);
	} else if (context->pending_check) {
		insert_widget(context, context->pending_check);
		insert_text(context, "\t", -1, NULL, 0);
	}
	context->is_line_marked = context->pending_marker || context->pending_check;
	g_clear_pointer(&context->pending_marker, g_free);
	g_clear_object(&context->pending_check);
	return start;
}


static GtkTextTag *get_gap_tag(RenderContext *context) {
	return get_named_tag(context, context->list_depth > 0 ? "gap-small" : "gap-large");
}


/* Ends the line begun at start, giving it the current indent, bars and gap with an optional tag of its own */
static void end_line(RenderContext *context, int start, GtkTextTag *extra) {
	GtkTextIter end_iter;
	gtk_text_buffer_get_end_iter(get_buffer(context), &end_iter);
	gtk_text_buffer_insert(get_buffer(context), &end_iter, "\n", 1);
	int end = get_end_offset(context);
	for (guint index = 0; index < context->line_tags->len; index++)
		apply_tag(context, g_ptr_array_index(context->line_tags, index), start, end);
	if (context->indent > 0) {
		GtkTextBuffer *buffer = get_buffer(context);
		GtkTextTag *indent = context->is_line_marked
			? get_indent_tag(buffer, context->indent - LIST_INDENT, LIST_INDENT) : get_indent_tag(buffer, context->indent, 0);
		apply_tag(context, indent, start, end);
	}
	apply_tag(context, get_gap_tag(context), start, end);
	if (extra)
		apply_tag(context, extra, start, end);
}


/* Gives the last written line the gap of the level it now ends, after a list closes */
static void set_last_line_gap(RenderContext *context) {
	GtkTextBuffer *buffer = get_buffer(context);
	GtkTextIter start, end;
	gtk_text_buffer_get_end_iter(buffer, &end);
	start = end;
	if (!gtk_text_iter_backward_line(&start))
		return;
	gtk_text_buffer_remove_tag_by_name(buffer, "gap-small", &start, &end);
	gtk_text_buffer_remove_tag_by_name(buffer, "gap-large", &start, &end);
	gtk_text_buffer_apply_tag(buffer, get_gap_tag(context), &start, &end);
}


static void insert_block_widget(RenderContext *context, GtkWidget *widget) {
	int start = begin_line(context);
	insert_widget(context, create_block_frame(widget, context->indent));
	end_line(context, start, NULL);
}


static GtkTextTag *create_style_tag(GtkTextBuffer *buffer, const char *name, const char *element) {
	if (strcmp(element, "b") == 0)
		return gtk_text_buffer_create_tag(buffer, name, "weight", PANGO_WEIGHT_BOLD, NULL);
	if (strcmp(element, "i") == 0)
		return gtk_text_buffer_create_tag(buffer, name, "style", PANGO_STYLE_ITALIC, NULL);
	if (strcmp(element, "s") == 0)
		return gtk_text_buffer_create_tag(buffer, name, "strikethrough", TRUE, NULL);
	if (strcmp(element, "u") == 0)
		return gtk_text_buffer_create_tag(buffer, name, "underline", PANGO_UNDERLINE_SINGLE, NULL);
	if (strcmp(element, "sub") == 0)
		return gtk_text_buffer_create_tag(buffer, name, "rise", SUBSCRIPT_RISE, "scale", SCRIPT_SCALE, NULL);
	if (strcmp(element, "sup") == 0)
		return gtk_text_buffer_create_tag(buffer, name, "rise", SUPERSCRIPT_RISE, "scale", SCRIPT_SCALE, NULL);
	if (strcmp(element, "small") == 0)
		return gtk_text_buffer_create_tag(buffer, name, "scale", SMALL_SCALE, NULL);
	if (strcmp(element, "big") == 0)
		return gtk_text_buffer_create_tag(buffer, name, "scale", BIG_SCALE, NULL);
	if (strcmp(element, "tt") == 0)
		return gtk_text_buffer_create_tag(buffer, name, "family", "monospace", NULL);
	return NULL;
}


/* Reads a colour with an alpha such as 45%, as Pango markup writes them in two attributes */
static gboolean read_color(const char *value, const char *alpha, GdkRGBA *color) {
	if (!value || !gdk_rgba_parse(color, value))
		return FALSE;
	if (alpha)
		color->alpha = g_ascii_strtod(alpha, NULL) / 100;
	return TRUE;
}


static const char *find_attribute(const char **names, const char **values, const char *name) {
	for (int index = 0; names[index]; index++)
		if (strcmp(names[index], name) == 0)
			return values[index];
	return NULL;
}


static void set_span_properties(GtkTextTag *tag, const char **names, const char **values) {
	const char *family = find_attribute(names, values, "font_family");
	const char *weight = find_attribute(names, values, "weight");
	const char *style = find_attribute(names, values, "style");
	const char *size = find_attribute(names, values, "size");
	GdkRGBA color;
	if (family)
		g_object_set(tag, "family", family, NULL);
	if (weight)
		g_object_set(tag, "weight", strcmp(weight, "bold") == 0 ? PANGO_WEIGHT_BOLD
			: strcmp(weight, "normal") == 0 ? PANGO_WEIGHT_NORMAL : atoi(weight), NULL);
	if (g_strcmp0(style, "italic") == 0)
		g_object_set(tag, "style", PANGO_STYLE_ITALIC, NULL);
	if (size && strchr(size, '%'))
		g_object_set(tag, "scale", g_ascii_strtod(size, NULL) / 100, NULL);
	else if (g_strcmp0(size, "small") == 0)
		g_object_set(tag, "scale", SMALL_SCALE, NULL);
	if (read_color(find_attribute(names, values, "background"), find_attribute(names, values, "bgalpha"), &color))
		g_object_set(tag, "background-rgba", &color, NULL);
	if (read_color(find_attribute(names, values, "foreground"), find_attribute(names, values, "fgalpha"), &color))
		g_object_set(tag, "foreground-rgba", &color, NULL);
}


/* One shared tag per element and attribute set, so long documents do not fill the tag table */
static GtkTextTag *get_element_tag(RenderContext *context, const char *element, const char **names,
	const char **values) {
	g_autoptr(GString) name = g_string_new("element-");
	g_string_append(name, element);
	for (int index = 0; names[index]; index++)
		g_string_append_printf(name, "|%s=%s", names[index], values[index]);
	GtkTextTag *tag = get_named_tag(context, name->str);
	if (tag)
		return tag;
	GtkTextBuffer *buffer = get_buffer(context);
	if (strcmp(element, "span") != 0)
		return create_style_tag(buffer, name->str, element);
	tag = gtk_text_buffer_create_tag(buffer, name->str, NULL);
	set_span_properties(tag, names, values);
	return tag;
}


/* Each link gets its own tag carrying its address, beside the shared look of links */
static GtkTextTag *create_link_tag(RenderContext *context, const char **names, const char **values) {
	const char *link = find_attribute(names, values, "href");
	const char *title = find_attribute(names, values, "title");
	if (!link)
		return NULL;
	GtkTextTag *tag = gtk_text_tag_new(NULL);
	gtk_text_tag_table_add(gtk_text_buffer_get_tag_table(get_buffer(context)), tag);
	g_object_unref(tag);
	g_object_set_data_full(G_OBJECT(tag), "link", g_strdup(link), g_free);
	/* The title was escaped once more so labels would show it as plain text */
	g_object_set_data_full(G_OBJECT(tag), "title", title ? decode_entities(title, strlen(title)) : g_strdup(link),
		g_free);
	return tag;
}


static void start_markup_element(GMarkupParseContext *parser, const char *element, const char **names,
	const char **values, gpointer data, GError **error) {
	MarkupWriter *writer = data;
	guint count = writer->tags->len;
	if (strcmp(element, "a") == 0) {
		GtkTextTag *link = create_link_tag(writer->context, names, values);
		if (link) {
			g_ptr_array_add(writer->tags, get_named_tag(writer->context, "link"));
			g_ptr_array_add(writer->tags, link);
		}
	} else if (strcmp(element, "markup") != 0) {
		GtkTextTag *tag = get_element_tag(writer->context, element, names, values);
		if (tag)
			g_ptr_array_add(writer->tags, tag);
	}
	guint added = writer->tags->len - count;
	g_array_append_val(writer->counts, added);
}


static void end_markup_element(GMarkupParseContext *parser, const char *element, gpointer data, GError **error) {
	MarkupWriter *writer = data;
	if (writer->counts->len == 0)
		return;
	guint added = g_array_index(writer->counts, guint, writer->counts->len - 1);
	g_array_set_size(writer->counts, writer->counts->len - 1);
	g_ptr_array_set_size(writer->tags, writer->tags->len - added);
}


static void write_markup_text(GMarkupParseContext *parser, const char *text, gsize length, gpointer data,
	GError **error) {
	MarkupWriter *writer = data;
	insert_text(writer->context, text, length, (GtkTextTag *const *)writer->tags->pdata, writer->tags->len);
}


/* Writes the inline markup the parser produced, the same subset of Pango markup labels read */
static void insert_markup(RenderContext *context, const char *markup, GtkTextTag *base) {
	static const GMarkupParser PARSER = {start_markup_element, end_markup_element, write_markup_text, NULL, NULL};
	MarkupWriter writer = {context, g_ptr_array_new(), g_array_new(FALSE, FALSE, sizeof(guint))};
	if (base)
		g_ptr_array_add(writer.tags, base);
	g_autofree char *wrapped = g_strconcat("<markup>", markup, "</markup>", NULL);
	GMarkupParseContext *parser = g_markup_parse_context_new(&PARSER, 0, &writer, NULL);
	g_autoptr(GError) error = NULL;
	if (!g_markup_parse_context_parse(parser, wrapped, -1, &error))
		g_warning("Inline markup could not be shown: %s", error->message);
	g_markup_parse_context_free(parser);
	g_ptr_array_unref(writer.tags);
	g_array_unref(writer.counts);
}


static gboolean activate_label_link(GtkLabel *label, const char *link, RenderContext *context) {
	context->activate_link(link, context->data);
	return TRUE;
}


static GtkWidget *create_label(RenderContext *context, const char *markup, const char *css_class) {
	GtkWidget *label = gtk_label_new(NULL);
	gtk_label_set_markup(GTK_LABEL(label), markup);
	gtk_label_set_wrap(GTK_LABEL(label), TRUE);
	gtk_label_set_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
	gtk_label_set_xalign(GTK_LABEL(label), 0);
	gtk_label_set_selectable(GTK_LABEL(label), TRUE);
	gtk_widget_set_focus_on_click(label, FALSE);
	if (css_class)
		gtk_widget_add_css_class(label, css_class);
	g_signal_connect(label, "activate-link", G_CALLBACK(activate_label_link), context);
	return label;
}


static void click_picture(GtkGestureClick *gesture, int presses, double x, double y, RenderContext *context) {
	GtkWidget *picture = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
	const char *link = g_object_get_data(G_OBJECT(picture), "link");
	if (link)
		context->activate_link(link, context->data);
	else
		open_picture_outside(GTK_PICTURE(picture));
}


static GtkWidget *create_picture(RenderContext *context, const Piece *piece) {
	GtkWidget *picture = gtk_picture_new();
	gtk_widget_add_css_class(picture, "picture");
	if (piece->image_text && *piece->image_text) {
		gtk_picture_set_alternative_text(GTK_PICTURE(picture), piece->image_text);
		gtk_widget_set_tooltip_text(picture, piece->image_text);
	}
	if (piece->image_link) {
		g_object_set_data_full(G_OBJECT(picture), "link", g_strdup(piece->image_link), g_free);
		g_autofree char *tooltip = piece->image_text && *piece->image_text
			? g_strdup_printf("%s\n%s", piece->image_text, piece->image_link) : g_strdup(piece->image_link);
		gtk_widget_set_tooltip_text(picture, tooltip);
	}
	GtkGesture *click = gtk_gesture_click_new();
	g_signal_connect(click, "released", G_CALLBACK(click_picture), context);
	gtk_widget_add_controller(picture, GTK_EVENT_CONTROLLER(click));
	gtk_widget_set_cursor_from_name(picture, piece->image_link ? "pointer" : "zoom-in");
	load_picture(GTK_PICTURE(picture), piece->image_source, context->base_directory);
	return picture;
}


/* Gathers the run of pictures starting at index into one wrapping row and returns the index after it */
static guint add_picture_run(RenderContext *context, const Block *block, guint index, GtkWidget **flow) {
	*flow = create_picture_flow(block->centered);
	for (; index < block->pieces->len; index++) {
		const Piece *piece = g_ptr_array_index(block->pieces, index);
		if (piece->markup)
			break;
		add_flow_picture(PICTURE_FLOW(*flow), create_picture(context, piece), piece->image_width);
	}
	return index;
}


/* Writes a paragraph's text runs as lines and its pictures as rows between them */
static void render_inline(RenderContext *context, const Block *block, GtkTextTag *line_tag) {
	GtkTextTag *centered = block->centered ? get_named_tag(context, "centered") : NULL;
	int start = -1;
	for (guint index = 0; index < block->pieces->len;) {
		const Piece *piece = g_ptr_array_index(block->pieces, index);
		if (piece->markup) {
			if (start < 0)
				start = begin_line(context);
			insert_markup(context, piece->markup, NULL);
			index++;
			continue;
		}
		if (start >= 0)
			end_line(context, start, line_tag ? line_tag : centered);
		start = -1;
		GtkWidget *flow = NULL;
		index = add_picture_run(context, block, index, &flow);
		insert_block_widget(context, flow);
	}
	if (start >= 0)
		end_line(context, start, line_tag ? line_tag : centered);
}


/* Shows a table cell as one label, or as labels and rows of pictures in reading order */
static GtkWidget *create_cell_content(RenderContext *context, const Block *block) {
	if (block->pieces->len == 0)
		return NULL;
	const Piece *first = g_ptr_array_index(block->pieces, 0);
	if (block->pieces->len == 1 && first->markup)
		return create_label(context, first->markup, NULL);
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, LIST_SPACING);
	for (guint index = 0; index < block->pieces->len;) {
		const Piece *piece = g_ptr_array_index(block->pieces, index);
		if (piece->markup) {
			gtk_box_append(GTK_BOX(box), create_label(context, piece->markup, NULL));
			index++;
			continue;
		}
		GtkWidget *flow = NULL;
		index = add_picture_run(context, block, index, &flow);
		gtk_box_append(GTK_BOX(box), flow);
	}
	return box;
}


static void render_children(RenderContext *context, const Block *block) {
	for (guint index = 0; index < block->children->len; index++)
		render_block(context, g_ptr_array_index(block->children, index));
}


static GtkTextMark *create_end_mark(RenderContext *context) {
	GtkTextIter end;
	gtk_text_buffer_get_end_iter(get_buffer(context), &end);
	return gtk_text_buffer_create_mark(get_buffer(context), NULL, &end, TRUE);
}


static char *get_line_text(RenderContext *context, GtkTextMark *mark) {
	GtkTextIter start, end;
	gtk_text_buffer_get_iter_at_mark(get_buffer(context), &start, mark);
	end = start;
	gtk_text_iter_forward_to_line_end(&end);
	return gtk_text_buffer_get_text(get_buffer(context), &start, &end, FALSE);
}


/* Rewrites a checklist heading's count with the tags around it, so it keeps the heading's look */
static void show_task_count(GtkTextBuffer *buffer, TaskCount *count) {
	GtkTextIter start, end;
	gtk_text_buffer_get_iter_at_mark(buffer, &start, count->start);
	gtk_text_buffer_get_iter_at_mark(buffer, &end, count->end);
	GtkTextIter before = start;
	gtk_text_iter_backward_char(&before);
	g_autoptr(GSList) tags = gtk_text_iter_get_tags(&before);
	gtk_text_buffer_delete(buffer, &start, &end);
	g_autofree char *text = g_strdup_printf("%d / %d", count->done, count->total);
	int offset = gtk_text_iter_get_offset(&start);
	gtk_text_buffer_insert(buffer, &start, text, -1);
	gtk_text_buffer_get_iter_at_offset(buffer, &end, offset);
	for (GSList *item = tags; item; item = item->next)
		gtk_text_buffer_apply_tag(buffer, item->data, &end, &start);
	gtk_text_buffer_apply_tag_by_name(buffer, "task-count", &end, &start);
	gtk_text_buffer_move_mark(buffer, count->end, &start);
}


static void add_task_count(RenderContext *context, const Block *block) {
	insert_text(context, "  ", -1, NULL, 0);
	TaskCount *count = g_new0(TaskCount, 1);
	GtkTextIter end;
	gtk_text_buffer_get_end_iter(get_buffer(context), &end);
	count->start = gtk_text_buffer_create_mark(get_buffer(context), NULL, &end, TRUE);
	insert_text(context, " ", -1, NULL, 0);
	gtk_text_buffer_get_end_iter(get_buffer(context), &end);
	/* Both ends keep left of text written at them, so the line break after the count is left outside it */
	count->end = gtk_text_buffer_create_mark(get_buffer(context), NULL, &end, TRUE);
	count->done = block->task_done;
	count->total = block->task_total;
	g_ptr_array_add(context->task_counts, count);
	context->task_count = count;
}


static void render_heading(RenderContext *context, const Block *block, GtkTextMark *mark) {
	g_autofree char *level = g_strdup_printf("h%d", CLAMP(block->level, 1, 6));
	GtkTextTag *tag = get_named_tag(context, level);
	int start = begin_line(context);
	for (guint index = 0; index < block->pieces->len; index++) {
		const Piece *piece = g_ptr_array_index(block->pieces, index);
		if (piece->markup)
			insert_markup(context, piece->markup, NULL);
	}
	g_autofree char *title = get_line_text(context, mark);
	if (block->task_total > 0)
		add_task_count(context, block);
	end_line(context, start, tag);
	if (block->task_total > 0)
		show_task_count(get_buffer(context), context->task_count);
	if (block->label) {
		char *anchor = g_strdup(block->label);
		for (int count = 1; g_hash_table_contains(context->anchors, anchor); count++) {
			g_free(anchor);
			anchor = g_strdup_printf("%s-%d", block->label, count);
		}
		g_hash_table_insert(context->anchors, anchor, mark);
	}
	OutlineEntry *entry = g_new0(OutlineEntry, 1);
	entry->title = g_strdup(block->text ? block->text : title);
	entry->level = block->level;
	entry->line = block->line;
	entry->mark = mark;
	g_ptr_array_add(context->outline, entry);
}


static void render_quote(RenderContext *context, const Block *block) {
	g_ptr_array_add(context->line_tags, get_bar_tag(get_buffer(context), context->indent, NULL));
	g_ptr_array_add(context->line_tags, get_named_tag(context, "quote"));
	context->indent += QUOTE_INDENT;
	render_children(context, block);
	context->indent -= QUOTE_INDENT;
	g_ptr_array_set_size(context->line_tags, context->line_tags->len - 2);
}


static void render_alert(RenderContext *context, const Block *block) {
	const char *family = "note", *icon = "dialog-information-symbolic";
	for (guint index = 0; index < G_N_ELEMENTS(ALERT_KINDS); index++) {
		if (g_strcmp0(block->text, ALERT_KINDS[index].kind) != 0)
			continue;
		family = ALERT_KINDS[index].family;
		icon = ALERT_KINDS[index].icon;
	}
	GtkTextTag *bar = get_bar_tag(get_buffer(context), context->indent, get_alert_color(family));
	g_ptr_array_add(context->line_tags, bar);
	context->indent += QUOTE_INDENT;
	int start = begin_line(context);
	GtkWidget *image = gtk_image_new_from_icon_name(icon);
	g_autofree char *family_class = g_strconcat("alert-", family, NULL);
	gtk_widget_add_css_class(image, "alert-icon");
	gtk_widget_add_css_class(image, family_class);
	insert_widget(context, image);
	insert_text(context, " ", -1, NULL, 0);
	g_autofree char *kind_title = g_strdup(block->text);
	kind_title[0] = g_ascii_toupper(kind_title[0]);
	g_autofree char *escaped = g_markup_escape_text(kind_title, -1);
	g_autofree char *title_tag = g_strconcat("alert-title-", family, NULL);
	insert_markup(context, block->label ? block->label : escaped, get_named_tag(context, title_tag));
	end_line(context, start, NULL);
	render_children(context, block);
	context->indent -= QUOTE_INDENT;
	g_ptr_array_set_size(context->line_tags, context->line_tags->len - 1);
}


static void tick_task(GtkCheckButton *check, RenderContext *context) {
	long offset = GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(check), "task-offset"));
	gboolean is_done = gtk_check_button_get_active(check);
	TaskCount *count = g_object_get_data(G_OBJECT(check), "task-count");
	GtkWidget *view = gtk_widget_get_ancestor(GTK_WIDGET(check), GTK_TYPE_TEXT_VIEW);
	if (count && view) {
		count->done += is_done ? 1 : -1;
		show_task_count(gtk_text_view_get_buffer(GTK_TEXT_VIEW(view)), count);
	}
	context->toggle_task(offset, is_done, context->data);
}


/* Tasks with a known mark in the source can be ticked, the others only show their state */
static GtkWidget *create_task_check(RenderContext *context, const Block *item) {
	GtkWidget *check = gtk_check_button_new();
	gtk_check_button_set_active(GTK_CHECK_BUTTON(check), item->checked);
	gtk_widget_add_css_class(check, "task");
	gboolean can_toggle = item->task_offset >= 0 && context->toggle_task;
	gtk_widget_set_can_target(check, can_toggle);
	gtk_widget_set_can_focus(check, can_toggle);
	if (can_toggle) {
		g_object_set_data(G_OBJECT(check), "task-offset", GSIZE_TO_POINTER((gsize)item->task_offset));
		g_object_set_data(G_OBJECT(check), "task-count", context->task_count);
		g_signal_connect(check, "toggled", G_CALLBACK(tick_task), context);
	}
	return g_object_ref_sink(check);
}


static void render_list(RenderContext *context, const Block *list) {
	int depth = context->list_depth++;
	context->indent += LIST_INDENT;
	for (guint index = 0; index < list->children->len; index++) {
		const Block *item = g_ptr_array_index(list->children, index);
		if (item->task)
			context->pending_check = create_task_check(context, item);
		else
			context->pending_marker = list->ordered ? g_strdup_printf("%d.", list->level + (int)index)
				: g_strdup(BULLETS[depth % BULLET_COUNT]);
		render_children(context, item);
		if (context->pending_marker || context->pending_check)
			end_line(context, begin_line(context), NULL);
	}
	context->indent -= LIST_INDENT;
	context->list_depth--;
	set_last_line_gap(context);
}


/* One shared tag per look the style scheme gives code, so colours from many code blocks reuse a few tags */
static GtkTextTag *get_code_color_tag(RenderContext *context, GtkTextTag *source) {
	gboolean has_foreground = FALSE, has_background = FALSE, has_weight = FALSE, has_style = FALSE;
	gboolean has_underline = FALSE, has_strikethrough = FALSE, is_struck = FALSE;
	g_autoptr(GdkRGBA) foreground = NULL;
	g_autoptr(GdkRGBA) background = NULL;
	int weight = 0, style = 0, underline = 0;
	g_object_get(source, "foreground-set", &has_foreground, "foreground-rgba", &foreground, "background-set",
		&has_background, "background-rgba", &background, "weight-set", &has_weight, "weight", &weight, "style-set",
		&has_style, "style", &style, "underline-set", &has_underline, "underline", &underline, "strikethrough-set",
		&has_strikethrough, "strikethrough", &is_struck, NULL);
	g_autoptr(GString) name = g_string_new("code-color");
	g_autofree char *foreground_text = has_foreground ? gdk_rgba_to_string(foreground) : NULL;
	g_autofree char *background_text = has_background ? gdk_rgba_to_string(background) : NULL;
	g_string_append_printf(name, "|%s|%s|%d|%d|%d|%d", foreground_text ? foreground_text : "",
		background_text ? background_text : "", has_weight ? weight : 0, has_style ? style : -1,
		has_underline ? underline : -1, has_strikethrough ? is_struck : -1);
	if (!foreground_text && !background_text && !has_weight && !has_style && !has_underline && !has_strikethrough)
		return NULL;
	GtkTextTag *tag = get_named_tag(context, name->str);
	if (tag)
		return tag;
	tag = gtk_text_buffer_create_tag(get_buffer(context), name->str, NULL);
	if (foreground_text)
		g_object_set(tag, "foreground-rgba", foreground, NULL);
	if (background_text)
		g_object_set(tag, "background-rgba", background, NULL);
	if (has_weight)
		g_object_set(tag, "weight", weight, NULL);
	if (has_style)
		g_object_set(tag, "style", style, NULL);
	if (has_underline)
		g_object_set(tag, "underline", underline, NULL);
	if (has_strikethrough)
		g_object_set(tag, "strikethrough", is_struck, NULL);
	return tag;
}


/* Where coloured runs of one code block go in the view */
typedef struct {
	RenderContext *context;
	int base;
} CodeWriter;


static void color_code_run(int start, int end, GtkTextTag *source, gpointer data) {
	CodeWriter *writer = data;
	GtkTextTag *tag = get_code_color_tag(writer->context, source);
	if (tag)
		apply_tag(writer->context, tag, writer->base + start, writer->base + end);
}


/* Tab stops four characters wide in the code font, as the editor has them */
static void measure_code_tabs(RenderContext *context, GtkTextTag *code) {
	if (g_object_get_data(G_OBJECT(code), "tabs-measured"))
		return;
	PangoLayout *layout = gtk_widget_create_pango_layout(GTK_WIDGET(context->view), "    ");
	PangoFontDescription *font = pango_font_description_from_string("monospace");
	pango_layout_set_font_description(layout, font);
	int width = 0;
	pango_layout_get_pixel_size(layout, &width, NULL);
	PangoTabArray *tabs = pango_tab_array_new_with_positions(1, TRUE, PANGO_TAB_LEFT, MAX(width, 1));
	g_object_set(code, "tabs", tabs, NULL);
	pango_tab_array_free(tabs);
	pango_font_description_free(font);
	g_object_unref(layout);
	g_object_set_data(G_OBJECT(code), "tabs-measured", GINT_TO_POINTER(TRUE));
}


/* Code is text in the view on a drawn background, so it can be selected, searched and streamed like the rest */
static void render_code(RenderContext *context, const Block *block) {
	GtkTextBuffer *buffer = get_buffer(context);
	GtkTextTag *code = get_named_tag(context, "code");
	measure_code_tabs(context, code);
	context->indent += CODE_PADDING;
	int start = begin_line(context);
	int base = get_end_offset(context);
	const char *text = block->text ? block->text : "";
	GtkTextIter end;
	gtk_text_buffer_get_end_iter(buffer, &end);
	gtk_text_buffer_insert(buffer, &end, text, -1);
	CodeWriter writer = {context, base};
	highlight_code(text, block->label, color_code_run, &writer);
	end_line(context, start, code);
	context->indent -= CODE_PADDING;
	GtkTextIter first_end, last_start;
	gtk_text_buffer_get_iter_at_offset(buffer, &first_end, start);
	gtk_text_iter_forward_to_line_end(&first_end);
	gtk_text_buffer_get_end_iter(buffer, &end);
	last_start = end;
	gtk_text_iter_backward_line(&last_start);
	GtkTextIter first_start;
	gtk_text_buffer_get_iter_at_offset(buffer, &first_start, start);
	gtk_text_buffer_apply_tag_by_name(buffer, "code-first", &first_start, &first_end);
	gtk_text_buffer_apply_tag_by_name(buffer, "code-last", &last_start, &end);
}


static GtkWidget *create_table(RenderContext *context, const Block *table) {
	static const float ALIGNMENTS[] = {0, 0, 0.5f, 1};
	GtkWidget *grid = gtk_grid_new();
	gtk_grid_set_row_spacing(GTK_GRID(grid), 1);
	gtk_grid_set_column_spacing(GTK_GRID(grid), 1);
	gtk_widget_set_halign(grid, GTK_ALIGN_START);
	gtk_widget_add_css_class(grid, "table");
	for (guint row_index = 0; row_index < table->children->len; row_index++) {
		const Block *row = g_ptr_array_index(table->children, row_index);
		for (guint column = 0; column < row->children->len; column++) {
			const Block *cell = g_ptr_array_index(row->children, column);
			GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
			gtk_widget_add_css_class(box, "cell");
			if (row->header)
				gtk_widget_add_css_class(box, "header");
			else if (row_index % 2 == 0)
				gtk_widget_add_css_class(box, "even");
			GtkWidget *content = create_cell_content(context, cell);
			if (GTK_IS_LABEL(content)) {
				gtk_label_set_xalign(GTK_LABEL(content), ALIGNMENTS[CLAMP(cell->level, 0, 3)]);
				gtk_label_set_max_width_chars(GTK_LABEL(content), MAXIMUM_CELL_CHARACTERS);
			}
			if (content)
				gtk_box_append(GTK_BOX(box), content);
			gtk_grid_attach(GTK_GRID(grid), box, column, row_index, 1, 1);
		}
	}
	GtkWidget *scroller = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
	gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroller), TRUE);
	set_scroll_child(GTK_SCROLLED_WINDOW(scroller), grid);
	return scroller;
}


static GtkWidget *create_metadata(RenderContext *context, const Block *block) {
	GtkWidget *grid = gtk_grid_new();
	gtk_grid_set_column_spacing(GTK_GRID(grid), 16);
	gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
	gtk_widget_add_css_class(grid, "metadata");
	g_auto(GStrv) lines = g_strsplit(block->text, "\n", -1);
	for (int index = 0; lines[index]; index++) {
		char *colon = strchr(lines[index], ':');
		gboolean is_entry = colon && !g_ascii_isspace(lines[index][0]);
		g_autofree char *key = is_entry ? g_markup_escape_text(lines[index], colon - lines[index]) : g_strdup("");
		g_autofree char *value = g_markup_escape_text(g_strstrip(is_entry ? colon + 1 : lines[index]), -1);
		gtk_grid_attach(GTK_GRID(grid), create_label(context, key, "dimmed"), 0, index, 1, 1);
		gtk_grid_attach(GTK_GRID(grid), create_label(context, value, NULL), 1, index, 1, 1);
	}
	return grid;
}


static void toggle_details(GtkButton *summary, GtkTextTag *hidden) {
	gboolean is_open = !gtk_widget_has_css_class(GTK_WIDGET(summary), "open");
	g_object_set(hidden, "invisible", !is_open, NULL);
	if (is_open)
		gtk_widget_add_css_class(GTK_WIDGET(summary), "open");
	else
		gtk_widget_remove_css_class(GTK_WIDGET(summary), "open");
}


/* A summary button that shows and hides the section's text, as GitHub shows HTML details */
static void render_details(RenderContext *context, const Block *block) {
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	GtkWidget *arrow = gtk_image_new_from_icon_name("pan-end-symbolic");
	gtk_widget_add_css_class(arrow, "details-arrow");
	gtk_box_append(GTK_BOX(row), arrow);
	gtk_box_append(GTK_BOX(row), gtk_label_new(block->label && *block->label ? block->label : TEXT_DETAILS));
	GtkWidget *summary = gtk_button_new();
	gtk_button_set_child(GTK_BUTTON(summary), row);
	gtk_widget_add_css_class(summary, "flat");
	gtk_widget_add_css_class(summary, "details-summary");
	if (block->checked)
		gtk_widget_add_css_class(summary, "open");
	int start = begin_line(context);
	insert_widget(context, summary);
	end_line(context, start, NULL);
	GtkTextTag *hidden = gtk_text_tag_new(NULL);
	g_object_set(hidden, "invisible", !block->checked, NULL);
	gtk_text_tag_table_add(gtk_text_buffer_get_tag_table(get_buffer(context)), hidden);
	g_object_unref(hidden);
	g_signal_connect(summary, "clicked", G_CALLBACK(toggle_details), hidden);
	g_ptr_array_add(context->line_tags, hidden);
	context->indent += QUOTE_INDENT;
	render_children(context, block);
	context->indent -= QUOTE_INDENT;
	g_ptr_array_set_size(context->line_tags, context->line_tags->len - 1);
}


/* Writes one block at the end of the view, remembering where blocks with a source line start */
void render_block(RenderContext *context, const Block *block) {
	gboolean needs_mark = block->line > 0 || block->kind == BLOCK_HEADING || block->anchors;
	GtkTextMark *mark = needs_mark ? create_end_mark(context) : NULL;
	if (block->line > 0) {
		BlockMark place = {mark, block->line};
		g_array_append_val(context->blocks, place);
	}
	for (guint index = 0; block->anchors && index < block->anchors->len; index++) {
		const char *anchor = g_ptr_array_index(block->anchors, index);
		if (!g_hash_table_contains(context->anchors, anchor))
			g_hash_table_insert(context->anchors, g_strdup(anchor), mark);
	}
	switch (block->kind) {
	case BLOCK_PARAGRAPH:
		render_inline(context, block, NULL);
		break;
	case BLOCK_HEADING:
		render_heading(context, block, mark);
		break;
	case BLOCK_QUOTE:
		render_quote(context, block);
		break;
	case BLOCK_ALERT:
		render_alert(context, block);
		break;
	case BLOCK_LIST:
		render_list(context, block);
		break;
	case BLOCK_CODE:
		render_code(context, block);
		break;
	case BLOCK_RULE:
		end_line(context, begin_line(context), get_named_tag(context, "rule"));
		break;
	case BLOCK_TABLE:
		insert_block_widget(context, create_table(context, block));
		break;
	case BLOCK_METADATA:
		insert_block_widget(context, create_metadata(context, block));
		break;
	case BLOCK_DETAILS:
		render_details(context, block);
		break;
	default:
		break;
	}
}
