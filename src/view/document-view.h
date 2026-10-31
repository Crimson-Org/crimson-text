#pragma once

#include <gtk/gtk.h>

/* Room between the view's edges and its text, before any indentation */
#define DOCUMENT_MARGIN 28
/* Space below a block and below a line inside a list, before zooming */
#define BLOCK_GAP 16
#define LIST_GAP 6
/* Room around the text of a code block inside its background */
#define CODE_PADDING 16
/* Smaller text, such as small print, task counts and table notes */
#define SMALL_SCALE 0.833
/* Separates lines inside one paragraph, so the paragraph is one line of the buffer with one gap below */
#define LINE_SEPARATOR "\u2028"

#define DOCUMENT_VIEW_TYPE (document_view_get_type())
G_DECLARE_FINAL_TYPE(DocumentView, document_view, DOCUMENT, VIEW, GtkTextView)

GtkWidget *create_document_view(void);
GtkWidget *create_block_frame(GtkWidget *child, int indent);
GtkTextTag *get_indent_tag(GtkTextBuffer *buffer, int left, int hang);
GtkTextTag *get_bar_tag(GtkTextBuffer *buffer, int x, const char *color);
const char *get_alert_color(const char *family);
void set_document_zoom(int percent);
gboolean is_text_hidden(const GtkTextIter *iter);
void flash_text_line(GtkTextView *view, const GtkTextIter *line_start);
void draw_text_flash(GtkTextView *view, GtkSnapshot *snapshot);
