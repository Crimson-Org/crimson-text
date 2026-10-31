#include <string.h>

#include "document/inline.h"
#include "document/parse.h"

#define SOURCE_CHUNK_LINES 400
#define KIND_SCAN_BYTES (64 * 1024)
/* Longest run of plain text lines in one block, so huge files never become one label */
#define TEXT_BLOCK_LINES 200

const char *const MARKDOWN_EXTENSIONS[] = {"md", "markdown", "mdown", "mkd", "mkdn", "mdx"};
const guint MARKDOWN_EXTENSION_COUNT = G_N_ELEMENTS(MARKDOWN_EXTENSIONS);

/* Plain text extensions shown as reading text */
static const char *const TEXT_EXTENSIONS[] = {"txt", "text"};

static const char *const SOURCE_EXTENSIONS[] = {
	"c", "h", "cc", "cpp", "cxx", "hpp", "hh", "rs", "go", "py", "js", "mjs", "cjs", "ts", "tsx", "jsx",
	"sh", "bash", "zsh", "fish", "lua", "rb", "java", "kt", "cs", "swift", "json", "toml", "yaml", "yml",
	"ini", "conf", "cfg", "xml", "html", "htm", "css", "scss", "sql", "bmx", "mk", "cmake", "diff", "patch",
	"log", "vala", "d", "zig", "nim", "php", "pl", "glsl", "hlsl", "desktop", "service", "timer", "socket",
	"mount", "target", "properties", "jsonc", "svg", "ui", "rules", "env", "nix",
};

static const char *const TABLE_EXTENSIONS[] = {"csv", "tsv"};

static const char *const TASK_NAMES[] = {
	"TODO", "TODO.txt", "todo.txt", "TASKS", "TASKS.txt", "PLAN", "PLAN.txt", ".todo", ".plan", "done.txt",
};

static const char *const TASK_EXTENSIONS[] = {"todo", "plan", "tasks", "taskpaper"};

static const char *const SOURCE_NAMES[] = {
	"Makefile", "CMakeLists.txt", "Dockerfile", "PKGBUILD", "meson.build", ".gitignore", ".bashrc", ".profile",
	".zshrc", ".gitconfig", ".editorconfig", ".env",
};

/* Languages for names that carry no extension of their own */
static const struct {
	const char *name, *language;
} NAME_LANGUAGES[] = {
	{"PKGBUILD", "bash"}, {".bashrc", "bash"}, {".profile", "bash"}, {".zshrc", "zsh"},
	{"meson.build", "py"}, {".gitconfig", "gitconfig"}, {".editorconfig", "editorconfig"}, {".env", "env"},
};

/* Characters that end a bare web address when they close a sentence */
static const char URL_TRAILING[] = ".,;:!?)]}>\"'";


static gboolean is_listed(const char *value, const char *const *list, guint count) {
	for (guint index = 0; index < count; index++)
		if (g_ascii_strcasecmp(value, list[index]) == 0)
			return TRUE;
	return FALSE;
}


/* Reads the interpreter name from a line such as #!/usr/bin/env python3 */
static char *get_interpreter(const char *text, gsize length) {
	gsize line_length = strcspn(text, "\n");
	g_autofree char *line = g_strndup(text + 2, MIN(line_length, length) - 2);
	g_auto(GStrv) words = g_strsplit_set(g_strstrip(line), " \t", -1);
	if (!words[0])
		return NULL;
	g_autofree char *program = g_path_get_basename(words[0]);
	if (strcmp(program, "env") == 0 && words[1])
		return g_strdup(words[1][0] == '-' && words[2] ? words[2] : words[1]);
	return g_steal_pointer(&program);
}


/* Text without a known extension is Markdown only when it shows headings, links or fences */
static gboolean looks_like_markdown(const char *text, gsize length) {
	const char *end = text + MIN(length, KIND_SCAN_BYTES);
	for (const char *line = text; line < end;) {
		const char *line_end = memchr(line, '\n', end - line);
		if (!line_end)
			line_end = end;
		gsize size = line_end - line;
		gsize hashes = 0;
		while (hashes < size && line[hashes] == '#')
			hashes++;
		if ((hashes > 0 && hashes <= 6 && hashes < size && line[hashes] == ' ') || strncmp(line, "```", 3) == 0
			|| g_strstr_len(line, size, "]("))
			return TRUE;
		line = line_end + 1;
	}
	return FALSE;
}


