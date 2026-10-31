#pragma once

#include <glib.h>

/* Markdown has six levels of heading, from # to ###### */
#define HEADING_LEVEL_COUNT 6
/* Shown in place of a character that cannot be read, also as its UTF-8 bytes */
#define REPLACEMENT_CHARACTER 0xFFFD
#define REPLACEMENT_CHARACTER_TEXT "\xEF\xBF\xBD"

/* Kinds of block the viewer draws */
typedef enum {
	BLOCK_DOCUMENT,
	BLOCK_PARAGRAPH,
	BLOCK_HEADING,
	BLOCK_QUOTE,
	BLOCK_ALERT,
	BLOCK_LIST,
	BLOCK_ITEM,
	BLOCK_CODE,
	BLOCK_RULE,
	BLOCK_TABLE,
	BLOCK_ROW,
	BLOCK_CELL,
	BLOCK_METADATA,
	/* A run of lines from a code file starting at the line in level, read only when checked */
	BLOCK_SOURCE,
	/* A collapsible section with its summary in label, open when checked */
	BLOCK_DETAILS
} BlockKind;

/* Whether a file renders as Markdown, a code view, plain reading text, a checklist or a table */
typedef enum { DOCUMENT_MARKDOWN, DOCUMENT_SOURCE, DOCUMENT_TEXT, DOCUMENT_TASKS, DOCUMENT_TABLE } DocumentKind;

/* Tables from CSV and TSV files are marked checked and shown in a column view instead of a grid */

/* Markdown flavours that change how a document is parsed */
typedef enum { STYLE_GITHUB, STYLE_OBSIDIAN, STYLE_PANDOC } MarkdownStyle;

/* A run of Pango markup or one picture inside a paragraph */
typedef struct {
	char *markup;
	char *image_source, *image_link, *image_text;
	/* Requested picture width in pixels, or a negative percentage */
	int image_width;
} Piece;

/* One parsed block with its inline pieces and nested blocks */
typedef struct Block {
	BlockKind kind;
	/* Heading level, list start number or column alignment */
	int level;
	gboolean ordered, task, checked, header, centered;
	/* Code text, alert kind or metadata lines */
	char *text;
	/* Code language, alert title or heading anchor */
	char *label;
	GPtrArray *pieces, *children;
	/* Source line where the block starts, or zero when it could not be found */
	int line;
	/* Byte offset of a task's mark between its brackets, or minus one when it cannot be ticked */
	long task_offset;
	/* Ticked and total tasks under a checklist heading */
	int task_done, task_total;
	/* Names that links can jump to at the start of the block, made when the first is added */
	GPtrArray *anchors;
} Block;

Block *create_block(BlockKind kind);
void free_block(Block *block);
void add_child_block(Block *parent, Block *child);
void add_block_anchor(Block *block, const char *anchor);
/* Whether a line opens or closes a fenced code block, which takes three backticks or three tildes */
gboolean starts_code_fence(const char *line, gsize size);
