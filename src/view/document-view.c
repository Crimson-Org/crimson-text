#include <math.h>

#include <adwaita.h>

#include "common/long-lines.h"
#include "view/document-view.h"

#define TOP_MARGIN 36
#define BOTTOM_MARGIN 72
#define BAR_WIDTH 4
#define RULE_HEIGHT 3
#define BORDER_HEIGHT 1
/* Space between a heading's text and its border, drawn that far above the gap below */
#define BORDER_SPACE 6
#define LINE_ALPHA 0.15
#define QUOTE_BAR_ALPHA 0.2
#define QUOTE_TEXT_ALPHA 0.7
#define DIM_TEXT_ALPHA 0.55
#define LIGHT_TEXT_ALPHA 0.8
/* Each flash fades in and out over twice this, and the highlight reaches past the text by the padding */
#define FLASH_DURATION 325
#define FLASH_ALPHA 0.3
#define FLASH_COUNT 2
#define FLASH_PADDING 8
#define FLASH_RADIUS 8
#define DEFAULT_ZOOM 100
#define CODE_RADIUS 8
#define CODE_ALPHA 0.06
/* Marks a spacing value a tag leaves to the tags below it */
#define UNSET_SPACING (-1)

/* Things drawn beside or under lines that text tags cannot draw themselves */
typedef enum { DECORATION_NONE, DECORATION_BAR, DECORATION_RULE, DECORATION_BORDER, DECORATION_CODE } Decoration;

/* A read only text view that draws quote bars, rules and heading borders under its text */
struct _DocumentView {
	GtkTextView parent_instance;
	/* Width the embedded blocks were last measured for */
	int frame_width;
};

G_DEFINE_FINAL_TYPE(DocumentView, document_view, GTK_TYPE_TEXT_VIEW)

/* Sizes one embedded block, such as a code block or a table, to the width of the text beside it */
struct _BlockFrame {
	GtkWidget parent_instance;
	GtkWidget *child;
	int indent;
};

#define BLOCK_FRAME_TYPE (block_frame_get_type())
G_DECLARE_FINAL_TYPE(BlockFrame, block_frame, BLOCK, FRAME, GtkWidget)
G_DEFINE_FINAL_TYPE(BlockFrame, block_frame, GTK_TYPE_WIDGET)

/* Heading sizes and the space above them, from GitHub's own style */
static const struct {
	double scale;
	int space_above;
} HEADINGS[] = {{2.0, 8}, {1.5, 12}, {1.25, 8}, {1.0, 4}, {0.875, 4}, {0.85, 4}};

/* Alert families with their colours from the libadwaita palette */
static const struct {
	const char *family, *color;
} ALERT_COLORS[] = {
	{"note", "#3584e4"}, {"tip", "#2ec27e"}, {"important", "#9141ac"}, {"warning", "#e5a50a"},
	{"caution", "#e01b24"},
};

static int document_zoom = DEFAULT_ZOOM;
static GList *live_views;


static int scale_size(int size) {
	return (int)lround(size * document_zoom / 100.0);
}


/* Space a tag sets around its lines, unzoomed, with the gap below as the part that separates blocks */
typedef struct {
	int gap, above, padding;
} Spacing;


static void scale_spacing_tag(GtkTextTag *tag) {
	const Spacing *spacing = g_object_get_data(G_OBJECT(tag), "spacing");
	if (spacing->gap != UNSET_SPACING)
		g_object_set(tag, "pixels-below-lines", scale_size(spacing->gap + spacing->padding), NULL);
	if (spacing->above != UNSET_SPACING)
		g_object_set(tag, "pixels-above-lines", scale_size(spacing->above), NULL);
	int right = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(tag), "right-padding"));
	if (right)
		g_object_set(tag, "right-margin", DOCUMENT_MARGIN + scale_size(right), NULL);
}


/* Spacing is stored unzoomed and set zoomed, so it follows the text size */
static void set_spacing(GtkTextTag *tag, int gap, int above, int padding) {
	Spacing *spacing = g_new(Spacing, 1);
	*spacing = (Spacing){gap, above, padding};
	g_object_set_data_full(G_OBJECT(tag), "spacing", spacing, g_free);
	scale_spacing_tag(tag);
}


