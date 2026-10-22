#include <stdlib.h>
#include <string.h>

#include <pango/pango.h>

#include "inline.h"
#include "md4c/entity.h"

#define MAXIMUM_ENTITY_BYTES 32
#define MAXIMUM_TAG_NAME_BYTES 16

/* HTML tags that map straight onto Pango markup */
static const struct {
	const char *name, *open, *close;
	gboolean ends_line;
} SIMPLE_TAGS[] = {
	{"b", "<b>", "</b>", FALSE}, {"strong", "<b>", "</b>", FALSE},
	{"i", "<i>", "</i>", FALSE}, {"em", "<i>", "</i>", FALSE}, {"cite", "<i>", "</i>", FALSE},
	{"s", "<s>", "</s>", FALSE}, {"del", "<s>", "</s>", FALSE}, {"strike", "<s>", "</s>", FALSE},
	{"u", "<u>", "</u>", FALSE}, {"ins", "<u>", "</u>", FALSE},
	{"sub", "<sub>", "</sub>", FALSE}, {"sup", "<sup>", "</sup>", FALSE},
	{"small", "<small>", "</small>", FALSE}, {"mark", HIGHLIGHT_OPEN, "</span>", FALSE},
	{"code", CODE_OPEN, "</span>", FALSE}, {"kbd", CODE_OPEN, "</span>", FALSE},
	{"tt", CODE_OPEN, "</span>", FALSE}, {"samp", CODE_OPEN, "</span>", FALSE},
	{"summary", "<b>", "</b>", TRUE},
	{"h1", "<span size=\"200%\" weight=\"bold\">", "</span>", TRUE},
	{"h2", "<span size=\"150%\" weight=\"bold\">", "</span>", TRUE},
	{"h3", "<span size=\"125%\" weight=\"bold\">", "</span>", TRUE},
	{"h4", "<b>", "</b>", TRUE},
	{"h5", "<span size=\"90%\" weight=\"bold\">", "</span>", TRUE},
	{"h6", "<span size=\"85%\" weight=\"bold\">", "</span>", TRUE},
};

/* HTML tags that only separate lines of text */
static const char *const BREAK_TAGS[] = {
	"p", "div", "center", "table", "tr", "details", "blockquote", "ul", "ol", "dl", "dt", "dd",
	"section", "figure", "figcaption", "picture", "article", "header", "footer", "hr",
};


static void free_span(gpointer data) {
	OpenSpan *span = data;
	g_free(span->name);
	g_free(span->open);
	g_free(span->close);
	g_free(span->link);
	g_free(span);
}


void start_inline(InlineWriter *writer, Block *block) {
	if (!writer->markup) {
		writer->markup = g_string_new(NULL);
		writer->plain = g_string_new(NULL);
		writer->image_text = g_string_new(NULL);
		writer->spans = g_ptr_array_new_with_free_func(free_span);
	}
	g_string_truncate(writer->markup, 0);
	g_string_truncate(writer->plain, 0);
	g_ptr_array_set_size(writer->spans, 0);
	writer->block = block;
	writer->image = NULL;
	writer->is_highlight_open = FALSE;
}


void clear_inline_writer(InlineWriter *writer) {
	if (!writer->markup)
		return;
	g_string_free(writer->markup, TRUE);
	g_string_free(writer->plain, TRUE);
	g_string_free(writer->image_text, TRUE);
	g_ptr_array_unref(writer->spans);
	writer->markup = NULL;
}


/* Closes a span, or drops its opening tag when nothing was written inside it */
static void append_close(InlineWriter *writer, const OpenSpan *span) {
	if (!span->is_written)
		return;
	gsize open_length = strlen(span->open);
	if (writer->markup->len >= open_length && g_str_has_suffix(writer->markup->str, span->open))
		g_string_truncate(writer->markup, writer->markup->len - open_length);
	else
		g_string_append(writer->markup, span->close);
}


