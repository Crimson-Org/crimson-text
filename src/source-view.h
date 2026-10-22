#pragma once

#include <gtksourceview/gtksource.h>

GtkSourceLanguage *find_source_language(const char *file_name, const char *language);
void follow_style_scheme(GtkSourceBuffer *buffer);
GtkWidget *create_source_view(void);
void prepare_source_view(GtkSourceView *view, const char *file_name, const char *language);
/* Receives one coloured run of code as character offsets with the tag that colours it */
typedef void (*CodeRunFound)(int start, int end, GtkTextTag *tag, gpointer data);

void highlight_code(const char *text, const char *language, CodeRunFound found, gpointer data);
/* Numbers the gutter by the file's lines, leaving the later pieces of a split line blank */
void show_file_line_numbers(GtkSourceView *view);
