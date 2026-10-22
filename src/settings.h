#pragma once

#include <glib.h>

#define MINIMUM_TEXT_SIZE 50
#define MAXIMUM_TEXT_SIZE 300
#define DEFAULT_TEXT_SIZE 100

/* Choices the reader makes in Settings, kept between launches */
typedef struct {
	/* Text sizes as percentages of the default look, which itself scales the system document font */
	int reading_size, editor_size;
	/* Editor font family, or empty for the system monospace font */
	char *editor_font;
	/* A pencil fades in beside the block under the pointer, as well as Edit Here in the right click menu */
	gboolean shows_edit_pencil;
} Settings;

Settings *get_settings(void);
void apply_settings(void);
void save_settings(void);
