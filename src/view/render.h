#pragma once

#include <gtk/gtk.h>

#include "document/document.h"

/* One heading listed in the contents sidebar */
typedef struct {
	char *title;
	/* Heading level and source line, or zero when the line is unknown */
	int level, line;
	GtkTextMark *mark;
} OutlineEntry;

/* Where a block with a known source line starts in the view */
typedef struct {
	GtkTextMark *mark;
	int line;
} BlockMark;

/* The count on a checklist heading, kept current as its tasks are ticked */
typedef struct {
	GtkTextMark *start, *end;
	int done, total;
} TaskCount;

/* Receives a link the reader clicked */
typedef void (*LinkActivated)(const char *link, gpointer data);

/* Receives a ticked or cleared task with the byte offset of its mark in the source */
typedef void (*TaskToggled)(long offset, gboolean is_done, gpointer data);

/* What rendering needs from the tab, what it reports back and where it is writing */
typedef struct {
	GtkTextView *view;
	char *base_directory;
	/* Heading marks by anchor name, headings in order and blocks with source lines in order */
	GHashTable *anchors;
	GPtrArray *outline;
	GArray *blocks;
	GPtrArray *task_counts;
	LinkActivated activate_link;
	TaskToggled toggle_task;
	gpointer data;
	/* Tags every new line takes, such as quote bars and collapsed sections */
	GPtrArray *line_tags;
	/* Unzoomed indent of new lines and how many lists deep they are */
	int indent, list_depth;
	/* List marker or checkbox that starts the next line */
	char *pending_marker;
	GtkWidget *pending_check;
	/* The line being written started with a list marker, so it hangs left of the text below it */
	gboolean is_line_marked;
	TaskCount *task_count;
} RenderContext;

void reset_render_context(RenderContext *context, GtkTextView *view);
void clear_render_context(RenderContext *context);
void swap_render_contexts(RenderContext *shown, RenderContext *next);
void render_block(RenderContext *context, const Block *block);
