#include <string.h>

#include <adwaita.h>

#include "common/long-lines.h"
#include "document-view.h"
#include "source-view.h"

#define LIGHT_SCHEME "Adwaita"
#define DARK_SCHEME "Adwaita-dark"
#define TAB_WIDTH 4
#define SPLIT_WATCH "split-watch"
#define MINIMUM_NUMBER_DIGITS 2
#define NUMBER_PADDING_PIXELS 6

/* A source view that also draws the landing flash under its text, as the reading view does */
struct _FlashingSourceView {
	GtkSourceView parent_instance;
};

#define FLASHING_SOURCE_VIEW_TYPE (flashing_source_view_get_type())
G_DECLARE_FINAL_TYPE(FlashingSourceView, flashing_source_view, FLASHING, SOURCE_VIEW, GtkSourceView)
G_DEFINE_FINAL_TYPE(FlashingSourceView, flashing_source_view, GTK_SOURCE_TYPE_VIEW)

/* Code fence and interpreter names that differ from the language ids GtkSourceView uses */
static const struct {
	const char *name, *language;
} LANGUAGE_NAMES[] = {
	{"bash", "sh"}, {"zsh", "sh"}, {"shell", "sh"}, {"console", "sh"}, {"py", "python3"}, {"python", "python3"},
	{"js", "js"}, {"javascript", "js"}, {"node", "js"}, {"ts", "typescript"}, {"rs", "rust"}, {"yml", "yaml"},
	{"md", "markdown"}, {"htm", "html"}, {"cs", "c-sharp"}, {"csharp", "c-sharp"}, {"c++", "cpp"},
	{"h", "c"}, {"kt", "kotlin"}, {"rb", "ruby"}, {"pl", "perl"}, {"makefile", "makefile"}, {"mk", "makefile"},
	{"cmakelists.txt", "cmake"}, {"dockerfile", "dockerfile"}, {"gitconfig", "ini"}, {"editorconfig", "ini"},
	{"env", "sh"}, {"patch", "diff"}, {"jsonc", "json"}, {"conf", "ini"}, {"cfg", "ini"}, {"service", "ini"},
	{"timer", "ini"}, {"socket", "ini"}, {"mount", "ini"}, {"target", "ini"}, {"ui", "xml"}, {"svg", "xml"},
	{"log", NULL},
};


static GtkSourceLanguage *get_language_by_name(GtkSourceLanguageManager *manager, const char *name) {
	g_autofree char *lowered = g_ascii_strdown(name, -1);
	g_strchomp(lowered);
	for (guint index = 0; index < G_N_ELEMENTS(LANGUAGE_NAMES); index++)
		if (g_strcmp0(lowered, LANGUAGE_NAMES[index].name) == 0)
			return LANGUAGE_NAMES[index].language
				? gtk_source_language_manager_get_language(manager, LANGUAGE_NAMES[index].language) : NULL;
	GtkSourceLanguage *found = gtk_source_language_manager_get_language(manager, lowered);
	if (found)
		return found;
	/* Interpreters such as python3.12 or lua5.4 name their language before the version */
	gsize length = strcspn(lowered, "0123456789.");
	if (length > 0 && length < strlen(lowered)) {
		lowered[length] = '\0';
		return get_language_by_name(manager, lowered);
	}
	g_autofree char *file_name = g_strconcat("file.", lowered, NULL);
	return gtk_source_language_manager_guess_language(manager, file_name, NULL);
}


/* Uses the file name first, as GtkSourceView knows most names, then the language this viewer detected */
GtkSourceLanguage *find_source_language(const char *file_name, const char *language) {
	GtkSourceLanguageManager *manager = gtk_source_language_manager_get_default();
	GtkSourceLanguage *found = file_name ? gtk_source_language_manager_guess_language(manager, file_name, NULL) : NULL;
	if (!found && language && *language)
		found = get_language_by_name(manager, language);
	return found;
}


static void apply_style_scheme(AdwStyleManager *styles, GParamSpec *property, GtkSourceBuffer *buffer) {
	GtkSourceStyleSchemeManager *schemes = gtk_source_style_scheme_manager_get_default();
	const char *name = adw_style_manager_get_dark(styles) ? DARK_SCHEME : LIGHT_SCHEME;
	gtk_source_buffer_set_style_scheme(buffer, gtk_source_style_scheme_manager_get_scheme(schemes, name));
}


/* Keeps a buffer's colours in step with the light or dark system style until the buffer is freed */
void follow_style_scheme(GtkSourceBuffer *buffer) {
	AdwStyleManager *styles = adw_style_manager_get_default();
	apply_style_scheme(styles, NULL, buffer);
	g_signal_connect_object(styles, "notify::dark", G_CALLBACK(apply_style_scheme), buffer, 0);
}


