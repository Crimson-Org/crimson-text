#include <string.h>

#include "data-table.h"

/* Shows comma and tab separated files in a recycling column view, which only builds widgets for visible rows */


/* One shown row, made when the view asks for it, with its own copy of the cells */
struct _DataRow {
	GObject parent_instance;
	GStrv cells;
};

#define DATA_ROW_TYPE (data_row_get_type())
G_DECLARE_FINAL_TYPE(DataRow, data_row, CRIMSON, DATA_ROW, GObject)
G_DEFINE_FINAL_TYPE(DataRow, data_row, G_TYPE_OBJECT)


static void finalize_data_row(GObject *object) {
	g_strfreev(CRIMSON_DATA_ROW(object)->cells);
	G_OBJECT_CLASS(data_row_parent_class)->finalize(object);
}


static void data_row_class_init(DataRowClass *class) {
	G_OBJECT_CLASS(class)->finalize = finalize_data_row;
}


static void data_row_init(DataRow *row) {
}


/* Every row's cells as plain strings, which a million row file keeps in a fraction of the memory one object per
 * cell would take, handing row objects out only for the rows on screen */
struct _DataRows {
	GObject parent_instance;
	GPtrArray *rows;
};

#define DATA_ROWS_TYPE (data_rows_get_type())
G_DECLARE_FINAL_TYPE(DataRows, data_rows, CRIMSON, DATA_ROWS, GObject)

static void data_rows_list_init(GListModelInterface *interface);

G_DEFINE_FINAL_TYPE_WITH_CODE(DataRows, data_rows, G_TYPE_OBJECT,
	G_IMPLEMENT_INTERFACE(G_TYPE_LIST_MODEL, data_rows_list_init))


static GType get_row_type(GListModel *model) {
	return DATA_ROW_TYPE;
}


static guint count_rows(GListModel *model) {
	return CRIMSON_DATA_ROWS(model)->rows->len;
}


static gpointer get_row(GListModel *model, guint position) {
	DataRows *rows = CRIMSON_DATA_ROWS(model);
	if (position >= rows->rows->len)
		return NULL;
	DataRow *row = g_object_new(DATA_ROW_TYPE, NULL);
	row->cells = g_strdupv(g_ptr_array_index(rows->rows, position));
	return row;
}


static void data_rows_list_init(GListModelInterface *interface) {
	interface->get_item_type = get_row_type;
	interface->get_n_items = count_rows;
	interface->get_item = get_row;
}


static void finalize_data_rows(GObject *object) {
	g_ptr_array_unref(CRIMSON_DATA_ROWS(object)->rows);
	G_OBJECT_CLASS(data_rows_parent_class)->finalize(object);
}


static void data_rows_class_init(DataRowsClass *class) {
	G_OBJECT_CLASS(class)->finalize = finalize_data_rows;
}


static void data_rows_init(DataRows *rows) {
	rows->rows = g_ptr_array_new_with_free_func(g_free);
}


static GtkColumnView *get_column_view(GtkWidget *table) {
	return GTK_COLUMN_VIEW(gtk_scrolled_window_get_child(GTK_SCROLLED_WINDOW(table)));
}


static DataRows *get_rows(GtkWidget *table) {
	return g_object_get_data(G_OBJECT(table), "rows");
}


static void set_up_cell(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer data) {
	GtkWidget *label = gtk_label_new(NULL);
	gtk_label_set_xalign(GTK_LABEL(label), 0);
	gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
	gtk_list_item_set_child(item, label);
}


static void bind_cell(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer column) {
	DataRow *row = gtk_list_item_get_item(item);
	guint index = GPOINTER_TO_UINT(column);
	const char *text = index < g_strv_length(row->cells) ? row->cells[index] : "";
	gtk_label_set_text(GTK_LABEL(gtk_list_item_get_child(item)), text);
	gtk_widget_set_tooltip_text(gtk_list_item_get_child(item), *text ? text : NULL);
}


static void add_column(GtkColumnView *view, guint index, const char *title) {
	GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
	g_signal_connect(factory, "setup", G_CALLBACK(set_up_cell), NULL);
	g_signal_connect(factory, "bind", G_CALLBACK(bind_cell), GUINT_TO_POINTER(index));
	GtkColumnViewColumn *column = gtk_column_view_column_new(title, factory);
	gtk_column_view_column_set_resizable(column, TRUE);
	gtk_column_view_append_column(view, column);
	g_object_unref(column);
}


