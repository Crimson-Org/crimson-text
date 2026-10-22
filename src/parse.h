#pragma once

#include <gio/gio.h>

#include "document.h"

/* Receives each finished top level block on the parsing thread and takes ownership of it */
typedef void (*BlockReady)(Block *block, gpointer data);

extern const char *const MARKDOWN_EXTENSIONS[];
extern const guint MARKDOWN_EXTENSION_COUNT;

gboolean is_document_name(const char *name);
MarkdownStyle detect_markdown_style(const char *text, gsize length);
gboolean parse_markdown(const char *text, gsize length, BlockReady ready, gpointer data,
	GCancellable *cancellable);
DocumentKind detect_document_kind(const char *name, const char *text, gsize length, char **language);
void split_source(const char *text, gsize length, const char *language, BlockReady ready, gpointer data,
	GCancellable *cancellable);
void split_text(const char *text, gsize length, BlockReady ready, gpointer data, GCancellable *cancellable);
gboolean looks_like_tasks(const char *text, gsize length);
void split_tasks(const char *text, gsize length, BlockReady ready, gpointer data, GCancellable *cancellable);
void split_table(const char *text, gsize length, char separator, BlockReady ready, gpointer data,
	GCancellable *cancellable);
gboolean is_minified_json(const char *text, gsize length);
char *indent_json(const char *text, gsize length);
