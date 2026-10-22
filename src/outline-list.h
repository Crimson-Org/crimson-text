#pragma once

#include <gtk/gtk.h>

/* Called with the index of the heading the reader chose */
typedef void (*OutlineChosen)(guint index, gpointer data);

/* A recycling list of a document's headings, which builds rows only for the headings in view */
GtkWidget *create_outline_list(OutlineChosen chosen, gpointer data);
/* Shows the headings in the array, which may grow, starting over when they belong to another document */
void show_outline_entries(GtkWidget *list, GPtrArray *entries, gboolean is_new_document);
guint count_outline_entries(GtkWidget *list);