static void snapshot_flashing_layer(GtkTextView *view, GtkTextViewLayer layer, GtkSnapshot *snapshot) {
	GtkTextViewClass *parent = GTK_TEXT_VIEW_CLASS(flashing_source_view_parent_class);
	if (parent->snapshot_layer)
		parent->snapshot_layer(view, layer, snapshot);
	if (layer == GTK_TEXT_VIEW_LAYER_BELOW_TEXT)
		draw_text_flash(view, snapshot);
}


static void flashing_source_view_class_init(FlashingSourceViewClass *class) {
	GTK_TEXT_VIEW_CLASS(class)->snapshot_layer = snapshot_flashing_layer;
}


static void flashing_source_view_init(FlashingSourceView *view) {
}


/* GtkSourceView's parser spends seconds on each split line even with colours off, so a document with one loses
 * its language, as minified code gains little from colours */
static void notice_split_lines(GtkTextBuffer *buffer, GtkTextIter *end, char *text, int length, gpointer data) {
	if (!has_split_lines(buffer))
		return;
	gtk_source_buffer_set_language(GTK_SOURCE_BUFFER(buffer), NULL);
	gtk_source_buffer_set_highlight_matching_brackets(GTK_SOURCE_BUFFER(buffer), FALSE);
	g_signal_handlers_disconnect_by_data(buffer, SPLIT_WATCH);
}


/* Line numbers by the file's own lines, so the later pieces of a split line show none */
struct _FileLineNumbers {
	GtkSourceGutterRendererText parent_instance;
	int digits;
};

#define FILE_LINE_NUMBERS_TYPE (file_line_numbers_get_type())
G_DECLARE_FINAL_TYPE(FileLineNumbers, file_line_numbers, CRIMSON, FILE_LINE_NUMBERS, GtkSourceGutterRendererText)
G_DEFINE_FINAL_TYPE(FileLineNumbers, file_line_numbers, GTK_SOURCE_TYPE_GUTTER_RENDERER_TEXT)


static void file_line_numbers_query_data(GtkSourceGutterRenderer *renderer, GtkSourceGutterLines *lines,
	guint line) {
	GtkTextBuffer *buffer = GTK_TEXT_BUFFER(gtk_source_gutter_renderer_get_buffer(renderer));
	char label[16] = "";
	GtkTextIter iter;
	gtk_text_buffer_get_iter_at_line(buffer, &iter, (int)line);
	if (!has_split_lines(buffer))
		g_snprintf(label, sizeof label, "%u", line + 1);
	else if (!continues_split_line(&iter))
		g_snprintf(label, sizeof label, "%d", get_joined_line(buffer, (int)line) + 1);
	gtk_source_gutter_renderer_text_set_text(GTK_SOURCE_GUTTER_RENDERER_TEXT(renderer), label, -1);
}


static int count_number_digits(FileLineNumbers *numbers) {
	GtkSourceBuffer *buffer = gtk_source_gutter_renderer_get_buffer(GTK_SOURCE_GUTTER_RENDERER(numbers));
	int digits = 0;
	for (int count = buffer ? gtk_text_buffer_get_line_count(GTK_TEXT_BUFFER(buffer)) : 1; count > 0; count /= 10)
		digits++;
	return MAX(digits, MINIMUM_NUMBER_DIGITS);
}


static void file_line_numbers_measure(GtkWidget *widget, GtkOrientation orientation, int for_size, int *minimum,
	int *natural, int *minimum_baseline, int *natural_baseline) {
	*minimum_baseline = *natural_baseline = -1;
	*minimum = *natural = 0;
	if (orientation == GTK_ORIENTATION_VERTICAL)
		return;
	FileLineNumbers *numbers = CRIMSON_FILE_LINE_NUMBERS(widget);
	numbers->digits = count_number_digits(numbers);
	g_autofree char *sample = g_strnfill(numbers->digits, '0');
	int width = 0;
	gtk_source_gutter_renderer_text_measure(GTK_SOURCE_GUTTER_RENDERER_TEXT(widget), sample, &width, NULL);
	*minimum = *natural = width + 2 * gtk_source_gutter_renderer_get_xpad(GTK_SOURCE_GUTTER_RENDERER(widget));
}


/* Asks for a new width only when the line count gains or loses a digit */
static void notice_line_count(GtkTextBuffer *buffer, FileLineNumbers *numbers) {
	if (count_number_digits(numbers) != numbers->digits)
		gtk_widget_queue_resize(GTK_WIDGET(numbers));
}