static void close_open_spans(InlineWriter *writer) {
	for (guint index = writer->spans->len; index > 0; index--)
		append_close(writer, g_ptr_array_index(writer->spans, index - 1));
}


static void add_markup_piece(InlineWriter *writer) {
	if (writer->markup->len > 0) {
		Piece *piece = g_new0(Piece, 1);
		piece->markup = g_strdup(writer->markup->str);
		g_ptr_array_add(writer->block->pieces, piece);
	}
	g_string_truncate(writer->markup, 0);
}


/* Ends the current markup piece and reopens its spans so a picture can sit between two runs */
static void flush_markup(InlineWriter *writer) {
	close_open_spans(writer);
	add_markup_piece(writer);
	for (guint index = 0; index < writer->spans->len; index++) {
		OpenSpan *span = g_ptr_array_index(writer->spans, index);
		if (span->is_written)
			g_string_append(writer->markup, span->open);
	}
}


void finish_inline(InlineWriter *writer) {
	if (!writer->block)
		return;
	if (writer->image)
		finish_image(writer);
	close_open_spans(writer);
	add_markup_piece(writer);
	g_ptr_array_set_size(writer->spans, 0);
	writer->block = NULL;
}


/* Labels read the title as markup, so the shown address is escaped twice to stay plain text */
char *create_link_open(const char *link, const char *shown) {
	g_autofree char *escaped = g_markup_escape_text(link, -1);
	g_autofree char *title_markup = g_markup_escape_text(shown, -1);
	g_autofree char *title = g_markup_escape_text(title_markup, -1);
	return g_strdup_printf("<a href=\"%s\" title=\"%s\">", escaped, title);
}


void open_span(InlineWriter *writer, const char *name, const char *open, const char *close, const char *link) {
	OpenSpan *span = g_new0(OpenSpan, 1);
	span->name = g_strdup(name);
	span->open = g_strdup(open);
	span->close = g_strdup(close);
	span->link = g_strdup(link);
	span->is_written = !writer->image;
	g_ptr_array_add(writer->spans, span);
	if (span->is_written)
		g_string_append(writer->markup, open);
}


/* Closes the newest span with this name, closing and reopening any spans opened after it */
void close_span(InlineWriter *writer, const char *name) {
	guint found = writer->spans->len;
	while (found > 0 && g_strcmp0(((OpenSpan *)g_ptr_array_index(writer->spans, found - 1))->name, name) != 0)
		found--;
	if (found == 0)
		return;
	found--;
	for (guint index = writer->spans->len; index > found; index--)
		append_close(writer, g_ptr_array_index(writer->spans, index - 1));
	g_ptr_array_remove_index(writer->spans, found);
	for (guint index = found; index < writer->spans->len; index++) {
		OpenSpan *span = g_ptr_array_index(writer->spans, index);
		if (span->is_written)
			g_string_append(writer->markup, span->open);
	}
}


static const char *get_innermost_link(InlineWriter *writer) {
	for (guint index = writer->spans->len; index > 0; index--) {
		OpenSpan *span = g_ptr_array_index(writer->spans, index - 1);
		if (span->link)
			return span->link;
	}
	return NULL;
}


void start_image(InlineWriter *writer, const char *source) {
	if (writer->image)
		return;
	flush_markup(writer);
	writer->image = g_new0(Piece, 1);
	writer->image->image_source = g_strdup(source);
	writer->image->image_link = g_strdup(get_innermost_link(writer));
	g_string_truncate(writer->image_text, 0);
}


void finish_image(InlineWriter *writer) {
	if (!writer->image)
		return;
	writer->image->image_text = g_strdup(writer->image_text->str);
	g_ptr_array_add(writer->block->pieces, writer->image);
	writer->image = NULL;
}


void write_text(InlineWriter *writer, const char *text, gsize size) {
	if (writer->image) {
		g_string_append_len(writer->image_text, text, size);
		return;
	}
	g_autofree char *escaped = g_markup_escape_text(text, size);
	g_string_append(writer->markup, escaped);
	g_string_append_len(writer->plain, text, size);
}


