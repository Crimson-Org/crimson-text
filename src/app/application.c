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


static gboolean open_requested_files(gpointer data) {
	OpenRequest *request = data;
	for (guint index = 0; index < request->files->len; index++) {
		Tab *tab = open_window_file(request->window, g_ptr_array_index(request->files, index));
		if (request->is_editing)
			request_tab_editing(tab);
	}
	return G_SOURCE_REMOVE;
}


static void free_open_request(gpointer data) {
	OpenRequest *request = data;
	g_ptr_array_unref(request->files);
	g_free(request);
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
	run_after_frame(window, open_requested_files, request, free_open_request);
}


static void start_up(GApplication *application) {
	apply_settings();
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

static const Shortcut SHORTCUTS[] = {
	{"win.open", {"<Control>o", NULL}},
	{"win.save", {"<Control>s", NULL}},
	{"win.edit", {"<Control>e", NULL}},
	{"win.close-tab", {"<Control>w", NULL}},
	{"win.sidebar", {"F9", NULL}},
	{"win.back", {"<Alt>Left", NULL}},
	{"win.forward", {"<Alt>Right", NULL}},
	{"win.zoom-in", {"<Control>plus", "<Control>equal", "<Control>KP_Add", NULL}},
	{"win.zoom-out", {"<Control>minus", "<Control>KP_Subtract", NULL}},
	{"win.zoom-reset", {"<Control>0", "<Control>KP_0", NULL}},
	{"win.find", {"<Control>f", NULL}},
};

const ApplicationSetup APPLICATION_SETUP = {
	.identity = {"io.github.CrimsonOrg.CrimsonText", TEXT_APPLICATION_NAME, "crimson-text", APPLICATION_VERSION},
	.start_up = start_up,
	.activate = activate,
	.open = open_files,
	.handle_options = handle_options,
	.options = OPTIONS,
	.show_settings = show_settings,
	.shortcuts = SHORTCUTS,
	.shortcut_count = G_N_ELEMENTS(SHORTCUTS),
};
