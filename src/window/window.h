#pragma once

#include <adwaita.h>

#include "window/tab.h"

typedef struct Window Window;

Window *create_window(GtkApplication *application);
GtkWindow *get_gtk_window(Window *window);
Tab *open_window_file(Window *window, GFile *file);
void show_window_start(Window *window);
