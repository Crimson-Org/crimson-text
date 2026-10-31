#include "editor/code-editor.h"
#include "common/long-lines.h"
#include "editor/source-view.h"


/* Code files have no separate view, so the editor with colours and line numbers is how they are read */
void start_code_editor(Tab *tab, const char *language) {
	tab->is_code = TRUE;
	tab->is_editing = TRUE;
	tab->is_editor_filled = TRUE;
	g_autofree char *name = get_tab_title(tab);
	prepare_source_view(GTK_SOURCE_VIEW(tab->editor), name, language);
	/* The language is set once all the text is in, since matching every arriving run took seconds for big files */
	GtkSourceBuffer *buffer = GTK_SOURCE_BUFFER(gtk_text_view_get_buffer(GTK_TEXT_VIEW(tab->editor)));
	tab->code_language = gtk_source_buffer_get_language(buffer);
	gtk_source_buffer_set_language(buffer, NULL);
	show_file_line_numbers(GTK_SOURCE_VIEW(tab->editor));
	gtk_source_view_set_highlight_current_line(GTK_SOURCE_VIEW(tab->editor), TRUE);
	gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(tab->editor), GTK_WRAP_NONE);
	/* Code keeps the whole window, since its lines are long and do not wrap */
	adw_clamp_scrollable_set_maximum_size(ADW_CLAMP_SCROLLABLE(tab->editor_clamp), G_MAXINT);
	/* The text streams in first, and typing into it before it is complete would save a partial file */
	gtk_text_view_set_editable(GTK_TEXT_VIEW(tab->editor), FALSE);
	gtk_stack_set_visible_child_name(GTK_STACK(tab->root), "edit");
	tab->events.state_changed(tab, tab->events.data);
}


/* Appends text exactly as read, outside the undo history and without marking the tab unsaved */
void append_code_text(Tab *tab, const char *text) {
	GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tab->editor));
	GtkTextIter end;
	gtk_text_buffer_get_end_iter(buffer, &end);
	gboolean was_modified = gtk_text_buffer_get_modified(buffer);
	gtk_text_buffer_begin_irreversible_action(buffer);
	gtk_text_buffer_insert(buffer, &end, text, -1);
	gtk_text_buffer_end_irreversible_action(buffer);
	gtk_text_buffer_set_modified(buffer, was_modified);
}


void add_code_block(Tab *tab, const Block *block) {
	if (!tab->is_code)
		start_code_editor(tab, block->label);
	if (block->checked)
		mark_tab_read_only(tab);
	append_code_text(tab, block->text);
	if (block->level != 1)
		return;
	GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tab->editor));
	GtkTextIter start;
	gtk_text_buffer_get_start_iter(buffer, &start);
	gtk_text_buffer_place_cursor(buffer, &start);
}


void finish_code_loading(Tab *tab) {
	gtk_text_view_set_editable(GTK_TEXT_VIEW(tab->editor), !tab->is_read_only);
	/* A file with split lines stays without colours, as the split line watch decided while it streamed in */
	GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tab->editor));
	if (!has_split_lines(buffer))
		gtk_source_buffer_set_language(GTK_SOURCE_BUFFER(buffer), tab->code_language);
}
