#include <string.h>

#include "lang/en.h"
#include "document/footnotes.h"
#include "document/inline.h"

#define MAXIMUM_DEFINITION_INDENT 3
#define CONTINUATION_INDENT 4
#define REFERENCE_OPEN "[^"
#define REFERENCE_OPEN_LENGTH (sizeof REFERENCE_OPEN - 1)
/* What ends a definition's label, as in [^note]: Text */
#define DEFINITION_CLOSE_LENGTH (sizeof "]:" - 1)
/* The shortest definition, such as [^a]: */
#define SHORTEST_DEFINITION_LENGTH (REFERENCE_OPEN_LENGTH + 1 + DEFINITION_CLOSE_LENGTH)


static void free_footnote(gpointer data) {
	Footnote *footnote = data;
	g_free(footnote->label);
	g_free(footnote->text);
	g_free(footnote);
}


static Footnote *find_footnote(Footnotes *footnotes, const char *label, gsize length) {
	for (guint index = 0; index < footnotes->notes->len; index++) {
		Footnote *footnote = g_ptr_array_index(footnotes->notes, index);
		if (strlen(footnote->label) == length && strncmp(footnote->label, label, length) == 0)
			return footnote;
	}
	return NULL;
}


static gboolean is_fence(const char *line, gsize size) {
	gsize indent = 0;
	while (indent < size && indent <= MAXIMUM_DEFINITION_INDENT && line[indent] == ' ')
		indent++;
	return starts_code_fence(line + indent, size - indent);
}


/* Reads a definition such as [^note]: Text, giving its label and where its text starts */
static gboolean read_definition(const char *line, gsize size, gsize *label_start, gsize *label_length,
	gsize *text_start) {
	gsize position = 0;
	while (position < size && position < MAXIMUM_DEFINITION_INDENT && line[position] == ' ')
		position++;
	if (size - position < SHORTEST_DEFINITION_LENGTH
		|| strncmp(line + position, REFERENCE_OPEN, REFERENCE_OPEN_LENGTH) != 0)
		return FALSE;
	gsize start = position + REFERENCE_OPEN_LENGTH, end = start;
	while (end < size && line[end] != ']' && !g_ascii_isspace(line[end]))
		end++;
	if (end == start || end + 1 >= size || line[end] != ']' || line[end + 1] != ':')
		return FALSE;
	*label_start = start;
	*label_length = end - start;
	*text_start = end + DEFINITION_CLOSE_LENGTH;
	return TRUE;
}


static gboolean is_continuation(const char *line, gsize size) {
	gboolean is_indented = (size > 0 && line[0] == '\t')
		|| (size >= CONTINUATION_INDENT && strncmp(line, "    ", CONTINUATION_INDENT) == 0);
	for (gsize index = 0; is_indented && index < size; index++)
		if (!g_ascii_isspace(line[index]))
			return TRUE;
	return FALSE;
}


/* Collects footnote definitions and returns a copy of the text with them blanked by spaces
 * Every byte keeps its place, so block lines and task offsets still point into the real file */
char *take_footnotes(const char *text, gsize length, gsize start, Footnotes *footnotes) {
	footnotes->notes = g_ptr_array_new_with_free_func(free_footnote);
	footnotes->next_number = 1;
	char *blanked = g_strndup(text, length);
	gboolean is_in_fence = FALSE;
	int line_number = 1;
	for (gsize index = 0; index < start; index++)
		line_number += text[index] == '\n';
	Footnote *open = NULL;
	for (const char *line = text + start; line < text + length; line_number++) {
		const char *line_end = memchr(line, '\n', text + length - line);
		gsize size = (line_end ? line_end : text + length) - line;
		gsize label_start = 0, label_length = 0, text_start = 0;
		gboolean is_blanked = FALSE;
		if (is_fence(line, size)) {
			is_in_fence = !is_in_fence;
			open = NULL;
		} else if (!is_in_fence && read_definition(line, size, &label_start, &label_length, &text_start)) {
			open = g_new0(Footnote, 1);
			open->label = g_strndup(line + label_start, label_length);
			open->text = g_strstrip(g_strndup(line + text_start, size - text_start));
			open->line = line_number;
			if (find_footnote(footnotes, open->label, label_length))
				g_clear_pointer(&open, free_footnote);
			else
				g_ptr_array_add(footnotes->notes, open);
			is_blanked = TRUE;
		} else if (open && is_continuation(line, size)) {
			g_autofree char *more = g_strstrip(g_strndup(line, size));
			char *joined = g_strconcat(open->text, " ", more, NULL);
			g_free(open->text);
			open->text = joined;
			is_blanked = TRUE;
		} else {
			open = NULL;
		}
		if (is_blanked)
			memset(blanked + (line - text), ' ', size);
		if (!line_end)
			break;
		line = line_end + 1;
	}
	return blanked;
}


