#pragma once

#include <adwaita.h>
#include <gtksourceview/gtksource.h>

#include "render.h"
#include "stream.h"

typedef struct Tab Tab;

/* What a tab tells its window about */
typedef struct {
	void (*outline_changed)(Tab *tab, gpointer data);
	void (*state_changed)(Tab *tab, gpointer data);
	void (*open_link)(Tab *tab, const char *link, gpointer data);
	void (*show_message)(Tab *tab, const char *message, gpointer data);
	/* Saving needs the file's folder, which does not exist yet */
	void (*folder_missing)(Tab *tab, gpointer data);
	/* Editor matches finished counting in the background */
	void (*find_counted)(Tab *tab, gpointer data);
	gpointer data;
} TabEvents;

/* One open document with its rendered view and its editor */
struct Tab {
	TabEvents events;
	AdwTabPage *page;
	GtkWidget *root, *overlay, *scroller, *clamp, *content, *editor, *editor_clamp, *error_page;
	/* Column view page for CSV and TSV files, made when the first rows arrive */
	GtkWidget *data_table;
	/* Pencil that follows the pointer and opens the editor at the line beside it */
	GtkWidget *block_edit;
	/* Source lines of the block under the pencil and of the block the right click menu was opened on */
	int hover_line, menu_line;
	/* Right click menu over the view with the actions it offers */
	GtkWidget *view_menu;
	GSimpleActionGroup *view_actions;
	/* Copy button over the code block under the pointer, with that block's range in the view */
	GtkWidget *code_copy;
	int copy_start, copy_end;
	/* Last pointer position over the view, so the pencil can follow content scrolling under it */
	double pointer_x, pointer_y;
	gboolean has_pointer;
	/* Content built off screen while a changed document renders again */
	GtkWidget *next_content;
	GFile *file;
	GFileMonitor *monitor;
	GCancellable *file_cancellable;
	/* Text that is rendered, text last read from or written to disk, and the file version it came from */
	char *source, *disk_text, *etag;
	/* Charset the file is saved in, or nothing for UTF-8 */
	char *charset;
	/* The shown content's context, and the one filled while changed content renders off screen */
	RenderContext context, next_context;
	/* Saving would not give back the file exactly, so it is shown without editing */
	gboolean is_read_only;
	/* The tab left its window and must not report to it any more */
	gboolean is_closed, is_closing_after_save;
	/* Heading to show once the document has loaded, from a link such as other.md#section */
	char *pending_anchor;
	guint outline_generation;
	Stream *stream;
	/* Source line the view should show once its content is laid out */
	int pending_line;
	/* Tick callbacks waiting for layout before moving the view or the editor */
	guint landing_handler, editor_landing_handler;
	/* Earlier and later places for the back and forward buttons */
	GArray *back_places, *forward_places;
	/* Number of the selected search match in the view, and the buffer where it is marked */
	int find_index;
	GtkTextBuffer *find_buffer;
	gboolean is_editing, is_editor_filled;
	/* Markdown and checklists have headings for the contents sidebar, other kinds of file do not */
	gboolean has_contents;
	/* The editor was asked for before the text arrived */
	gboolean is_editing_requested;
	/* Code files are read and edited in the editor alone, coloured by their language */
	gboolean is_code;
	/* Finds and marks matches in the editor, made on the first search there */
	GtkSourceSearchContext *editor_search;
};

Tab *create_tab(GFile *file, const TabEvents *events);
Tab *get_tab(GtkWidget *root);
char *get_tab_title(Tab *tab);
gboolean is_tab_modified(Tab *tab);
gboolean can_switch_tab_editing(Tab *tab);
void mark_tab_read_only(Tab *tab);
void request_tab_editing(Tab *tab);
void set_tab_editing(Tab *tab, gboolean is_editing);
void save_tab(Tab *tab);
void save_and_close_tab(Tab *tab);
/* Creates the missing folder and saves, or ends the save and any close waiting for it */
void finish_tab_folder_question(Tab *tab, gboolean is_creating);
void shut_down_tab(Tab *tab);
void show_tab_anchor(Tab *tab, const char *anchor);
void scroll_tab_to(Tab *tab, GtkTextMark *target);
void show_tab_heading(Tab *tab, const OutlineEntry *entry);
gboolean go_back_in_tab(Tab *tab);
gboolean go_forward_in_tab(Tab *tab);
gboolean scroll_tab_to_anchor(Tab *tab, const char *anchor);
