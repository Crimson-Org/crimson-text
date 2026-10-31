#pragma once

#include "common/smooth-scroll.h"
#include "window/tab.h"

/* Room around the editor text, shared by its setup and by reading its top line */
#define EDITOR_MARGIN 28
#define FIND_GLIDE_DURATION 300

/* Where the reader was, for going back and forward */
typedef struct {
	gboolean is_editing;
	int line;
} Place;

void change_tab_editing(Tab *tab, gboolean is_editing, int line);
int get_view_line(Tab *tab);
void request_view_line(Tab *tab, int line);
void scroll_editor_to_line(Tab *tab, int line);
int get_editor_line(Tab *tab);
void follow_view_in_editor(Tab *tab);
void remember_place(Tab *tab);
GtkWidget *add_block_edit(Tab *tab);
void add_block_menu(Tab *tab, GtkWidget *view);