static char *create_footnote_anchor(const char *prefix, const char *label) {
	g_autofree char *lowered = g_utf8_strdown(label, -1);
	return g_strconcat(prefix, lowered, NULL);
}


/* Turns references such as [^note] into numbered superscript links that show the note on hover */
static char *link_references(Footnotes *footnotes, Block *block, const char *markup) {
	GString *linked = g_string_new(NULL);
	const char *rest = markup;
	for (const char *found = strstr(rest, REFERENCE_OPEN); found; found = strstr(rest, REFERENCE_OPEN)) {
		const char *label = found + REFERENCE_OPEN_LENGTH;
		const char *close = strchr(label, ']');
		Footnote *footnote = close ? find_footnote(footnotes, label, close - label) : NULL;
		g_string_append_len(linked, rest, (footnote ? found : label) - rest);
		rest = footnote ? close + 1 : label;
		if (!footnote)
			continue;
		if (!footnote->number)
			footnote->number = footnotes->next_number++;
		g_autofree char *target = create_footnote_anchor("#fn-", footnote->label);
		g_autofree char *open = create_link_open(target, footnote->text);
		g_string_append_printf(linked, "<sup>%s%d</a></sup>", open, footnote->number);
		g_autofree char *back = create_footnote_anchor("fnref-", footnote->label);
		add_block_anchor(block, back);
	}
	g_string_append(linked, rest);
	return g_string_free(linked, FALSE);
}


void link_footnote_references(Footnotes *footnotes, Block *block) {
	if (!footnotes->notes || footnotes->notes->len == 0)
		return;
	for (guint index = 0; index < block->pieces->len; index++) {
		Piece *piece = g_ptr_array_index(block->pieces, index);
		if (!piece->markup || !strstr(piece->markup, REFERENCE_OPEN))
			continue;
		char *linked = link_references(footnotes, block, piece->markup);
		g_free(piece->markup);
		piece->markup = linked;
	}
}


static void keep_first_block(Block *block, gpointer data) {
	Block **first = data;
	if (!*first && block->kind == BLOCK_PARAGRAPH)
		*first = block;
	else
		free_block(block);
}


/* Parses a note's own Markdown into one paragraph ending with a link back to where the text refers to it */
static Block *create_note_paragraph(Footnote *footnote) {
	Block *paragraph = NULL;
	parse_markdown(footnote->text, strlen(footnote->text), keep_first_block, &paragraph, NULL);
	if (!paragraph)
		paragraph = create_block(BLOCK_PARAGRAPH);
	paragraph->line = footnote->line;
	g_autofree char *anchor = create_footnote_anchor("fn-", footnote->label);
	add_block_anchor(paragraph, anchor);
	g_autofree char *back = create_footnote_anchor("#fnref-", footnote->label);
	g_autofree char *open = create_link_open(back, TEXT_FOOTNOTE_BACK);
	Piece *last = paragraph->pieces->len ? g_ptr_array_index(paragraph->pieces, paragraph->pieces->len - 1) : NULL;
	if (!last || !last->markup) {
		last = g_new0(Piece, 1);
		last->markup = g_strdup("");
		g_ptr_array_add(paragraph->pieces, last);
	}
	char *markup = g_strconcat(last->markup, *last->markup ? " " : "", open, "↩</a>", NULL);
	g_free(last->markup);
	last->markup = markup;
	return paragraph;
}


static int compare_numbers(gconstpointer first, gconstpointer second) {
	return (*(Footnote *const *)first)->number - (*(Footnote *const *)second)->number;
}


/* Lists the notes after the document in reference order, with notes nothing refers to last */
void emit_footnotes(Footnotes *footnotes, BlockReady ready, gpointer data) {
	if (!footnotes->notes || footnotes->notes->len == 0)
		return;
	for (guint index = 0; index < footnotes->notes->len; index++) {
		Footnote *footnote = g_ptr_array_index(footnotes->notes, index);
		if (!footnote->number)
			footnote->number = footnotes->next_number++;
	}
	g_ptr_array_sort(footnotes->notes, compare_numbers);
	ready(create_block(BLOCK_RULE), data);
	Block *list = create_block(BLOCK_LIST);
	list->ordered = TRUE;
	list->level = 1;
	for (guint index = 0; index < footnotes->notes->len; index++) {
		Block *item = create_block(BLOCK_ITEM);
		add_child_block(item, create_note_paragraph(g_ptr_array_index(footnotes->notes, index)));
		add_child_block(list, item);
	}
	ready(list, data);
}


void clear_footnotes(Footnotes *footnotes) {
	g_clear_pointer(&footnotes->notes, g_ptr_array_unref);
}
