#include <adwaita.h>

#include "lang/en.h"
#include "app/settings-dialog.h"
#include "app/settings.h"

#define SIZE_STEP 10
#define SYSTEM_MONOSPACE "Monospace"
#define DIALOG_WIDTH 520


static void store_settings(void) {
	apply_settings();
	save_settings();
}


static void change_size(AdwSpinRow *row, GParamSpec *property, int *size) {
	*size = (int)adw_spin_row_get_value(row);
	store_settings();
}


/* Text size as a percentage of the default look */
static GtkWidget *create_size_row(const char *title, const char *subtitle, int *size) {
	GtkWidget *row = adw_spin_row_new_with_range(MINIMUM_TEXT_SIZE, MAXIMUM_TEXT_SIZE, SIZE_STEP);
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
	adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
	adw_spin_row_set_value(ADW_SPIN_ROW(row), *size);
	g_signal_connect(row, "notify::value", G_CALLBACK(change_size), size);
	return row;
}


static void switch_edit_pencil(AdwSwitchRow *row, GParamSpec *property, gpointer data) {
	get_settings()->shows_edit_pencil = adw_switch_row_get_active(row);
	save_settings();
}


static GtkWidget *create_pencil_row(void) {
	GtkWidget *row = adw_switch_row_new();
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), TEXT_EDIT_PENCIL);
	adw_action_row_set_subtitle(ADW_ACTION_ROW(row), TEXT_EDIT_PENCIL_DETAIL);
	adw_switch_row_set_active(ADW_SWITCH_ROW(row), get_settings()->shows_edit_pencil);
	g_signal_connect(row, "notify::active", G_CALLBACK(switch_edit_pencil), NULL);
	return row;
}


static void choose_font(GtkFontDialogButton *button, GParamSpec *property, gpointer data) {
	PangoFontDescription *font = gtk_font_dialog_button_get_font_desc(button);
	const char *family = font ? pango_font_description_get_family(font) : NULL;
	/* The family goes into a quoted style rule, so quotes and backslashes in a name are left out */
	g_autofree char *safe = g_strdup(family && g_strcmp0(family, SYSTEM_MONOSPACE) != 0 ? family : "");
	g_strdelimit(safe, "\"\\", ' ');
	g_set_str(&get_settings()->editor_font, g_strstrip(safe));
	store_settings();
}


static void use_system_font(GtkButton *reset, GtkFontDialogButton *button) {
	PangoFontDescription *font = pango_font_description_from_string(SYSTEM_MONOSPACE);
	gtk_font_dialog_button_set_font_desc(button, font);
	pango_font_description_free(font);
}


static GtkWidget *create_font_row(void) {
	GtkFontDialog *dialog = gtk_font_dialog_new();
	GtkWidget *button = gtk_font_dialog_button_new(dialog);
	gtk_font_dialog_button_set_level(GTK_FONT_DIALOG_BUTTON(button), GTK_FONT_LEVEL_FAMILY);
	const char *family = get_settings()->editor_font;
	PangoFontDescription *font = pango_font_description_from_string(*family ? family : SYSTEM_MONOSPACE);
	gtk_font_dialog_button_set_font_desc(GTK_FONT_DIALOG_BUTTON(button), font);
	pango_font_description_free(font);
	gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
	g_signal_connect(button, "notify::font-desc", G_CALLBACK(choose_font), NULL);
	GtkWidget *reset = gtk_button_new_from_icon_name("edit-undo-symbolic");
	gtk_widget_set_tooltip_text(reset, TEXT_SYSTEM_FONT);
	gtk_widget_set_valign(reset, GTK_ALIGN_CENTER);
	gtk_widget_add_css_class(reset, "flat");
	g_signal_connect(reset, "clicked", G_CALLBACK(use_system_font), button);
	GtkWidget *row = adw_action_row_new();
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), TEXT_EDITOR_FONT);
	adw_action_row_add_suffix(ADW_ACTION_ROW(row), reset);
	adw_action_row_add_suffix(ADW_ACTION_ROW(row), button);
	return row;
}


void show_settings(GtkWidget *parent) {
	Settings *settings = get_settings();
	GtkWidget *reading = adw_preferences_group_new();
	adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(reading), TEXT_READING);
	adw_preferences_group_add(ADW_PREFERENCES_GROUP(reading),
		create_size_row(TEXT_TEXT_SIZE, TEXT_READING_SIZE_DETAIL, &settings->reading_size));
	adw_preferences_group_add(ADW_PREFERENCES_GROUP(reading), create_pencil_row());
	GtkWidget *editing = adw_preferences_group_new();
	adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(editing), TEXT_EDITING);
	adw_preferences_group_add(ADW_PREFERENCES_GROUP(editing),
		create_size_row(TEXT_TEXT_SIZE, TEXT_EDITOR_SIZE_DETAIL, &settings->editor_size));
	adw_preferences_group_add(ADW_PREFERENCES_GROUP(editing), create_font_row());
	GtkWidget *page = adw_preferences_page_new();
	gtk_widget_set_size_request(page, DIALOG_WIDTH, -1);
	adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(reading));
	adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(editing));
	AdwDialog *dialog = adw_preferences_dialog_new();
	adw_dialog_set_title(dialog, TEXT_SETTINGS);
	/* The few rows fit without scrolling, so the dialog takes their height instead of its default one */
	adw_dialog_set_follows_content_size(dialog, TRUE);
	adw_preferences_dialog_add(ADW_PREFERENCES_DIALOG(dialog), ADW_PREFERENCES_PAGE(page));
	adw_dialog_present(dialog, parent);
}
