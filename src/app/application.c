#include <adwaita.h>

#include "lang/en.h"
#include "app/application.h"
#include "app/settings-dialog.h"
#include "app/settings.h"
#include "window/window.h"

/* Hint that asks for the opened files in their editor, which also reaches an instance already running */
#define EDIT_HINT "edit"


static Window *get_window(GtkApplication *application) {
	GtkWindow *active = gtk_application_get_active_window(application);
	if (active)
		return g_object_get_data(G_OBJECT(active), "window");
	return create_window(application);
}


static void activate(GApplication *application) {
	Window *window = get_window(GTK_APPLICATION(application));
	gtk_window_present(get_gtk_window(window));
	show_window_start(window);
}


/* Files waiting for the window's first frame, with whether they open in the editor */
typedef struct {
	Window *window;
	GPtrArray *files;
	gboolean is_editing;
} OpenRequest;


static gboolean open_requested_files(GtkWidget *widget, GdkFrameClock *clock, gpointer data) {
	OpenRequest *request = data;
	for (guint index = 0; index < request->files->len; index++) {
		Tab *tab = open_window_file(request->window, g_ptr_array_index(request->files, index));
		if (request->is_editing)
			request_tab_editing(tab);
	}
	g_ptr_array_unref(request->files);
	g_free(request);
	return G_SOURCE_REMOVE;
}


/* Shows the window first and opens the files on its next frame, so something is on screen at once */
static void open_files(GApplication *application, GFile **files, int count, const char *hint) {
	OpenRequest *request = g_new0(OpenRequest, 1);
	request->window = get_window(GTK_APPLICATION(application));
	request->files = g_ptr_array_new_with_free_func(g_object_unref);
	request->is_editing = g_strcmp0(hint, EDIT_HINT) == 0;
	for (int index = 0; index < count; index++)
		g_ptr_array_add(request->files, g_object_ref(files[index]));
	GtkWidget *window = GTK_WIDGET(get_gtk_window(request->window));
	gtk_window_present(GTK_WINDOW(window));
	gtk_widget_add_tick_callback(window, open_requested_files, request, NULL);
}


static void show_about(GSimpleAction *action, GVariant *parameter, gpointer application) {
	AdwDialog *dialog = adw_about_dialog_new();
	adw_about_dialog_set_application_name(ADW_ABOUT_DIALOG(dialog), get_application_identity()->name);
	adw_about_dialog_set_application_icon(ADW_ABOUT_DIALOG(dialog), get_application_identity()->id);
	adw_about_dialog_set_version(ADW_ABOUT_DIALOG(dialog), get_application_identity()->version);
	adw_dialog_present(dialog, GTK_WIDGET(gtk_application_get_active_window(application)));
}


static void open_settings(GSimpleAction *action, GVariant *parameter, gpointer application) {
	show_settings(GTK_WIDGET(gtk_application_get_active_window(application)));
}


static void quit(GSimpleAction *action, GVariant *parameter, gpointer application) {
	GtkWindow *window = gtk_application_get_active_window(application);
	if (window)
		gtk_window_close(window);
}


static void start_up(GApplication *application) {
	static const GActionEntry ENTRIES[] = {
		{"about", show_about, NULL, NULL, NULL, {0}},
		{"settings", open_settings, NULL, NULL, NULL, {0}},
		{"quit", quit, NULL, NULL, NULL, {0}},
	};
	static const struct {
		const char *action, *accelerators[4];
	} SHORTCUTS[] = {
		{"win.open", {"<Control>o", NULL}},
		{"win.save", {"<Control>s", NULL}},
		{"win.edit", {"<Control>e", NULL}},
		{"win.close-tab", {"<Control>w", NULL}},
		{"win.sidebar", {"F9", NULL}},
		{"win.back", {"<Alt>Left", NULL}},
		{"win.zoom-in", {"<Control>plus", "<Control>equal", "<Control>KP_Add", NULL}},
		{"win.zoom-out", {"<Control>minus", "<Control>KP_Subtract", NULL}},
		{"win.zoom-reset", {"<Control>0", "<Control>KP_0", NULL}},
		{"win.find", {"<Control>f", NULL}},
		{"win.forward", {"<Alt>Right", NULL}},
		{"app.settings", {"<Control>comma", NULL}},
		{"app.quit", {"<Control>q", NULL}},
	};
	g_action_map_add_action_entries(G_ACTION_MAP(application), ENTRIES, G_N_ELEMENTS(ENTRIES), application);
	apply_settings();
	for (guint index = 0; index < G_N_ELEMENTS(SHORTCUTS); index++)
		gtk_application_set_accels_for_action(GTK_APPLICATION(application), SHORTCUTS[index].action,
			SHORTCUTS[index].accelerators);
}


/* Opens the named files itself so it can pass the edit hint, which the default handling would leave out */
static int handle_options(GApplication *application, GVariantDict *options) {
	g_autofree const char **names = NULL;
	if (!g_variant_dict_lookup(options, G_OPTION_REMAINING, "^a&ay", &names))
		return -1;
	g_autoptr(GError) error = NULL;
	if (!g_application_register(application, NULL, &error)) {
		g_printerr("%s\n", error->message);
		return EXIT_FAILURE;
	}
	g_autoptr(GPtrArray) files = g_ptr_array_new_with_free_func(g_object_unref);
	for (int index = 0; names[index]; index++)
		g_ptr_array_add(files, g_file_new_for_commandline_arg(names[index]));
	gboolean is_editing = g_variant_dict_contains(options, "edit");
	g_application_open(application, (GFile **)files->pdata, (int)files->len, is_editing ? EDIT_HINT : "");
	return EXIT_SUCCESS;
}


static const GOptionEntry OPTIONS[] = {
	{"edit", 'e', 0, G_OPTION_ARG_NONE, NULL, TEXT_EDIT_OPTION, NULL},
	{G_OPTION_REMAINING, 0, 0, G_OPTION_ARG_FILENAME_ARRAY, NULL, NULL, TEXT_FILES_OPTION},
	G_OPTION_ENTRY_NULL,
};

const ApplicationSetup APPLICATION_SETUP = {
	.identity = {"io.github.CrimsonOrg.CrimsonText", TEXT_APPLICATION_NAME, "crimson-text", APPLICATION_VERSION},
	.start_up = start_up,
	.activate = activate,
	.open = open_files,
	.handle_options = handle_options,
	.options = OPTIONS,
};