static const char *get_cell_text(const Block *row, guint index) {
	const Block *cell = g_ptr_array_index(row->children, index);
	return cell->text ? cell->text : "";
}


/* Packs a row's cell pointers and their text into one allocation, freed with g_free, since separate strings for
 * each cell cost more in allocator overhead than their text */
static GStrv create_cells(const Block *row) {
	gsize pointers = (row->children->len + 1) * sizeof(char *), size = pointers;
	for (guint index = 0; index < row->children->len; index++)
		size += strlen(get_cell_text(row, index)) + 1;
	GStrv cells = g_malloc(size);
	char *text = (char *)cells + pointers;
	for (guint index = 0; index < row->children->len; index++) {
		cells[index] = text;
		text = g_stpcpy(text, get_cell_text(row, index)) + 1;
	}
	cells[row->children->len] = NULL;
	return cells;
}


GtkWidget *create_data_table(void) {
	DataRows *rows = g_object_new(DATA_ROWS_TYPE, NULL);
	GtkSingleSelection *selection = gtk_single_selection_new(G_LIST_MODEL(g_object_ref(rows)));
	gtk_single_selection_set_autoselect(selection, FALSE);
	gtk_single_selection_set_can_unselect(selection, TRUE);
	gtk_single_selection_set_selected(selection, GTK_INVALID_LIST_POSITION);
	GtkWidget *view = gtk_column_view_new(GTK_SELECTION_MODEL(selection));
	gtk_column_view_set_show_row_separators(GTK_COLUMN_VIEW(view), TRUE);
	gtk_column_view_set_show_column_separators(GTK_COLUMN_VIEW(view), TRUE);
	gtk_widget_add_css_class(view, "data-table");
	GtkWidget *table = gtk_scrolled_window_new();
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(table), view);
	g_object_set_data_full(G_OBJECT(table), "rows", rows, g_object_unref);
	return table;
}


/* Adds a streamed batch of rows, taking column titles from the header row and adding columns as rows widen, and
 * tells the view once per batch */
void add_data_rows(GtkWidget *table, const Block *block) {
	GtkColumnView *view = get_column_view(table);
	GListModel *columns = gtk_column_view_get_columns(view);
	DataRows *rows = get_rows(table);
	guint first = rows->rows->len;
	for (guint index = 0; index < block->children->len; index++) {
		const Block *row = g_ptr_array_index(block->children, index);
		GStrv cells = create_cells(row);
		guint width = g_strv_length(cells);
		for (guint column = g_list_model_get_n_items(columns); column < width; column++)
			add_column(view, column, row->header ? cells[column] : "");
		if (row->header)
			g_free(cells);
		else
			g_ptr_array_add(rows->rows, cells);
	}
	if (rows->rows->len > first)
		g_list_model_items_changed(G_LIST_MODEL(rows), first, 0, rows->rows->len - first);
}


void clear_data_table(GtkWidget *table) {
	GtkColumnView *view = get_column_view(table);
	GListModel *columns = gtk_column_view_get_columns(view);
	while (g_list_model_get_n_items(columns) > 0) {
		g_autoptr(GtkColumnViewColumn) column = g_list_model_get_item(columns, 0);
		gtk_column_view_remove_column(view, column);
	}
	DataRows *rows = get_rows(table);
	guint removed = rows->rows->len;
	g_ptr_array_set_size(rows->rows, 0);
	if (removed)
		g_list_model_items_changed(G_LIST_MODEL(rows), 0, removed, 0);
}


/* Selects the row of the next cell holding the text and returns its number counting from one, as find does */
int find_in_data_table(GtkWidget *table, const char *query, int step, int *index, int *total) {
	GPtrArray *rows = get_rows(table)->rows;
	g_autofree char *needle = g_utf8_casefold(query, -1);
	g_autoptr(GArray) matches = g_array_new(FALSE, FALSE, sizeof(guint));
	for (guint row = 0; row < rows->len; row++) {
		GStrv cells = g_ptr_array_index(rows, row);
		for (guint column = 0; cells[column]; column++) {
			g_autofree char *text = g_utf8_casefold(cells[column], -1);
			if (strstr(text, needle))
				g_array_append_val(matches, row);
		}
	}
	*total = (int)matches->len;
	if (matches->len == 0)
		return 0;
	*index = step == 0 ? CLAMP(*index, 0, *total - 1) : (*index + step + *total) % *total;
	guint row = g_array_index(matches, guint, *index);
	gtk_column_view_scroll_to(get_column_view(table), row, NULL, GTK_LIST_SCROLL_SELECT, NULL);
	return *index + 1;
}