static void set_decoration(GtkTextTag *tag, Decoration decoration) {
	g_object_set_data(G_OBJECT(tag), "decoration", GINT_TO_POINTER(decoration));
}


static void create_fixed_tags(GtkTextBuffer *buffer) {
	set_spacing(gtk_text_buffer_create_tag(buffer, "gap-small", NULL), LIST_GAP, UNSET_SPACING, 0);
	set_spacing(gtk_text_buffer_create_tag(buffer, "gap-large", NULL), BLOCK_GAP, UNSET_SPACING, 0);
	for (guint index = 0; index < G_N_ELEMENTS(HEADINGS); index++) {
		g_autofree char *name = g_strdup_printf("h%u", index + 1);
		GtkTextTag *tag = gtk_text_buffer_create_tag(buffer, name, "scale", HEADINGS[index].scale, "weight", 600, NULL);
		gboolean has_border = index < 2;
		set_spacing(tag, has_border ? BLOCK_GAP + BORDER_SPACE : BLOCK_GAP, HEADINGS[index].space_above, 0);
		if (has_border)
			set_decoration(tag, DECORATION_BORDER);
	}
	gtk_text_buffer_create_tag(buffer, "centered", "justification", GTK_JUSTIFY_CENTER, NULL);
	gtk_text_buffer_create_tag(buffer, "quote", NULL);
	gtk_text_buffer_create_tag(buffer, "dimmed", NULL);
	gtk_text_buffer_create_tag(buffer, "task-count", "scale", 0.833, "weight", 400, NULL);
	set_decoration(gtk_text_buffer_create_tag(buffer, "rule", NULL), DECORATION_RULE);
	gtk_text_buffer_create_tag(buffer, "link", "underline", PANGO_UNDERLINE_SINGLE, NULL);
	for (guint index = 0; index < G_N_ELEMENTS(ALERT_COLORS); index++) {
		g_autofree char *name = g_strconcat("alert-title-", ALERT_COLORS[index].family, NULL);
		gtk_text_buffer_create_tag(buffer, name, "foreground", ALERT_COLORS[index].color, "weight", 600, NULL);
	}
	gtk_text_buffer_create_tag(buffer, "alert-title-quiet", "weight", 600, NULL);
	GtkTextTag *code = gtk_text_buffer_create_tag(buffer, "code", "family", "monospace", "wrap-mode", GTK_WRAP_CHAR,
		NULL);
	g_object_set_data(G_OBJECT(code), "right-padding", GINT_TO_POINTER(CODE_PADDING));
	set_spacing(code, 0, 0, 0);
	set_decoration(code, DECORATION_CODE);
	set_spacing(gtk_text_buffer_create_tag(buffer, "code-first", NULL), UNSET_SPACING, CODE_PADDING, 0);
	set_spacing(gtk_text_buffer_create_tag(buffer, "code-last", NULL), BLOCK_GAP, UNSET_SPACING, CODE_PADDING);
}


/* Text tags hold fixed colours, so the ones taken from the theme are set again when it changes */
static void apply_theme_colors(GtkTextView *view) {
	AdwStyleManager *styles = adw_style_manager_get_default();
	gboolean is_dark = adw_style_manager_get_dark(styles);
	double shade = is_dark ? 1 : 0, alpha = is_dark ? 1 : LIGHT_TEXT_ALPHA;
	GdkRGBA quote = {shade, shade, shade, alpha * QUOTE_TEXT_ALPHA};
	GdkRGBA dim = {shade, shade, shade, alpha * DIM_TEXT_ALPHA};
	GdkRGBA link;
	adw_accent_color_to_standalone_rgba(adw_style_manager_get_accent_color(styles), is_dark, &link);
	GtkTextTagTable *tags = gtk_text_buffer_get_tag_table(gtk_text_view_get_buffer(view));
	g_object_set(gtk_text_tag_table_lookup(tags, "quote"), "foreground-rgba", &quote, NULL);
	g_object_set(gtk_text_tag_table_lookup(tags, "h6"), "foreground-rgba", &dim, NULL);
	g_object_set(gtk_text_tag_table_lookup(tags, "dimmed"), "foreground-rgba", &dim, NULL);
	g_object_set(gtk_text_tag_table_lookup(tags, "task-count"), "foreground-rgba", &dim, NULL);
	g_object_set(gtk_text_tag_table_lookup(tags, "alert-title-quiet"), "foreground-rgba", &dim, NULL);
	g_object_set(gtk_text_tag_table_lookup(tags, "link"), "foreground-rgba", &link, NULL);
}