static const char *get_extension(const char *name) {
	const char *dot = name ? strrchr(name, '.') : NULL;
	return dot && dot != name ? dot + 1 : NULL;
}


/* Whether a file name is one this viewer shows itself, so links to it open in a tab */
gboolean is_document_name(const char *name) {
	const char *extension = get_extension(name);
	if (!extension)
		return is_listed(name, SOURCE_NAMES, G_N_ELEMENTS(SOURCE_NAMES))
			|| is_listed(name, TASK_NAMES, G_N_ELEMENTS(TASK_NAMES));
	return is_listed(extension, MARKDOWN_EXTENSIONS, G_N_ELEMENTS(MARKDOWN_EXTENSIONS))
		|| is_listed(extension, SOURCE_EXTENSIONS, G_N_ELEMENTS(SOURCE_EXTENSIONS))
		|| is_listed(extension, TABLE_EXTENSIONS, G_N_ELEMENTS(TABLE_EXTENSIONS))
		|| is_listed(extension, TASK_EXTENSIONS, G_N_ELEMENTS(TASK_EXTENSIONS))
		|| is_listed(extension, TEXT_EXTENSIONS, G_N_ELEMENTS(TEXT_EXTENSIONS));
}


DocumentKind detect_document_kind(const char *name, const char *text, gsize length, char **language) {
	*language = NULL;
	const char *extension = get_extension(name);
	if (extension && is_listed(extension, MARKDOWN_EXTENSIONS, G_N_ELEMENTS(MARKDOWN_EXTENSIONS)))
		return DOCUMENT_MARKDOWN;
	gboolean is_task_name = (name && is_listed(name, TASK_NAMES, G_N_ELEMENTS(TASK_NAMES)))
		|| (extension && is_listed(extension, TASK_EXTENSIONS, G_N_ELEMENTS(TASK_EXTENSIONS)));
	if (is_task_name && (g_strstr_len(text, length, "[ ]") || g_strstr_len(text, length, "[x]")))
		return DOCUMENT_TASKS;
	if (is_task_name)
		return DOCUMENT_TEXT;
	if (extension && is_listed(extension, TABLE_EXTENSIONS, G_N_ELEMENTS(TABLE_EXTENSIONS))) {
		*language = g_ascii_strdown(extension, -1);
		return DOCUMENT_TABLE;
	}
	if (length > 2 && text[0] == '#' && text[1] == '!') {
		*language = get_interpreter(text, length);
		return DOCUMENT_SOURCE;
	}
	if (extension && is_listed(extension, SOURCE_EXTENSIONS, G_N_ELEMENTS(SOURCE_EXTENSIONS))) {
		*language = g_ascii_strdown(extension, -1);
		return DOCUMENT_SOURCE;
	}
	if (name && is_listed(name, SOURCE_NAMES, G_N_ELEMENTS(SOURCE_NAMES))) {
		*language = g_ascii_strdown(name, -1);
		for (guint index = 0; index < G_N_ELEMENTS(NAME_LANGUAGES); index++)
			if (strcmp(name, NAME_LANGUAGES[index].name) == 0)
				g_set_str(language, NAME_LANGUAGES[index].language);
		return DOCUMENT_SOURCE;
	}
	if (looks_like_markdown(text, length))
		return DOCUMENT_MARKDOWN;
	return looks_like_tasks(text, length) ? DOCUMENT_TASKS : DOCUMENT_TEXT;
}


