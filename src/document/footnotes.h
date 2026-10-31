#pragma once

#include "document/parse.h"

/* One footnote definition with the number its first reference gave it, or zero while unreferenced */
typedef struct {
	char *label, *text;
	int line, number;
} Footnote;

/* The footnotes of one document, numbered in the order the text first refers to them */
typedef struct {
	GPtrArray *notes;
	int next_number;
} Footnotes;

char *take_footnotes(const char *text, gsize length, gsize start, Footnotes *footnotes);
void link_footnote_references(Footnotes *footnotes, Block *block);
void emit_footnotes(Footnotes *footnotes, BlockReady ready, gpointer data);
void clear_footnotes(Footnotes *footnotes);
