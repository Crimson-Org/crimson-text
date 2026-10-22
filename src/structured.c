#include <string.h>

#include "parse.h"

#define TASK_INDENT_SPACES 4
#define MAXIMUM_TASK_DEPTH 8
#define TABLE_BATCH_ROWS 500
#define JSON_INDENT "  "
/* A JSON file whose lines average longer than this is laid out before showing */
#define MINIFIED_LINE_BYTES 300


/* Reads the leading indentation in levels, counting a tab or four spaces as one */
static int get_indent_depth(const char *line, const char **text) {
	int spaces = 0;
	const char *character = line;
	for (; *character == ' ' || *character == '\t'; character++)
		spaces += *character == '\t' ? TASK_INDENT_SPACES : 1;
	*text = character;
	return MIN(spaces / TASK_INDENT_SPACES, MAXIMUM_TASK_DEPTH - 1);
}


/* Recognises [ ] and [x] tasks, optionally after a list bullet, and points at the mark between the brackets */
static gboolean read_task(const char *text, gboolean *is_done, const char **title, const char **mark) {
	if ((*text == '-' || *text == '*') && text[1] == ' ')
		text += 2;
	if (text[0] != '[' || (text[1] != ' ' && text[1] != 'x' && text[1] != 'X') || text[2] != ']')
		return FALSE;
	*is_done = text[1] != ' ';
	*mark = text + 1;
	*title = text[3] == ' ' ? text + 4 : text + 3;
	return TRUE;
}


gboolean looks_like_tasks(const char *text, gsize length) {
	g_autofree char *copy = g_strndup(text, length);
	g_auto(GStrv) lines = g_strsplit(copy, "\n", -1);
	int task_count = 0, line_count = 0;
	for (int index = 0; lines[index]; index++) {
		const char *content = NULL, *title = NULL, *mark = NULL;
		gboolean is_done = FALSE;
		get_indent_depth(lines[index], &content);
		if (!*g_strstrip(lines[index]))
			continue;
		line_count++;
		task_count += read_task(content, &is_done, &title, &mark);
	}
	return task_count >= 2 && task_count * 2 >= line_count;
}


static void add_text_piece(Block *block, const char *text) {
	Piece *piece = g_new0(Piece, 1);
	piece->markup = g_markup_escape_text(text, -1);
	g_ptr_array_add(block->pieces, piece);
}


/* Ends a category, telling its heading how many of its tasks are done */
static void finish_task_group(Block **heading, Block **list, int done, int total, BlockReady ready, gpointer data) {
	if (*heading) {
		(*heading)->task_done = done;
		(*heading)->task_total = total;
		ready(*heading, data);
	}
	if (*list)
		ready(*list, data);
	*heading = NULL;
	*list = NULL;
}


/* Shows a plain task file as headed checklists, nesting items by their indentation */
void split_tasks(const char *text, gsize length, BlockReady ready, gpointer data, GCancellable *cancellable) {
	g_autofree char *copy = g_strndup(text, length);
	g_auto(GStrv) lines = g_strsplit(copy, "\n", -1);
	Block *heading = NULL, *top_list = NULL;
	Block *lists[MAXIMUM_TASK_DEPTH] = {0}, *items[MAXIMUM_TASK_DEPTH] = {0};
	int done = 0, total = 0, base_depth = 0;
	gsize line_offset = 0;
	for (int index = 0; lines[index] && !g_cancellable_is_cancelled(cancellable); index++) {
		const char *content = NULL, *title = NULL, *mark = NULL;
		gsize this_line_offset = line_offset;
		line_offset += strlen(lines[index]) + 1;
		gboolean is_done = FALSE;
		int depth = get_indent_depth(lines[index], &content);
		g_strchomp((char *)content);
		if (!*content)
			continue;
		gboolean is_task = read_task(content, &is_done, &title, &mark);
		if (!is_task && depth == 0) {
			finish_task_group(&heading, &top_list, done, total, ready, data);
			done = total = 0;
			heading = create_block(BLOCK_HEADING);
			heading->level = 3;
			heading->line = index + 1;
			heading->text = g_strdup(content);
			add_text_piece(heading, content);
			continue;
		}
		Block *item = create_block(BLOCK_ITEM);
		item->task = is_task;
		item->checked = is_done;
		item->line = index + 1;
		if (is_task)
			item->task_offset = (long)(this_line_offset + (gsize)(mark - lines[index]));
		Block *paragraph = create_block(BLOCK_PARAGRAPH);
		add_text_piece(paragraph, is_task ? title : content);
		add_child_block(item, paragraph);
		done += is_task && is_done;
		total += is_task;
		if (!top_list) {
			base_depth = depth;
			top_list = create_block(BLOCK_LIST);
			top_list->line = index + 1;
			memset(lists, 0, sizeof lists);
			memset(items, 0, sizeof items);
			lists[0] = top_list;
		}
		depth = MAX(depth - base_depth, 0);
		while (depth > 0 && !items[depth - 1])
			depth--;
		if (!lists[depth]) {
			lists[depth] = create_block(BLOCK_LIST);
			add_child_block(items[depth - 1], lists[depth]);
		}
		add_child_block(lists[depth], item);
		items[depth] = item;
		for (int deeper = depth + 1; deeper < MAXIMUM_TASK_DEPTH; deeper++)
			items[deeper] = lists[deeper] = NULL;
	}
	finish_task_group(&heading, &top_list, done, total, ready, data);
}