void write_markup(InlineWriter *writer, const char *markup) {
	if (!writer->image)
		g_string_append(writer->markup, markup);
}


/* Turns each == marker into the start or end of a highlight */
void write_highlighted_text(InlineWriter *writer, const char *text, gsize size) {
	gsize start = 0;
	for (gsize index = 0; index + 1 < size; index++) {
		if (text[index] != '=' || text[index + 1] != '=')
			continue;
		write_text(writer, text + start, index - start);
		if (writer->is_highlight_open)
			close_span(writer, "mark");
		else
			open_span(writer, "mark", HIGHLIGHT_OPEN, "</span>", NULL);
		writer->is_highlight_open = !writer->is_highlight_open;
		index++;
		start = index + 1;
	}
	write_text(writer, text + start, size - start);
}


void decode_entity(const char *entity, gsize size, GString *output) {
	if (size > 3 && entity[1] == '#') {
		gboolean is_hex = entity[2] == 'x' || entity[2] == 'X';
		gunichar character = (gunichar)g_ascii_strtoull(entity + (is_hex ? 3 : 2), NULL, is_hex ? 16 : 10);
		g_string_append_unichar(output, g_unichar_validate(character) && character ? character : 0xFFFD);
		return;
	}
	const ENTITY *found = entity_lookup(entity, size);
	if (!found) {
		g_string_append_len(output, entity, size);
		return;
	}
	g_string_append_unichar(output, found->codepoints[0]);
	if (found->codepoints[1])
		g_string_append_unichar(output, found->codepoints[1]);
}


char *decode_entities(const char *text, gsize size) {
	GString *output = g_string_sized_new(size);
	for (gsize index = 0; index < size; index++) {
		const char *end = NULL;
		if (text[index] == '&')
			end = memchr(text + index, ';', MIN(size - index, MAXIMUM_ENTITY_BYTES));
		if (!end) {
			g_string_append_c(output, text[index]);
			continue;
		}
		decode_entity(text + index, end - (text + index) + 1, output);
		index = end - text;
	}
	return g_string_free(output, FALSE);
}


/* Reads one attribute value from a raw tag, decoding its entities */
static char *get_attribute(const char *tag, gsize size, const char *name) {
	gsize name_length = strlen(name);
	for (gsize index = 1; index + name_length < size; index++) {
		if (!g_ascii_isspace(tag[index - 1]) || g_ascii_strncasecmp(tag + index, name, name_length) != 0)
			continue;
		gsize position = index + name_length;
		while (position < size && g_ascii_isspace(tag[position]))
			position++;
		if (position >= size || tag[position] != '=')
			continue;
		position++;
		while (position < size && g_ascii_isspace(tag[position]))
			position++;
		char quote = position < size && (tag[position] == '"' || tag[position] == '\'') ? tag[position] : 0;
		if (quote)
			position++;
		gsize end = position;
		while (end < size && (quote ? tag[end] != quote : !g_ascii_isspace(tag[end]) && tag[end] != '>'))
			end++;
		return decode_entities(tag + position, end - position);
	}
	return NULL;
}


static gboolean is_break_tag(const char *name) {
	for (guint index = 0; index < G_N_ELEMENTS(BREAK_TAGS); index++)
		if (strcmp(name, BREAK_TAGS[index]) == 0)
			return TRUE;
	return FALSE;
}


static void write_html_image(InlineWriter *writer, const char *tag, gsize size) {
	g_autofree char *source = get_attribute(tag, size, "src");
	if (!source)
		return;
	g_autofree char *text = get_attribute(tag, size, "alt");
	g_autofree char *width = get_attribute(tag, size, "width");
	start_image(writer, source);
	if (text)
		g_string_append(writer->image_text, text);
	if (width && atoi(width) > 0)
		writer->image->image_width = strchr(width, '%') ? -atoi(width) : atoi(width);
	finish_image(writer);
}