static void scale_indent_tag(GtkTextTag *tag) {
	int left = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(tag), "indent-left")) - 1;
	int hang = scale_size(GPOINTER_TO_INT(g_object_get_data(G_OBJECT(tag), "indent-hang")));
	g_object_set(tag, "left-margin", DOCUMENT_MARGIN + scale_size(left), "indent", -hang, NULL);
	if (hang <= 0)
		return;
	PangoTabArray *tabs = pango_tab_array_new_with_positions(1, TRUE, PANGO_TAB_LEFT, hang);
	g_object_set(tag, "tabs", tabs, NULL);
	pango_tab_array_free(tabs);
}


static void scale_tag(GtkTextTag *tag, gpointer data) {
	if (g_object_get_data(G_OBJECT(tag), "indent-left"))
		scale_indent_tag(tag);
	if (g_object_get_data(G_OBJECT(tag), "spacing"))
		scale_spacing_tag(tag);
}


/* Indents lines by an unzoomed width, with a hanging part for the marker that starts a list item */
GtkTextTag *get_indent_tag(GtkTextBuffer *buffer, int left, int hang) {
	g_autofree char *name = g_strdup_printf("indent-%d-%d", left, hang);
	GtkTextTagTable *table = gtk_text_buffer_get_tag_table(buffer);
	GtkTextTag *tag = gtk_text_tag_table_lookup(table, name);
	if (tag)
		return tag;
	tag = gtk_text_buffer_create_tag(buffer, name, NULL);
	/* Zero would read as a missing value, so the stored left width is offset by one */
	g_object_set_data(G_OBJECT(tag), "indent-left", GINT_TO_POINTER(left + 1));
	g_object_set_data(G_OBJECT(tag), "indent-hang", GINT_TO_POINTER(hang));
	scale_indent_tag(tag);
	return tag;
}


/* A bar down the left of quotes and alerts at an unzoomed position, in a colour or the quiet text colour */
GtkTextTag *get_bar_tag(GtkTextBuffer *buffer, int x, const char *color) {
	g_autofree char *name = g_strdup_printf("bar-%d-%s", x, color ? color : "quiet");
	GtkTextTagTable *table = gtk_text_buffer_get_tag_table(buffer);
	GtkTextTag *tag = gtk_text_tag_table_lookup(table, name);
	if (tag)
		return tag;
	tag = gtk_text_buffer_create_tag(buffer, name, NULL);
	set_decoration(tag, DECORATION_BAR);
	g_object_set_data(G_OBJECT(tag), "bar-x", GINT_TO_POINTER(x));
	GdkRGBA *rgba = g_new0(GdkRGBA, 1);
	if (!color || !gdk_rgba_parse(rgba, color))
		g_clear_pointer(&rgba, g_free);
	g_object_set_data_full(G_OBJECT(tag), "bar-color", rgba, g_free);
	return tag;
}


static void forget_frame_sizes(GtkWidget *widget) {
	for (GtkWidget *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child)) {
		if (BLOCK_IS_FRAME(child))
			gtk_widget_queue_resize(child);
		else if (!GTK_IS_TEXT_VIEW(child))
			forget_frame_sizes(child);
	}
}


/* Colour of an alert family, or nothing for the quiet family that takes the text colour */
const char *get_alert_color(const char *family) {
	for (guint index = 0; index < G_N_ELEMENTS(ALERT_COLORS); index++)
		if (g_strcmp0(family, ALERT_COLORS[index].family) == 0)
			return ALERT_COLORS[index].color;
	return NULL;
}