static void file_line_numbers_change_buffer(GtkSourceGutterRenderer *renderer, GtkSourceBuffer *old_buffer) {
	if (old_buffer)
		g_signal_handlers_disconnect_by_data(old_buffer, renderer);
	GtkSourceBuffer *buffer = gtk_source_gutter_renderer_get_buffer(renderer);
	if (buffer)
		g_signal_connect_object(buffer, "changed", G_CALLBACK(notice_line_count), renderer, 0);
	GTK_SOURCE_GUTTER_RENDERER_CLASS(file_line_numbers_parent_class)->change_buffer(renderer, old_buffer);
	gtk_widget_queue_resize(GTK_WIDGET(renderer));
}


static void file_line_numbers_class_init(FileLineNumbersClass *class) {
	GTK_WIDGET_CLASS(class)->measure = file_line_numbers_measure;
	GTK_SOURCE_GUTTER_RENDERER_CLASS(class)->query_data = file_line_numbers_query_data;
	GTK_SOURCE_GUTTER_RENDERER_CLASS(class)->change_buffer = file_line_numbers_change_buffer;
}


static void file_line_numbers_init(FileLineNumbers *numbers) {
	gtk_source_gutter_renderer_set_xalign(GTK_SOURCE_GUTTER_RENDERER(numbers), 1.0);
	gtk_source_gutter_renderer_set_xpad(GTK_SOURCE_GUTTER_RENDERER(numbers), NUMBER_PADDING_PIXELS);
}


void show_file_line_numbers(GtkSourceView *view) {
	if (g_object_get_data(G_OBJECT(view), "file-line-numbers"))
		return;
	GtkSourceGutterRenderer *numbers = g_object_new(FILE_LINE_NUMBERS_TYPE, NULL);
	g_object_set_data(G_OBJECT(view), "file-line-numbers", numbers);
	gtk_source_gutter_insert(gtk_source_view_get_gutter(view, GTK_TEXT_WINDOW_LEFT), numbers, 0);
}


/* A source view with the shared tab width and monospace font, without colours until it is prepared */
GtkWidget *create_source_view(void) {
	GtkWidget *view = g_object_new(FLASHING_SOURCE_VIEW_TYPE, NULL);
	gtk_source_view_set_tab_width(GTK_SOURCE_VIEW(view), TAB_WIDTH);
	gtk_text_view_set_monospace(GTK_TEXT_VIEW(view), TRUE);
	GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
	split_long_lines(buffer);
	g_signal_connect_after(buffer, "insert-text", G_CALLBACK(notice_split_lines), SPLIT_WATCH);
	join_long_line_copies(GTK_TEXT_VIEW(view));
	return view;
}


/* Colours code with GtkSourceView off screen and hands each coloured run over, for code shown inside other text */
void highlight_code(const char *text, const char *language, CodeRunFound found, gpointer data) {
	/* Loading a language compiles its rules, so one buffer per language is kept until the app quits */
	static GHashTable *buffers;
	GtkSourceLanguage *source_language = language ? find_source_language(NULL, language) : NULL;
	if (!source_language)
		return;
	if (!buffers)
		buffers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
	const char *id = gtk_source_language_get_id(source_language);
	GtkSourceBuffer *buffer = g_hash_table_lookup(buffers, id);
	if (!buffer) {
		buffer = gtk_source_buffer_new_with_language(source_language);
		gtk_source_buffer_set_highlight_matching_brackets(buffer, FALSE);
		g_hash_table_insert(buffers, g_strdup(id), buffer);
	}
	apply_style_scheme(adw_style_manager_get_default(), NULL, buffer);
	gtk_text_buffer_set_text(GTK_TEXT_BUFFER(buffer), text, -1);
	GtkTextIter iter, end;
	gtk_text_buffer_get_bounds(GTK_TEXT_BUFFER(buffer), &iter, &end);
	gtk_source_buffer_ensure_highlight(buffer, &iter, &end);
	while (!gtk_text_iter_is_end(&iter)) {
		GtkTextIter next = iter;
		gtk_text_iter_forward_to_tag_toggle(&next, NULL);
		g_autoptr(GSList) tags = gtk_text_iter_get_tags(&iter);
		for (GSList *item = tags; item; item = item->next)
			found(gtk_text_iter_get_offset(&iter), gtk_text_iter_get_offset(&next), item->data, data);
		iter = next;
	}
}


/* Loads the language and colours only when a view is first filled, since GtkSourceView parses every
 * language and style file on first use and that would delay the window */
void prepare_source_view(GtkSourceView *view, const char *file_name, const char *language) {
	GtkSourceBuffer *buffer = GTK_SOURCE_BUFFER(gtk_text_view_get_buffer(GTK_TEXT_VIEW(view)));
	gtk_source_buffer_set_language(buffer, find_source_language(file_name, language));
	if (g_object_get_data(G_OBJECT(view), "is-prepared"))
		return;
	g_object_set_data(G_OBJECT(view), "is-prepared", GINT_TO_POINTER(TRUE));
	follow_style_scheme(buffer);
}