/* A link without an address but with an id or name is a place other links jump to, as READMEs use them */
static void write_html_link(InlineWriter *writer, const char *tag, gsize size) {
	g_autofree char *link = get_attribute(tag, size, "href");
	g_autofree char *anchor = link ? NULL : get_attribute(tag, size, "id");
	if (!link && !anchor)
		anchor = get_attribute(tag, size, "name");
	if (anchor && *anchor) {
		g_autofree char *lowered = g_utf8_strdown(anchor, -1);
		add_block_anchor(writer->block, lowered);
	}
	if (!link)
		return;
	g_autofree char *open = create_link_open(link, link);
	open_span(writer, "a", open, "</a>", link);
}


void write_html_tag(InlineWriter *writer, const char *tag, gsize size) {
	if (size < 3 || g_str_has_prefix(tag, "<!"))
		return;
	gboolean is_closing = tag[1] == '/';
	char name[MAXIMUM_TAG_NAME_BYTES] = {0};
	for (gsize index = is_closing ? 2 : 1, length = 0; index < size && length + 1 < sizeof name; index++) {
		if (!g_ascii_isalnum(tag[index]))
			break;
		name[length++] = g_ascii_tolower(tag[index]);
	}
	for (guint index = 0; index < G_N_ELEMENTS(SIMPLE_TAGS); index++) {
		if (strcmp(name, SIMPLE_TAGS[index].name) != 0)
			continue;
		if (is_closing)
			close_span(writer, name);
		else
			open_span(writer, name, SIMPLE_TAGS[index].open, SIMPLE_TAGS[index].close, NULL);
		if (is_closing && SIMPLE_TAGS[index].ends_line)
			write_markup(writer, "\n");
		return;
	}
	if (strcmp(name, "br") == 0) {
		write_markup(writer, "\n");
	} else if (strcmp(name, "img") == 0 && !is_closing) {
		write_html_image(writer, tag, size);
	} else if (strcmp(name, "a") == 0) {
		if (is_closing)
			close_span(writer, "a");
		else
			write_html_link(writer, tag, size);
	} else if (strcmp(name, "li") == 0 && !is_closing) {
		write_markup(writer, "\n• ");
	} else if (is_break_tag(name)) {
		g_autofree char *align = is_closing ? NULL : get_attribute(tag, size, "align");
		if (strcmp(name, "center") == 0 || g_strcmp0(align, "center") == 0)
			writer->block->centered = TRUE;
		write_markup(writer, "\n");
	}
}


/* Writes a raw HTML block, keeping its text, pictures, links and simple formatting */
void write_html(InlineWriter *writer, const char *html, gsize size) {
	gsize index = 0;
	while (index < size) {
		if (html[index] == '<' && index + 3 < size && strncmp(html + index, "<!--", 4) == 0) {
			const char *end = g_strstr_len(html + index, size - index, "-->");
			index = end ? (gsize)(end - html) + 3 : size;
			continue;
		}
		if (html[index] == '<') {
			gsize end = index + 1;
			char quote = 0;
			while (end < size && (quote || html[end] != '>')) {
				if (html[end] == '"' || html[end] == '\'')
					quote = quote == html[end] ? 0 : quote ? quote : html[end];
				end++;
			}
			write_html_tag(writer, html + index, MIN(end + 1, size) - index);
			index = end + 1;
			continue;
		}
		gsize end = index;
		while (end < size && html[end] != '<')
			end++;
		g_autofree char *decoded = decode_entities(html + index, end - index);
		GString *text = g_string_new(NULL);
		for (const char *character = decoded; *character; character++) {
			if (!g_ascii_isspace(*character))
				g_string_append_c(text, *character);
			else if (text->len == 0 || text->str[text->len - 1] != ' ')
				g_string_append_c(text, ' ');
		}
		write_text(writer, text->str, text->len);
		g_string_free(text, TRUE);
		index = end;
	}
}


