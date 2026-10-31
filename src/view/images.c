#include <string.h>

#include <curl/curl.h>
#include <glycin.h>

#include <glib/gstdio.h>

#include "lang/en.h"
#include "view/images.h"
#include "app/launch.h"

#define LOADER_THREADS 6
#define DOWNLOAD_TIMEOUT_SECONDS 30L
#define CONNECT_TIMEOUT_SECONDS 10L
#define MAXIMUM_DOWNLOAD_BYTES (64L * 1024 * 1024)
#define MISSING_ICON_SIZE 32
#define USER_AGENT "CrimsonText/1.0"
#define MAXIMUM_PICTURE_BYTES (64 * 1024 * 1024)
/* Decoded pictures above this many pixels are refused, since each pixel takes four bytes of memory */
#define MAXIMUM_PICTURE_PIXELS (64 * 1024 * 1024)

/* One picture location being loaded with the widgets waiting for it */
typedef struct {
	/* Cache key, the location plus a local file's modification time so edited pictures load again */
	char *key;
	char *location;
	/* A web address, or a data address that holds the picture itself, instead of a file */
	gboolean is_remote, is_data;
	GPtrArray *pictures;
	GdkTexture *texture;
	char *error;
	/* Cancelled once no picture waits for the result, so closed tabs stop their downloads */
	GCancellable *cancellable;
	int waiting_count;
} ImageJob;

/* Finished textures by location, used only on the main thread */
static GHashTable *texture_cache;
/* Jobs still loading by location, used only on the main thread */
static GHashTable *loading_jobs;
static GThreadPool *loader_pool;


static void free_weak_reference(gpointer data) {
	g_weak_ref_clear(data);
	g_free(data);
}


static void free_job(ImageJob *job) {
	g_free(job->key);
	g_free(job->location);
	g_ptr_array_unref(job->pictures);
	g_clear_object(&job->texture);
	g_free(job->error);
	g_object_unref(job->cancellable);
	g_free(job);
}


/* Runs on the main thread when a waiting picture is destroyed */
static void forget_waiting_picture(gpointer data, GObject *picture) {
	ImageJob *job = data;
	if (--job->waiting_count == 0)
		g_cancellable_cancel(job->cancellable);
}


static int check_download_cancelled(void *job, curl_off_t total, curl_off_t received, curl_off_t upload_total,
	curl_off_t uploaded) {
	return g_cancellable_is_cancelled(((ImageJob *)job)->cancellable);
}


static size_t receive_bytes(char *data, size_t size, size_t count, void *buffer) {
	g_byte_array_append(buffer, (const guint8 *)data, size * count);
	return size * count;
}


