#include <gtk/gtk.h>

#include "common/key-store.h"
#include "document-view.h"
#include "settings.h"

#define SETTINGS_FILE "settings.ini"
#define SETTINGS_GROUP "settings"
/* Percentages of the system document font that a size of 100 shows, since that font reads small for whole pages */
#define READING_BASE_SIZE 120
#define EDITOR_BASE_SIZE 105

static Settings settings = {DEFAULT_TEXT_SIZE, DEFAULT_TEXT_SIZE, NULL, FALSE};
static GtkCssProvider *style;


static void load_settings(void) {
	g_autofree char *path = get_config_file_path(SETTINGS_FILE);
	g_autoptr(GKeyFile) file = load_key_file(path, G_KEY_FILE_NONE);
	settings.editor_font = g_strdup("");
	settings.reading_size = read_key_integer(file, SETTINGS_GROUP, "reading-size", DEFAULT_TEXT_SIZE,
		MINIMUM_TEXT_SIZE, MAXIMUM_TEXT_SIZE);
	settings.editor_size = read_key_integer(file, SETTINGS_GROUP, "editor-size", DEFAULT_TEXT_SIZE,
		MINIMUM_TEXT_SIZE, MAXIMUM_TEXT_SIZE);
	g_autofree char *font = g_key_file_get_string(file, SETTINGS_GROUP, "editor-font", NULL);
	if (font)
		g_set_str(&settings.editor_font, font);
	settings.shows_edit_pencil = g_key_file_get_boolean(file, SETTINGS_GROUP, "edit-pencil", NULL);
}


Settings *get_settings(void) {
	if (!settings.editor_font)
		load_settings();
	return &settings;
}


void save_settings(void) {
	g_autofree char *path = get_config_file_path(SETTINGS_FILE);
	g_autoptr(GKeyFile) file = g_key_file_new();
	g_key_file_set_integer(file, SETTINGS_GROUP, "reading-size", settings.reading_size);
	g_key_file_set_integer(file, SETTINGS_GROUP, "editor-size", settings.editor_size);
	g_key_file_set_string(file, SETTINGS_GROUP, "editor-font", settings.editor_font);
	g_key_file_set_boolean(file, SETTINGS_GROUP, "edit-pencil", settings.shows_edit_pencil);
	save_key_file(file, path);
}


/* Both sizes scale the system document font, since the monospace font is often set larger and would make
 * the editor and code files look bigger than documents at the same percentage */
void apply_settings(void) {
	Settings *current = get_settings();
	if (!style) {
		style = gtk_css_provider_new();
		gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(style),
			GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
	}
	g_autofree char *family = *current->editor_font
		? g_strdup_printf(" font-family: \"%s\";", current->editor_font) : g_strdup("");
	int reading = current->reading_size * READING_BASE_SIZE / 100;
	int editor = current->editor_size * EDITOR_BASE_SIZE / 100;
	g_autofree char *css = g_strdup_printf(
		".content.document { font-size: calc(var(--document-font-size) * %d / 100); }\n"
		".content .monospace { font-size: calc(var(--document-font-size) * %d / 100); }\n"
		"textview.editor { font-size: calc(var(--document-font-size) * %d / 100);%s }",
		reading, reading, editor, family);
	gtk_css_provider_load_from_string(style, css);
	set_document_zoom(reading);
}