static gboolean is_blank(const char *text) {
	for (; *text; text++)
		if (!g_ascii_isspace(*text))
			return FALSE;
	return TRUE;
}


/* Removes space at both ends, looking past opening tags at the start and closing tags at the end */
static void trim_markup(GString *markup) {
	gsize position = 0;
	while (position < markup->len) {
		const char *tag_end = strchr(markup->str + position, '>');
		if (g_ascii_isspace(markup->str[position]))
			g_string_erase(markup, position, 1);
		else if (markup->str[position] == '<' && markup->str[position + 1] != '/' && tag_end)
			position = tag_end - markup->str + 1;
		else
			break;
	}
	position = markup->len;
	while (position > 0) {
		const char *tag_start = g_strrstr_len(markup->str, position, "<");
		if (g_ascii_isspace(markup->str[position - 1])) {
			g_string_erase(markup, position - 1, 1);
			position--;
		} else if (markup->str[position - 1] == '>' && tag_start && tag_start[1] == '/') {
			position = tag_start - markup->str;
		} else {
			break;
		}
	}
}


/* Turns soft break markers into spaces and trims space around line ends */
static char *tidy_markup(const char *markup) {
	GString *tidy = g_string_new(NULL);
	gboolean is_line_start = TRUE;
	for (const char *character = markup; *character; character++) {
		const char *text = (char[]){*character == SOFT_BREAK ? ' ' : *character, 0};
		for (; *text; text++) {
			if (*text == '\n') {
				while (tidy->len > 0 && tidy->str[tidy->len - 1] == ' ')
					g_string_truncate(tidy, tidy->len - 1);
				gboolean is_double = tidy->len > 1 && tidy->str[tidy->len - 1] == '\n'
					&& tidy->str[tidy->len - 2] == '\n';
				if (tidy->len > 0 && !is_double)
					g_string_append_c(tidy, '\n');
				is_line_start = TRUE;
			} else if (*text != ' ' || !is_line_start) {
				g_string_append_c(tidy, *text);
				is_line_start = FALSE;
			}
		}
	}
	trim_markup(tidy);
	return g_string_free(tidy, FALSE);
}


static char *strip_tags(const char *markup) {
	GString *text = g_string_new(NULL);
	gboolean is_inside_tag = FALSE;
	for (const char *character = markup; *character; character++) {
		if (*character == '<' || *character == '>')
			is_inside_tag = *character == '<';
		else if (!is_inside_tag)
			g_string_append_c(text, *character);
	}
	return g_string_free(text, FALSE);
}


/* Checks markup the way a label will read it, where links are a label feature unknown to Pango */
gboolean get_markup_text(const char *markup, char **text) {
	GString *checked = g_string_new(NULL);
	for (const char *character = markup; *character; character++) {
		gboolean is_link_tag = g_str_has_prefix(character, "<a ") || g_str_has_prefix(character, "</a>");
		const char *tag_end = is_link_tag ? strchr(character, '>') : NULL;
		if (tag_end)
			character = tag_end;
		else
			g_string_append_c(checked, *character);
	}
	gboolean is_valid = pango_parse_markup(checked->str, -1, 0, NULL, text, NULL, NULL);
	g_string_free(checked, TRUE);
	return is_valid;
}


void settle_pieces(Block *block) {
	for (guint index = block->pieces->len; index > 0; index--) {
		Piece *piece = g_ptr_array_index(block->pieces, index - 1);
		if (!piece->markup)
			continue;
		char *tidy = tidy_markup(piece->markup);
		g_free(piece->markup);
		piece->markup = tidy;
		g_autofree char *visible = NULL;
		if (!get_markup_text(tidy, &visible)) {
			piece->markup = strip_tags(tidy);
			g_free(tidy);
			visible = g_strdup(piece->markup);
		}
		if (is_blank(visible))
			g_ptr_array_remove_index(block->pieces, index - 1);
	}
}