static void rescale_view(gpointer view, gpointer data) {
	GtkTextTagTable *table = gtk_text_buffer_get_tag_table(gtk_text_view_get_buffer(view));
	gtk_text_tag_table_foreach(table, scale_tag, NULL);
	gtk_text_view_set_pixels_below_lines(view, scale_size(BLOCK_GAP));
	forget_frame_sizes(view);
	gtk_widget_queue_draw(view);
}


/* Indents and embedded blocks are in pixels, so they follow the text size by hand */
void set_document_zoom(int percent) {
	if (document_zoom == percent)
		return;
	document_zoom = percent;
	g_list_foreach(live_views, rescale_view, NULL);
}


static gboolean is_tag_hiding(GtkTextTag *tag) {
	gboolean is_invisible = FALSE, is_set = FALSE;
	g_object_get(tag, "invisible", &is_invisible, "invisible-set", &is_set, NULL);
	return is_invisible && is_set;
}


/* Whether the text at the position is in a collapsed section */
gboolean is_text_hidden(const GtkTextIter *iter) {
	g_autoptr(GSList) tags = gtk_text_iter_get_tags(iter);
	for (GSList *item = tags; item; item = item->next)
		if (is_tag_hiding(item->data))
			return TRUE;
	return FALSE;
}


/* Gap below a line from the strongest of its tags that sets one, as tags come weakest first */
static int get_line_gap(GSList *tags) {
	int gap = 0;
	for (GSList *item = tags; item; item = item->next) {
		const Spacing *spacing = g_object_get_data(item->data, "spacing");
		if (spacing && spacing->gap != UNSET_SPACING)
			gap = spacing->gap;
	}
	return scale_size(gap);
}


/* Left edge of a line's text, from its indent */
static int get_line_left(GSList *tags) {
	int left = 0;
	for (GSList *item = tags; item; item = item->next) {
		gpointer value = g_object_get_data(item->data, "indent-left");
		if (value)
			left = GPOINTER_TO_INT(value) - 1;
	}
	return DOCUMENT_MARGIN + scale_size(left);
}


static gboolean has_named_tag(GSList *tags, const char *name) {
	for (GSList *item = tags; item; item = item->next) {
		g_autofree char *tag_name = NULL;
		g_object_get(item->data, "name", &tag_name, NULL);
		if (g_strcmp0(tag_name, name) == 0)
			return TRUE;
	}
	return FALSE;
}


/* Draws one line's part of a code block's rounded background, rounding the block's first and last lines */
static void draw_code(GtkSnapshot *snapshot, GSList *tags, int y, int height, int gap, int width,
	const GdkRGBA *text_color) {
	gboolean is_first = has_named_tag(tags, "code-first"), is_last = has_named_tag(tags, "code-last");
	int left = get_line_left(tags) - scale_size(CODE_PADDING);
	graphene_rect_t area = GRAPHENE_RECT_INIT(left, y, DOCUMENT_MARGIN + width - left, is_last ? height - gap : height);
	graphene_size_t top = is_first ? GRAPHENE_SIZE_INIT(CODE_RADIUS, CODE_RADIUS) : GRAPHENE_SIZE_INIT(0, 0);
	graphene_size_t bottom = is_last ? GRAPHENE_SIZE_INIT(CODE_RADIUS, CODE_RADIUS) : GRAPHENE_SIZE_INIT(0, 0);
	GskRoundedRect rounded;
	gsk_rounded_rect_init(&rounded, &area, &top, &top, &bottom, &bottom);
	GdkRGBA color = {text_color->red, text_color->green, text_color->blue, text_color->alpha * CODE_ALPHA};
	gtk_snapshot_push_rounded_clip(snapshot, &rounded);
	gtk_snapshot_append_color(snapshot, &color, &area);
	gtk_snapshot_pop(snapshot);
}


static gboolean has_tag(const GtkTextIter *line, GtkTextTag *tag) {
	return gtk_text_iter_has_tag(line, tag);
}


