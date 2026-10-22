#pragma once

#include <glib.h>

#include "document.h"

/* Marks a soft line break until the style decides whether it shows */
#define SOFT_BREAK '\x1f'

#define CODE_OPEN "<span font_family=\"monospace\" background=\"#808080\" bgalpha=\"18%\">"
#define HIGHLIGHT_OPEN "<span background=\"#ffd33d\" bgalpha=\"45%\">"

/* An open inline span with the markup that opens and closes it */
typedef struct {
	char *name, *open, *close, *link;
	/* Spans opened inside picture text never reach the markup, so they must not be closed there */
	gboolean is_written;
} OpenSpan;

/* Collects Pango markup and pictures for the block currently receiving inline content */
typedef struct {
	Block *block;
	GString *markup, *plain;
	GPtrArray *spans;
	/* Picture whose alternative text is being collected */
	Piece *image;
	GString *image_text;
	gboolean is_highlight_open;
} InlineWriter;

void start_inline(InlineWriter *writer, Block *block);
void finish_inline(InlineWriter *writer);
void clear_inline_writer(InlineWriter *writer);
void settle_pieces(Block *block);
gboolean get_markup_text(const char *markup, char **text);
char *create_link_open(const char *link, const char *shown);
void open_span(InlineWriter *writer, const char *name, const char *open, const char *close, const char *link);
void close_span(InlineWriter *writer, const char *name);
void start_image(InlineWriter *writer, const char *source);
void finish_image(InlineWriter *writer);
void write_text(InlineWriter *writer, const char *text, gsize size);
void write_markup(InlineWriter *writer, const char *markup);
void write_highlighted_text(InlineWriter *writer, const char *text, gsize size);
void write_html_tag(InlineWriter *writer, const char *tag, gsize size);
void write_html(InlineWriter *writer, const char *html, gsize size);
void decode_entity(const char *entity, gsize size, GString *output);
char *decode_entities(const char *text, gsize size);
