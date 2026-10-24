#include <string.h>

#include "../lang/en.h"
#include "charset.h"
#include "parse.h"
#include "stream.h"

/* Longest stretch of widget building before the main loop gets to draw a frame */
#define SLICE_MICROSECONDS 6000
/* Just below GTK's redraw at G_PRIORITY_HIGH_IDLE + 20, so frames still come first while the text views wait to
 * lay out until the whole file is in, which otherwise stretched big files over many seconds */
#define DELIVERY_PRIORITY (G_PRIORITY_HIGH_IDLE + 21)
#define MAXIMUM_FILE_BYTES (256 * 1024 * 1024)
/* A zero byte this early means the file is not text */
#define BINARY_SCAN_BYTES 8192
/* The worker waits once this many blocks wait to be shown, so a huge file never sits parsed in memory all at once */
#define MAXIMUM_WAITING_BLOCKS 64

/* Reads and parses one document on a worker thread, handing blocks to the main thread as they finish */
struct Stream {
	gatomicrefcount references;
	GMutex lock;
	/* Signalled when the main thread takes a block or the stream stops, so a waiting worker carries on */
	GCond room;
	/* Guarded by the lock */
	GQueue blocks;
	char *source, *error;
	SourceInfo info;
	gboolean is_finished;
	guint idle_source;
	/* Used only on the main thread */
	gboolean is_stopped, has_reported_finish;
	StreamEvents events;
	/* Set before the worker starts */
	GCancellable *cancellable;
	GFile *file;
	char *text, *name;
};


static Stream *reference_stream(Stream *stream) {
	g_atomic_ref_count_inc(&stream->references);
	return stream;
}


static void release_stream(gpointer data) {
	Stream *stream = data;
	if (!g_atomic_ref_count_dec(&stream->references))
		return;
	g_queue_clear_full(&stream->blocks, (GDestroyNotify)free_block);
	g_free(stream->source);
	g_free(stream->error);
	g_free(stream->info.etag);
	g_free(stream->info.language);
	g_free(stream->info.charset);
	g_free(stream->text);
	g_free(stream->name);
	g_clear_object(&stream->file);
	g_object_unref(stream->cancellable);
	g_mutex_clear(&stream->lock);
	g_cond_clear(&stream->room);
	g_free(stream);
}


static gboolean deliver_stream(gpointer data) {
	Stream *stream = data;
	gint64 deadline = g_get_monotonic_time() + SLICE_MICROSECONDS;
	g_mutex_lock(&stream->lock);
	char *source = g_steal_pointer(&stream->source);
	SourceInfo info = stream->info;
	g_mutex_unlock(&stream->lock);
	if (source && !stream->is_stopped)
		stream->events.source_ready(source, &info, stream->events.data);
	else
		g_free(source);
	while (!stream->is_stopped && g_get_monotonic_time() < deadline) {
		g_mutex_lock(&stream->lock);
		Block *block = g_queue_pop_head(&stream->blocks);
		g_cond_signal(&stream->room);
		g_mutex_unlock(&stream->lock);
		if (!block)
			break;
		stream->events.block_ready(block, stream->events.data);
		free_block(block);
	}
	g_mutex_lock(&stream->lock);
	gboolean has_more = !stream->is_stopped && !g_queue_is_empty(&stream->blocks);
	gboolean is_done = !has_more && stream->is_finished;
	if (!has_more)
		stream->idle_source = 0;
	g_autofree char *error = is_done ? g_steal_pointer(&stream->error) : NULL;
	g_mutex_unlock(&stream->lock);
	if (is_done && !stream->is_stopped && !stream->has_reported_finish) {
		stream->has_reported_finish = TRUE;
		stream->events.finished(error, stream->events.data);
	}
	return has_more ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
}


/* Must be called with the lock held */
static void schedule_delivery(Stream *stream) {
	if (!stream->idle_source)
		stream->idle_source = g_idle_add_full(DELIVERY_PRIORITY, deliver_stream,
			reference_stream(stream), release_stream);
}


static void queue_block(Block *block, gpointer data) {
	Stream *stream = data;
	g_mutex_lock(&stream->lock);
	while (g_queue_get_length(&stream->blocks) >= MAXIMUM_WAITING_BLOCKS
		&& !g_cancellable_is_cancelled(stream->cancellable))
		g_cond_wait(&stream->room, &stream->lock);
	g_queue_push_tail(&stream->blocks, block);
	schedule_delivery(stream);
	g_mutex_unlock(&stream->lock);
}


