#include <string.h>

#include "document/document.h"

#define CODE_FENCE_LENGTH 3
#define BACKTICK_FENCE "```"
#define TILDE_FENCE "~~~"

static void free_piece(gpointer data) {
	Piece *piece = data;
	g_free(piece->markup);
	g_free(piece->image_source);
	g_free(piece->image_link);
	g_free(piece->image_text);
	g_free(piece);
}


Block *create_block(BlockKind kind) {
	Block *block = g_new0(Block, 1);
	block->kind = kind;
	block->task_offset = -1;
	block->pieces = g_ptr_array_new_with_free_func(free_piece);
	block->children = g_ptr_array_new_with_free_func((GDestroyNotify)free_block);
	return block;
}


void free_block(Block *block) {
	if (!block)
		return;
	g_ptr_array_unref(block->pieces);
	g_ptr_array_unref(block->children);
	g_clear_pointer(&block->anchors, g_ptr_array_unref);
	g_free(block->text);
	g_free(block->label);
	g_free(block);
}


void add_child_block(Block *parent, Block *child) {
	g_ptr_array_add(parent->children, child);
}


void add_block_anchor(Block *block, const char *anchor) {
	if (!block->anchors)
		block->anchors = g_ptr_array_new_with_free_func(g_free);
	g_ptr_array_add(block->anchors, g_strdup(anchor));
}



gboolean starts_code_fence(const char *line, gsize size) {
	return size >= CODE_FENCE_LENGTH
		&& (strncmp(line, BACKTICK_FENCE, CODE_FENCE_LENGTH) == 0
			|| strncmp(line, TILDE_FENCE, CODE_FENCE_LENGTH) == 0);
}
