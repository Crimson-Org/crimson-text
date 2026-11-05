#pragma once

#include <gio/gio.h>

#include "document/document.h"

typedef struct Stream Stream;

/* What the worker learned about the text before handing it over */
typedef struct {
	DocumentKind kind;
	/* False when invalid UTF-8 was replaced, so saving would not give back the same file */
	gboolean is_exact;
	char *etag, *language;
	/* Charset the file was written in, or nothing for UTF-8 */
	char *charset;
} SourceInfo;

/* Receives a stream's results on the main thread */
typedef struct {
	/* Takes ownership of the whole document text once it is read and its kind is known */
	void (*source_ready)(char *source, const SourceInfo *info, gpointer data);
	void (*block_ready)(const Block *block, gpointer data);
	void (*finished)(const char *error, gpointer data);
	gpointer data;
} StreamEvents;

Stream *start_stream(GFile *file, const char *text, const char *name, const StreamEvents *events);
/* Reads a file on the calling thread, refusing ones too large to hold or that look like binary data, with the
 * reason in error */
char *read_text_file(GFile *file, GCancellable *cancellable, gsize *length, char **etag, char **error);
void stop_stream(Stream *stream);