static GBytes *download(ImageJob *job, char **error) {
	const char *url = job->location;
	static gsize is_curl_ready;
	if (g_once_init_enter(&is_curl_ready))
		g_once_init_leave(&is_curl_ready, curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK ? 1 : 2);
	CURL *request = curl_easy_init();
	if (!request) {
		*error = g_strdup("The network library could not start");
		return NULL;
	}
	GByteArray *buffer = g_byte_array_new();
	curl_easy_setopt(request, CURLOPT_URL, url);
	curl_easy_setopt(request, CURLOPT_FOLLOWLOCATION, 1L);
	/* Only web addresses, also after redirects, so a document cannot reach local files or other services */
	curl_easy_setopt(request, CURLOPT_PROTOCOLS_STR, "http,https");
	curl_easy_setopt(request, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
	curl_easy_setopt(request, CURLOPT_FAILONERROR, 1L);
	/* Signals cannot interrupt a worker thread, so timeouts must not rely on them */
	curl_easy_setopt(request, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(request, CURLOPT_TIMEOUT, DOWNLOAD_TIMEOUT_SECONDS);
	curl_easy_setopt(request, CURLOPT_CONNECTTIMEOUT, CONNECT_TIMEOUT_SECONDS);
	curl_easy_setopt(request, CURLOPT_MAXFILESIZE_LARGE, (curl_off_t)MAXIMUM_DOWNLOAD_BYTES);
	curl_easy_setopt(request, CURLOPT_USERAGENT, USER_AGENT);
	curl_easy_setopt(request, CURLOPT_ACCEPT_ENCODING, "");
	curl_easy_setopt(request, CURLOPT_WRITEFUNCTION, receive_bytes);
	curl_easy_setopt(request, CURLOPT_WRITEDATA, buffer);
	curl_easy_setopt(request, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(request, CURLOPT_XFERINFOFUNCTION, check_download_cancelled);
	curl_easy_setopt(request, CURLOPT_XFERINFODATA, job);
	CURLcode status = curl_easy_perform(request);
	curl_easy_cleanup(request);
	if (status != CURLE_OK) {
		*error = g_strdup(curl_easy_strerror(status));
		g_byte_array_unref(buffer);
		return NULL;
	}
	return g_byte_array_free_to_bytes(buffer);
}


static void show_texture(GtkPicture *picture, GdkTexture *texture, const char *error) {
	if (texture) {
		gtk_picture_set_paintable(picture, GDK_PAINTABLE(texture));
		gtk_widget_add_css_class(GTK_WIDGET(picture), "loaded");
		return;
	}
	GtkIconTheme *theme = gtk_icon_theme_get_for_display(gtk_widget_get_display(GTK_WIDGET(picture)));
	g_autoptr(GtkIconPaintable) icon = gtk_icon_theme_lookup_icon(theme, "image-missing-symbolic", NULL,
		MISSING_ICON_SIZE, gtk_widget_get_scale_factor(GTK_WIDGET(picture)), GTK_TEXT_DIR_NONE, 0);
	gtk_picture_set_paintable(picture, GDK_PAINTABLE(icon));
	gtk_widget_add_css_class(GTK_WIDGET(picture), "missing");
	g_debug("Picture failed: %s", error);
	const char *text = gtk_picture_get_alternative_text(picture);
	g_autofree char *tooltip = g_strdup_printf("%s\n%s", text && *text ? text : "", error);
	gtk_widget_set_tooltip_text(GTK_WIDGET(picture), g_strstrip(tooltip));
}


static gboolean deliver_job(gpointer data) {
	ImageJob *job = data;
	if (g_hash_table_lookup(loading_jobs, job->key) == job)
		g_hash_table_steal(loading_jobs, job->key);
	if (job->texture)
		g_hash_table_insert(texture_cache, g_strdup(job->key), g_object_ref(job->texture));
	for (guint index = 0; index < job->pictures->len; index++) {
		g_autoptr(GtkPicture) picture = g_weak_ref_get(g_ptr_array_index(job->pictures, index));
		if (!picture)
			continue;
		g_object_weak_unref(G_OBJECT(picture), forget_waiting_picture, job);
		show_texture(picture, job->texture, job->error);
	}
	free_job(job);
	return G_SOURCE_REMOVE;
}


/* Reads only regular files of a sane size, so a path such as a device or pipe cannot hang the loader */
static GBytes *read_local_picture(const char *path, char **error) {
	GStatBuf status;
	if (g_stat(path, &status) != 0 || !S_ISREG(status.st_mode)) {
		*error = g_strdup(TEXT_PICTURE_NOT_FOUND);
		return NULL;
	}
	if (status.st_size > MAXIMUM_PICTURE_BYTES) {
		*error = g_strdup(TEXT_PICTURE_TOO_LARGE);
		return NULL;
	}
	g_autoptr(GError) failure = NULL;
	char *contents = NULL;
	gsize length = 0;
	if (!g_file_get_contents(path, &contents, &length, &failure)) {
		*error = g_strdup(failure->message);
		return NULL;
	}
	return g_bytes_new_take(contents, length);
}


/* Turns the bytes into pixels in glycin's sandbox
 * The size is read first, so huge pictures are refused before any pixel is made */
static GdkTexture *decode_picture(GBytes *bytes, GCancellable *cancellable, char **error) {
	g_autoptr(GError) failure = NULL;
	g_autoptr(GlyLoader) loader = gly_loader_new_for_bytes(bytes);
	gly_loader_set_accepted_memory_formats(loader, GLY_MEMORY_SELECTION_R8G8B8A8_PREMULTIPLIED);
	g_autoptr(GlyImage) image = gly_loader_load(loader, &failure);
	if (!image) {
		*error = g_strdup(failure->message);
		return NULL;
	}
	if ((gint64)gly_image_get_width(image) * gly_image_get_height(image) > MAXIMUM_PICTURE_PIXELS) {
		*error = g_strdup(TEXT_PICTURE_TOO_LARGE);
		return NULL;
	}
	if (g_cancellable_is_cancelled(cancellable))
		return NULL;
	g_autoptr(GlyFrame) frame = gly_image_next_frame(image, &failure);
	if (!frame) {
		*error = g_strdup(failure->message);
		return NULL;
	}
	return gdk_memory_texture_new((int)gly_frame_get_width(frame), (int)gly_frame_get_height(frame),
		GDK_MEMORY_R8G8B8A8_PREMULTIPLIED, gly_frame_get_buf_bytes(frame), gly_frame_get_stride(frame));
}


/* Reads the picture held in a data address, such as one that starts with data:image/png;base64 */
static GBytes *read_data_picture(const char *location, char **error) {
	const char *comma = strchr(location, ',');
	if (!comma) {
		*error = g_strdup(TEXT_PICTURE_NOT_FOUND);
		return NULL;
	}
	g_autofree char *header = g_strndup(location, comma - location);
	g_autoptr(GBytes) payload = g_uri_unescape_bytes(comma + 1, -1, NULL, NULL);
	if (!payload) {
		*error = g_strdup(TEXT_PICTURE_NOT_FOUND);
		return NULL;
	}
	if (!g_str_has_suffix(header, ";base64"))
		return g_steal_pointer(&payload);
	gsize length = 0;
	g_autofree char *encoded = g_strndup(g_bytes_get_data(payload, NULL), g_bytes_get_size(payload));
	guchar *decoded = g_base64_decode(encoded, &length);
	return g_bytes_new_take(decoded, length);
}


static void load_in_thread(gpointer data, gpointer unused) {
	ImageJob *job = data;
	g_autoptr(GBytes) bytes = NULL;
	if (g_cancellable_is_cancelled(job->cancellable))
		bytes = NULL;
	else if (job->is_data)
		bytes = read_data_picture(job->location, &job->error);
	else if (job->is_remote)
		bytes = download(job, &job->error);
	else
		bytes = read_local_picture(job->location, &job->error);
	if (bytes && !g_cancellable_is_cancelled(job->cancellable))
		job->texture = decode_picture(bytes, job->cancellable, &job->error);
	if (!job->texture && !job->error)
		job->error = g_strdup("The picture was not loaded");
	g_idle_add(deliver_job, job);
}


/* Turns a Markdown picture source into a URL or a local path */
static char *resolve_location(const char *source, const char *base_directory, gboolean *is_remote) {
	if (g_str_has_prefix(source, "data:")) {
		*is_remote = FALSE;
		return g_strdup(source);
	}
	*is_remote = g_str_has_prefix(source, "http://") || g_str_has_prefix(source, "https://");
	if (*is_remote)
		return g_strdup(source);
	if (g_str_has_prefix(source, "file://"))
		return g_filename_from_uri(source, NULL, NULL);
	if (g_str_has_prefix(source, "//")) {
		*is_remote = TRUE;
		return g_strconcat("https:", source, NULL);
	}
	g_autofree char *path = g_strndup(source, strcspn(source, "?#"));
	g_autofree char *unescaped = g_uri_unescape_string(path, NULL);
	const char *name = unescaped ? unescaped : path;
	if (g_path_is_absolute(name) || !base_directory)
		return g_strdup(name);
	return g_build_filename(base_directory, name, NULL);
}


void load_picture(GtkPicture *picture, const char *source, const char *base_directory) {
	if (!texture_cache) {
		texture_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
		loading_jobs = g_hash_table_new(g_str_hash, g_str_equal);
		loader_pool = g_thread_pool_new(load_in_thread, NULL, LOADER_THREADS, FALSE, NULL);
	}
	gboolean is_remote = FALSE;
	g_autofree char *location = resolve_location(source, base_directory, &is_remote);
	if (!location)
		return;
	gboolean is_data = g_str_has_prefix(location, "data:");
	g_object_set_data_full(G_OBJECT(picture), "location", g_strdup(location), g_free);
	/* Pictures that are not files are written to the cache for opening and saving, as web pictures are */
	g_object_set_data(G_OBJECT(picture), "is-remote", GINT_TO_POINTER(is_remote || is_data));
	GStatBuf status = {0};
	if (!is_remote && !is_data)
		g_stat(location, &status);
	g_autofree char *key = g_strdup_printf("%s\n%" G_GINT64_FORMAT, location, (gint64)status.st_mtime);
	GdkTexture *cached = g_hash_table_lookup(texture_cache, key);
	if (cached) {
		show_texture(picture, cached, NULL);
		return;
	}
	/* A cancelled job still finishes on its own, while a new picture starts a fresh one */
	ImageJob *job = g_hash_table_lookup(loading_jobs, key);
	if (job && g_cancellable_is_cancelled(job->cancellable)) {
		g_hash_table_steal(loading_jobs, key);
		job = NULL;
	}
	gboolean is_new = !job;
	if (is_new) {
		job = g_new0(ImageJob, 1);
		job->key = g_strdup(key);
		job->location = g_strdup(location);
		job->is_remote = is_remote;
		job->is_data = is_data;
		job->pictures = g_ptr_array_new_with_free_func(free_weak_reference);
		job->cancellable = g_cancellable_new();
		g_hash_table_insert(loading_jobs, job->key, job);
	}
	job->waiting_count++;
	g_object_weak_ref(G_OBJECT(picture), forget_waiting_picture, job);
	GWeakRef *reference = g_new0(GWeakRef, 1);
	g_weak_ref_init(reference, picture);
	g_ptr_array_add(job->pictures, reference);
	if (is_new)
		g_thread_pool_push(loader_pool, job, NULL);
}


static GdkTexture *get_loaded_texture(GtkPicture *picture) {
	GdkPaintable *paintable = gtk_picture_get_paintable(picture);
	return GDK_IS_TEXTURE(paintable) && gtk_widget_has_css_class(GTK_WIDGET(picture), "loaded")
		? GDK_TEXTURE(paintable) : NULL;
}


static gboolean is_local_picture(GtkPicture *picture) {
	return !g_object_get_data(G_OBJECT(picture), "is-remote")
		&& g_object_get_data(G_OBJECT(picture), "location");
}


/* Writes a web picture to the cache so other applications can open it */
static GFile *get_picture_file(GtkPicture *picture) {
	const char *location = g_object_get_data(G_OBJECT(picture), "location");
	if (is_local_picture(picture))
		return g_file_new_for_path(location);
	GdkTexture *texture = get_loaded_texture(picture);
	if (!texture || !location)
		return NULL;
	g_autofree char *hash = g_compute_checksum_for_string(G_CHECKSUM_SHA1, location, -1);
	g_autofree char *name = g_strconcat(hash, ".png", NULL);
	g_autofree char *directory = g_build_filename(g_get_user_cache_dir(), "crimson-text", "pictures", NULL);
	g_autofree char *path = g_build_filename(directory, name, NULL);
	if (g_mkdir_with_parents(directory, 0700) != 0 || !gdk_texture_save_to_png(texture, path))
		return NULL;
	return g_file_new_for_path(path);
}


void open_picture_outside(GtkPicture *picture) {
	g_autoptr(GFile) file = get_picture_file(picture);
	if (!file)
		return;
	g_autofree char *uri = g_file_get_uri(file);
	launch_default(GTK_WIDGET(picture), uri);
}


static void finish_copying_picture(GObject *source, GAsyncResult *result, gpointer data) {
	g_autoptr(GtkWidget) picture = data;
	g_autoptr(GError) error = NULL;
	if (!g_file_copy_finish(G_FILE(source), result, &error))
		show_widget_message(picture, error->message);
}


static void finish_saving_picture(GObject *dialog, GAsyncResult *result, gpointer data) {
	g_autoptr(GtkPicture) picture = data;
	g_autoptr(GFile) target = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(dialog), result, NULL);
	if (!target)
		return;
	g_autoptr(GFile) source = get_picture_file(picture);
	if (!source) {
		show_widget_message(GTK_WIDGET(picture), TEXT_PICTURE_NOT_LOADED);
		return;
	}
	g_file_copy_async(source, target, G_FILE_COPY_OVERWRITE, G_PRIORITY_DEFAULT, NULL, NULL, NULL,
		finish_copying_picture, g_object_ref(picture));
}

void save_picture(GtkPicture *picture) {
	const char *location = g_object_get_data(G_OBJECT(picture), "location");
	if (!location)
		return;
	/* A data address names no file, so its picture is offered under a plain name */
	g_autofree char *path = g_str_has_prefix(location, "data:") ? g_strdup(TEXT_PICTURE_FILE_NAME)
		: g_strndup(location, strcspn(location, "?#"));
	g_autofree char *name = g_path_get_basename(path);
	g_autofree char *stem = g_strndup(name, strrchr(name, '.') ? (gsize)(strrchr(name, '.') - name) : strlen(name));
	g_autofree char *initial = is_local_picture(picture) ? g_strdup(name) : g_strconcat(stem, ".png", NULL);
	g_autoptr(GtkFileDialog) dialog = gtk_file_dialog_new();
	gtk_file_dialog_set_initial_name(dialog, initial);
	gtk_file_dialog_save(dialog, GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(picture))), NULL, finish_saving_picture,
		g_object_ref(picture));
}


void copy_picture(GtkPicture *picture) {
	GdkTexture *texture = get_loaded_texture(picture);
	if (texture)
		gdk_clipboard_set_texture(gtk_widget_get_clipboard(GTK_WIDGET(picture)), texture);
}