/* Hands a code file over in runs of whole lines whose bytes join back into the exact file */
void split_source(const char *text, gsize length, const char *language, BlockReady ready, gpointer data,
	GCancellable *cancellable) {
	/* Laid out JSON no longer matches the file, so it is shown read only and its runs carry no source line */
	g_autofree char *laid_out = g_strcmp0(language, "json") == 0 && is_minified_json(text, length)
		? indent_json(text, length) : NULL;
	if (laid_out) {
		text = laid_out;
		length = strlen(laid_out);
	}
	const char *end = text + length;
	int line_number = 1;
	const char *chunk = text;
	do {
		const char *chunk_end = chunk;
		int lines = 0;
		while (chunk_end < end && lines < SOURCE_CHUNK_LINES)
			lines += *chunk_end++ == '\n';
		Block *block = create_block(BLOCK_SOURCE);
		block->level = line_number;
		block->line = laid_out ? 0 : line_number;
		block->checked = laid_out != NULL;
		block->text = g_strndup(chunk, chunk_end - chunk);
		block->label = g_strdup(language);
		ready(block, data);
		line_number += lines;
		chunk = chunk_end;
	} while (chunk < end && !g_cancellable_is_cancelled(cancellable));
}


/* Escapes plain text and turns bare web addresses into links */
static char *link_addresses(const char *text, gsize length) {
	GString *markup = g_string_sized_new(length);
	gsize position = 0;
	while (position < length) {
		const char *found = g_strstr_len(text + position, length - position, "http");
		gsize start = found ? (gsize)(found - text) : length;
		gboolean is_address = found && (strncmp(found, "http://", 7) == 0 || strncmp(found, "https://", 8) == 0);
		gsize end = start;
		while (is_address && end < length && !g_ascii_isspace(text[end]) && text[end] != '<')
			end++;
		while (is_address && end > start && strchr(URL_TRAILING, text[end - 1]))
			end--;
		g_autofree char *before = g_markup_escape_text(text + position, (is_address ? start : MIN(start + 4, length))
			- position);
		g_string_append(markup, before);
		if (!is_address) {
			position = MIN(start + 4, length);
			continue;
		}
		g_autofree char *address = g_strndup(text + start, end - start);
		g_autofree char *open = create_link_open(address, address);
		g_autofree char *shown = g_markup_escape_text(address, -1);
		g_string_append_printf(markup, "%s%s</a>", open, shown);
		position = end;
	}
	return g_string_free(markup, FALSE);
}


static void emit_text_paragraph(GString *paragraph, int line, BlockReady ready, gpointer data) {
	if (paragraph->len == 0)
		return;
	Block *block = create_block(BLOCK_PARAGRAPH);
	block->line = line;
	Piece *piece = g_new0(Piece, 1);
	piece->markup = link_addresses(paragraph->str, paragraph->len);
	g_ptr_array_add(block->pieces, piece);
	ready(block, data);
	g_string_truncate(paragraph, 0);
}


/* Hands plain text over a paragraph at a time, keeping its line breaks and splitting very long runs */
void split_text(const char *text, gsize length, BlockReady ready, gpointer data, GCancellable *cancellable) {
	g_autoptr(GString) paragraph = g_string_new(NULL);
	const char *end = text + length;
	int line = 1, paragraph_line = 1, paragraph_lines = 0;
	for (const char *start = text; start < end && !g_cancellable_is_cancelled(cancellable); line++) {
		const char *line_end = memchr(start, '\n', end - start);
		if (!line_end)
			line_end = end;
		gsize size = line_end - start;
		if (size > 0 && start[size - 1] == '\r')
			size--;
		gboolean is_blank = TRUE;
		for (gsize index = 0; index < size && is_blank; index++)
			is_blank = g_ascii_isspace(start[index]);
		if (is_blank || paragraph_lines == TEXT_BLOCK_LINES) {
			emit_text_paragraph(paragraph, paragraph_line, ready, data);
			paragraph_lines = 0;
		}
		if (!is_blank) {
			if (paragraph->len == 0)
				paragraph_line = line;
			else
				g_string_append_c(paragraph, '\n');
			g_string_append_len(paragraph, start, size);
			paragraph_lines++;
		}
		start = line_end + 1;
	}
	emit_text_paragraph(paragraph, paragraph_line, ready, data);
}