static void draw_bar(GtkSnapshot *snapshot, GtkTextTag *tag, const GtkTextIter *line, int y, int height, int gap,
	const GdkRGBA *text_color) {
	GtkTextIter next = *line;
	gboolean continues = gtk_text_iter_forward_line(&next) && has_tag(&next, tag) && !is_text_hidden(&next);
	const GdkRGBA *bar_color = g_object_get_data(G_OBJECT(tag), "bar-color");
	GdkRGBA color = bar_color ? *bar_color : (GdkRGBA){text_color->red, text_color->green, text_color->blue,
		text_color->alpha * QUOTE_BAR_ALPHA};
	int x = DOCUMENT_MARGIN + scale_size(GPOINTER_TO_INT(g_object_get_data(G_OBJECT(tag), "bar-x")));
	int bottom = continues ? height : height - gap;
	gtk_snapshot_append_color(snapshot, &color, &GRAPHENE_RECT_INIT(x, y, BAR_WIDTH, MAX(bottom, 0)));
}


static void draw_line_decorations(GtkTextView *view, GtkSnapshot *snapshot, const GtkTextIter *line, int y, int height,
	const GdkRGBA *text_color) {
	g_autoptr(GSList) tags = gtk_text_iter_get_tags(line);
	int gap = get_line_gap(tags);
	int width = gtk_widget_get_width(GTK_WIDGET(view)) - DOCUMENT_MARGIN * 2;
	GdkRGBA line_color = {text_color->red, text_color->green, text_color->blue, text_color->alpha * LINE_ALPHA};
	for (GSList *item = tags; item; item = item->next) {
		Decoration decoration = GPOINTER_TO_INT(g_object_get_data(item->data, "decoration"));
		if (decoration == DECORATION_BAR)
			draw_bar(snapshot, item->data, line, y, height, gap, text_color);
		else if (decoration == DECORATION_RULE)
			gtk_snapshot_append_color(snapshot, &line_color, &GRAPHENE_RECT_INIT(DOCUMENT_MARGIN,
				y + (height - gap - RULE_HEIGHT) / 2.0f, width, RULE_HEIGHT));
		else if (decoration == DECORATION_CODE)
			draw_code(snapshot, tags, y, height, gap, width, text_color);
		else if (decoration == DECORATION_BORDER)
			gtk_snapshot_append_color(snapshot, &line_color, &GRAPHENE_RECT_INIT(DOCUMENT_MARGIN,
				y + height - scale_size(BLOCK_GAP) - BORDER_HEIGHT, width, BORDER_HEIGHT));
	}
}


/* Draws only the lines in sight, in buffer coordinates as the text view hands them over */
static void snapshot_layer(GtkTextView *view, GtkTextViewLayer layer, GtkSnapshot *snapshot) {
	if (layer != GTK_TEXT_VIEW_LAYER_BELOW_TEXT)
		return;
	draw_text_flash(view, snapshot);
	GdkRectangle visible;
	gtk_text_view_get_visible_rect(view, &visible);
	GdkRGBA text_color;
	gtk_widget_get_color(GTK_WIDGET(view), &text_color);
	GtkTextIter line;
	gtk_text_view_get_line_at_y(view, &line, visible.y, NULL);
	do {
		int y = 0, height = 0;
		gtk_text_view_get_line_yrange(view, &line, &y, &height);
		if (y > visible.y + visible.height)
			break;
		if (height > 0)
			draw_line_decorations(view, snapshot, &line, y, height, &text_color);
	} while (gtk_text_iter_forward_line(&line));
}


/* GTK keeps each widget's measured size, so frames forget theirs when the text width they follow changes */
static void allocate_view(GtkWidget *widget, int width, int height, int baseline) {
	DocumentView *document = DOCUMENT_VIEW(widget);
	if (document->frame_width != width) {
		document->frame_width = width;
		forget_frame_sizes(widget);
	}
	GTK_WIDGET_CLASS(document_view_parent_class)->size_allocate(widget, width, height, baseline);
}


static void dispose_view(GObject *object) {
	live_views = g_list_remove(live_views, object);
	G_OBJECT_CLASS(document_view_parent_class)->dispose(object);
}


static void document_view_class_init(DocumentViewClass *class) {
	G_OBJECT_CLASS(class)->dispose = dispose_view;
	GTK_TEXT_VIEW_CLASS(class)->snapshot_layer = snapshot_layer;
	GTK_WIDGET_CLASS(class)->size_allocate = allocate_view;
}


