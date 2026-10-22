#include "common/key-store.h"
#include "state.h"

#define STATE_FILE "window.ini"
#define STATE_GROUP "window"
#define DEFAULT_WIDTH 920
#define DEFAULT_HEIGHT 1000


void load_window_state(WindowState *state) {
	g_autofree char *path = get_state_file_path(STATE_FILE);
	g_autoptr(GKeyFile) file = load_key_file(path, G_KEY_FILE_NONE);
	load_window_geometry(file, &state->geometry, DEFAULT_WIDTH, DEFAULT_HEIGHT);
	state->shows_sidebar = g_key_file_get_boolean(file, STATE_GROUP, "sidebar", NULL);
}


void save_window_state(const WindowState *state) {
	g_autofree char *path = get_state_file_path(STATE_FILE);
	g_autoptr(GKeyFile) file = g_key_file_new();
	store_window_geometry(file, &state->geometry);
	g_key_file_set_boolean(file, STATE_GROUP, "sidebar", state->shows_sidebar);
	save_key_file(file, path);
}
