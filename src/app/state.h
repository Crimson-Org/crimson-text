#pragma once

#include "common/window-geometry.h"

/* Window layout kept between launches */
typedef struct {
	WindowGeometry geometry;
	gboolean shows_sidebar;
} WindowState;

void load_window_state(WindowState *state);
void save_window_state(const WindowState *state);