static void document_view_init(DocumentView *document) {
	GtkTextView *view = GTK_TEXT_VIEW(document);
	create_fixed_tags(gtk_text_view_get_buffer(view));
	split_long_lines(gtk_text_view_get_buffer(view));
	join_long_line_copies(view);
	apply_theme_colors(view);
	AdwStyleManager *styles = adw_style_manager_get_default();
	g_signal_connect_object(styles, "notify::dark", G_CALLBACK(apply_theme_colors), view, G_CONNECT_SWAPPED);
	g_signal_connect_object(styles, "notify::accent-color", G_CALLBACK(apply_theme_colors), view,
		G_CONNECT_SWAPPED);
	live_views = g_list_prepend(live_views, document);
}


GtkWidget *create_document_view(void) {
	GtkWidget *view = g_object_new(DOCUMENT_VIEW_TYPE, NULL);
	gtk_text_view_set_editable(GTK_TEXT_VIEW(view), FALSE);
	gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(view), FALSE);
	gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view), GTK_WRAP_WORD_CHAR);
	gtk_text_view_set_left_margin(GTK_TEXT_VIEW(view), DOCUMENT_MARGIN);
	gtk_text_view_set_right_margin(GTK_TEXT_VIEW(view), DOCUMENT_MARGIN);
	gtk_text_view_set_top_margin(GTK_TEXT_VIEW(view), TOP_MARGIN);
	gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(view), BOTTOM_MARGIN);
	gtk_text_view_set_pixels_below_lines(GTK_TEXT_VIEW(view), scale_size(BLOCK_GAP));
	gtk_widget_add_css_class(view, "content");
	gtk_widget_add_css_class(view, "document");
	return view;
}


/* Width of the text beside the frame, or nothing before its view has a size */
static int get_frame_width(BlockFrame *frame) {
	GtkWidget *view = gtk_widget_get_ancestor(GTK_WIDGET(frame), GTK_TYPE_TEXT_VIEW);
	int width = view ? gtk_widget_get_width(view) : 0;
	if (width <= 0)
		return -1;
	return MAX(width - DOCUMENT_MARGIN * 2 - scale_size(frame->indent), 1);
}


static void measure_frame(GtkWidget *widget, GtkOrientation orientation, int for_size, int *minimum, int *natural,
	int *minimum_baseline, int *natural_baseline) {
	BlockFrame *frame = BLOCK_FRAME(widget);
	int width = get_frame_width(frame);
	if (orientation == GTK_ORIENTATION_HORIZONTAL) {
		gtk_widget_measure(frame->child, orientation, for_size, minimum, natural, NULL, NULL);
		if (width > 0)
			*minimum = *natural = width;
		return;
	}
	int shown_width = for_size >= 0 ? for_size : width;
	/* A scroller that can scroll sideways measures its content at its narrowest, where every table cell wraps
	 * word by word, so the content is measured at the width it really gets */
	GtkWidget *content = GTK_IS_SCROLLED_WINDOW(frame->child)
		? gtk_scrolled_window_get_child(GTK_SCROLLED_WINDOW(frame->child)) : NULL;
	if (!content || shown_width < 0) {
		gtk_widget_measure(frame->child, orientation, shown_width, minimum, natural, NULL, NULL);
		return;
	}
	int content_natural = 0;
	gtk_widget_measure(content, GTK_ORIENTATION_HORIZONTAL, -1, NULL, &content_natural, NULL, NULL);
	gtk_widget_measure(content, orientation, MIN(shown_width, content_natural), minimum, natural, NULL, NULL);
	*minimum = *natural;
}


static void allocate_frame(GtkWidget *widget, int width, int height, int baseline) {
	gtk_widget_size_allocate(BLOCK_FRAME(widget)->child, &(GtkAllocation){0, 0, width, height}, -1);
}


static void dispose_frame(GObject *object) {
	g_clear_pointer(&BLOCK_FRAME(object)->child, gtk_widget_unparent);
	G_OBJECT_CLASS(block_frame_parent_class)->dispose(object);
}


static void block_frame_class_init(BlockFrameClass *class) {
	G_OBJECT_CLASS(class)->dispose = dispose_frame;
	GTK_WIDGET_CLASS(class)->measure = measure_frame;
	GTK_WIDGET_CLASS(class)->size_allocate = allocate_frame;
}