static void finish_stream(Stream *stream, char *error) {
	g_mutex_lock(&stream->lock);
	stream->error = error;
	stream->is_finished = TRUE;
	schedule_delivery(stream);
	g_mutex_unlock(&stream->lock);
}


/* Reads the file, refusing ones too large to hold or that look like binary data */
static char *read_file(Stream *stream, gsize *length, char **etag, char **error) {
	g_autoptr(GError) failure = NULL;
	g_autoptr(GFileInfo) info = g_file_query_info(stream->file, G_FILE_ATTRIBUTE_STANDARD_SIZE,
		G_FILE_QUERY_INFO_NONE, stream->cancellable, NULL);
	if (info && g_file_info_get_size(info) > MAXIMUM_FILE_BYTES) {
		*error = g_strdup(TEXT_FILE_TOO_LARGE);
		return NULL;
	}
	char *text = NULL;
	if (!g_file_load_contents(stream->file, stream->cancellable, &text, length, etag, &failure)) {
		*error = g_strdup(failure->message);
		return NULL;
	}
	if (!has_byte_order_mark(text, *length) && memchr(text, 0, MIN(*length, BINARY_SCAN_BYTES))) {
		*error = g_strdup(TEXT_BINARY_FILE);
		g_free(text);
		return NULL;
	}
	return text;
}


static void split_document(Stream *stream, const char *text, gsize length, DocumentKind kind, const char *language) {
	if (kind == DOCUMENT_SOURCE)
		split_source(text, length, language, queue_block, stream, stream->cancellable);
	else if (kind == DOCUMENT_TEXT)
		split_text(text, length, queue_block, stream, stream->cancellable);
	else if (kind == DOCUMENT_TASKS)
		split_tasks(text, length, queue_block, stream, stream->cancellable);
	else if (kind == DOCUMENT_TABLE)
		split_table(text, length, g_strcmp0(language, "tsv") == 0 ? '\t' : ',', queue_block, stream,
			stream->cancellable);
	else
		parse_markdown(text, length, queue_block, stream, stream->cancellable);
}


static gpointer run_stream(gpointer data) {
	Stream *stream = data;
	char *text = g_steal_pointer(&stream->text);
	gsize length = text ? strlen(text) : 0;
	char *etag = NULL, *error = NULL;
	if (!text && !(text = read_file(stream, &length, &etag, &error))) {
		finish_stream(stream, error);
		release_stream(stream);
		return NULL;
	}
	char *charset = NULL;
	char *decoded = decode_text(text, length, &charset);
	gboolean is_exact = decoded != NULL;
	if (!decoded)
		decoded = g_utf8_make_valid(text, length);
	g_free(text);
	text = decoded;
	length = strlen(text);
	g_autofree char *language = NULL;
	DocumentKind kind = detect_document_kind(stream->name, text, length, &language);
	g_mutex_lock(&stream->lock);
	stream->source = g_strndup(text, length);
	stream->info = (SourceInfo){kind, is_exact, etag, g_strdup(language), charset};
	schedule_delivery(stream);
	g_mutex_unlock(&stream->lock);
	split_document(stream, text, length, kind, language);
	g_free(text);
	finish_stream(stream, NULL);
	release_stream(stream);
	return NULL;
}


Stream *start_stream(GFile *file, const char *text, const char *name, const StreamEvents *events) {
	Stream *stream = g_new0(Stream, 1);
	g_atomic_ref_count_init(&stream->references);
	g_mutex_init(&stream->lock);
	g_cond_init(&stream->room);
	g_queue_init(&stream->blocks);
	stream->events = *events;
	stream->cancellable = g_cancellable_new();
	stream->file = file ? g_object_ref(file) : NULL;
	stream->text = g_strdup(text);
	stream->name = g_strdup(name);
	g_thread_unref(g_thread_new("parse", run_stream, reference_stream(stream)));
	return stream;
}


void stop_stream(Stream *stream) {
	if (!stream)
		return;
	stream->is_stopped = TRUE;
	g_cancellable_cancel(stream->cancellable);
	g_mutex_lock(&stream->lock);
	g_cond_broadcast(&stream->room);
	g_mutex_unlock(&stream->lock);
	release_stream(stream);
}