/* Splits one CSV line into cells, keeping separators that sit inside quotes */
static GPtrArray *read_csv_row(const char **cursor, const char *end, char separator) {
	GPtrArray *cells = g_ptr_array_new_with_free_func(g_free);
	GString *cell = g_string_new(NULL);
	gboolean is_quoted = FALSE;
	const char *character = *cursor;
	for (; character < end; character++) {
		if (is_quoted && *character == '"' && character + 1 < end && character[1] == '"') {
			g_string_append_c(cell, '"');
			character++;
		} else if (*character == '"') {
			is_quoted = !is_quoted;
		} else if (!is_quoted && *character == separator) {
			g_ptr_array_add(cells, g_string_free(g_steal_pointer(&cell), FALSE));
			cell = g_string_new(NULL);
		} else if (!is_quoted && (*character == '\n' || *character == '\r')) {
			break;
		} else {
			g_string_append_c(cell, *character);
		}
	}
	g_ptr_array_add(cells, g_string_free(cell, FALSE));
	while (character < end && (*character == '\r' || *character == '\n'))
		character++;
	*cursor = character;
	return cells;
}


/* Hands comma or tab separated values over in batches of rows, the first batch starting with the header */
void split_table(const char *text, gsize length, char separator, BlockReady ready, gpointer data,
	GCancellable *cancellable) {
	const char *cursor = text, *end = text + length;
	gboolean is_header = TRUE;
	while (cursor < end && !g_cancellable_is_cancelled(cancellable)) {
		Block *batch = create_block(BLOCK_TABLE);
		batch->checked = TRUE;
		batch->line = is_header ? 1 : 0;
		while (cursor < end && batch->children->len < TABLE_BATCH_ROWS) {
			g_autoptr(GPtrArray) cells = read_csv_row(&cursor, end, separator);
			Block *row = create_block(BLOCK_ROW);
			row->header = is_header;
			is_header = FALSE;
			for (guint index = 0; index < cells->len; index++) {
				Block *cell = create_block(BLOCK_CELL);
				cell->header = row->header;
				/* Table cells are only ever shown as plain text, so they skip the markup a paragraph needs */
				cell->text = g_strdup(g_ptr_array_index(cells, index));
				add_child_block(row, cell);
			}
			add_child_block(batch, row);
		}
		ready(batch, data);
	}
}


gboolean is_minified_json(const char *text, gsize length) {
	gsize lines = 1;
	for (gsize index = 0; index < length; index++)
		lines += text[index] == '\n';
	return length / lines > MINIFIED_LINE_BYTES;
}


static void start_json_line(GString *output, int depth) {
	g_string_append_c(output, '\n');
	for (int level = 0; level < depth; level++)
		g_string_append(output, JSON_INDENT);
}


/* Lays out compact JSON one value per line, leaving strings untouched */
char *indent_json(const char *text, gsize length) {
	GString *output = g_string_sized_new(length + length / 2);
	int depth = 0;
	gboolean is_in_string = FALSE;
	for (gsize index = 0; index < length; index++) {
		char character = text[index];
		if (is_in_string) {
			g_string_append_c(output, character);
			if (character == '\\' && index + 1 < length)
				g_string_append_c(output, text[++index]);
			else if (character == '"')
				is_in_string = FALSE;
			continue;
		}
		if (g_ascii_isspace(character))
			continue;
		if (character == '}' || character == ']') {
			depth = MAX(depth - 1, 0);
			if (output->len > 0 && output->str[output->len - 1] != '{' && output->str[output->len - 1] != '[')
				start_json_line(output, depth);
		}
		g_string_append_c(output, character);
		if (character == '"')
			is_in_string = TRUE;
		else if (character == ':')
			g_string_append_c(output, ' ');
		else if (character == ',')
			start_json_line(output, depth);
		if (character != '{' && character != '[')
			continue;
		gsize next = index + 1;
		while (next < length && g_ascii_isspace(text[next]))
			next++;
		depth++;
		if (next < length && text[next] != '}' && text[next] != ']')
			start_json_line(output, depth);
	}
	return g_string_free(output, FALSE);
}