static void block_frame_init(BlockFrame *frame) {
}


/* Wraps a block shown inside the text, indented by an unzoomed width */
GtkWidget *create_block_frame(GtkWidget *child, int indent) {
	BlockFrame *frame = g_object_new(BLOCK_FRAME_TYPE, NULL);
	frame->child = child;
	frame->indent = indent;
	gtk_widget_set_parent(child, GTK_WIDGET(frame));
	return GTK_WIDGET(frame);
}


/* The line a view is flashing, how strongly at this moment, and the animation that runs it */
typedef struct {
	GtkTextView *view;
	GtkTextMark *mark;
	double strength;
	AdwAnimation *animation;
} Flash;


static void free_flash(gpointer data) {
	Flash *flash = data;
	adw_animation_pause(flash->animation);
	g_object_unref(flash->animation);
	g_free(flash);
}


static void set_flash_strength(double value, gpointer data) {
	Flash *flash = data;
	flash->strength = value;
	gtk_widget_queue_draw(GTK_WIDGET(flash->view));
}


/* Shows where the reader landed by fading a highlight in and out twice behind that line */
void flash_text_line(GtkTextView *view, const GtkTextIter *line_start) {
	Flash *flash = g_object_get_data(G_OBJECT(view), "flash");
	if (!flash) {
		flash = g_new0(Flash, 1);
		flash->view = view;
		AdwAnimationTarget *target = adw_callback_animation_target_new(set_flash_strength, flash, NULL);
		flash->animation = adw_timed_animation_new(GTK_WIDGET(view), 0, 1, FLASH_DURATION, target);
		adw_timed_animation_set_repeat_count(ADW_TIMED_ANIMATION(flash->animation), FLASH_COUNT * 2);
		adw_timed_animation_set_alternate(ADW_TIMED_ANIMATION(flash->animation), TRUE);
		adw_timed_animation_set_easing(ADW_TIMED_ANIMATION(flash->animation), ADW_EASE_IN_OUT_CUBIC);
		g_object_set_data_full(G_OBJECT(view), "flash", flash, free_flash);
	}
	GtkTextBuffer *buffer = gtk_text_view_get_buffer(view);
	if (!flash->mark)
		flash->mark = gtk_text_buffer_create_mark(buffer, NULL, line_start, TRUE);
	gtk_text_buffer_move_mark(buffer, flash->mark, line_start);
	adw_animation_reset(flash->animation);
	adw_animation_play(flash->animation);
}


/* Draws the flash as a rounded highlight in the accent colour, called from a view's layer below its text */
void draw_text_flash(GtkTextView *view, GtkSnapshot *snapshot) {
	Flash *flash = g_object_get_data(G_OBJECT(view), "flash");
	if (!flash || !flash->mark || flash->strength <= 0)
		return;
	GtkTextIter line;
	gtk_text_buffer_get_iter_at_mark(gtk_text_view_get_buffer(view), &line, flash->mark);
	int y = 0, height = 0;
	gtk_text_view_get_line_yrange(view, &line, &y, &height);
	g_autoptr(GSList) tags = gtk_text_iter_get_tags(&line);
	int left = gtk_text_view_get_left_margin(view) - FLASH_PADDING;
	int width = gtk_widget_get_width(GTK_WIDGET(view)) - gtk_text_view_get_left_margin(view)
		- gtk_text_view_get_right_margin(view) + FLASH_PADDING * 2;
	graphene_rect_t area = GRAPHENE_RECT_INIT(left, y - FLASH_PADDING / 2.0f, width,
		height - get_line_gap(tags) + FLASH_PADDING);
	GskRoundedRect rounded;
	gsk_rounded_rect_init_from_rect(&rounded, &area, FLASH_RADIUS);
	g_autoptr(GdkRGBA) accent = adw_style_manager_get_accent_color_rgba(adw_style_manager_get_default());
	accent->alpha = FLASH_ALPHA * flash->strength;
	gtk_snapshot_push_rounded_clip(snapshot, &rounded);
	gtk_snapshot_append_color(snapshot, accent, &area);
	gtk_snapshot_pop(snapshot);
}
