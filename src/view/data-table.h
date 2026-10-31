#pragma once

#include <gtk/gtk.h>

#include "document/document.h"

GtkWidget *create_data_table(void);
void add_data_rows(GtkWidget *table, const Block *block);
void clear_data_table(GtkWidget *table);
int find_in_data_table(GtkWidget *table, const char *query, int step, int *index, int *total);
