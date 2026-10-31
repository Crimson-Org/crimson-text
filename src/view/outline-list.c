#include "view/outline-list.h"
#include "view/render.h"

#define OUTLINE_INDENT 14
#define DEEPEST_INDENTED_LEVEL 6

/* One heading handed to a row while it is in view */
struct _OutlineRow {
	GObject parent_instance;
	char *title;
	int level;
};

#define OUTLINE_ROW_TYPE (outline_row_get_type())
G_DECLARE_FINAL_TYPE(OutlineRow, outline_row, CRIMSON, OUTLINE_ROW, GObject)
G_DEFINE_FINAL_TYPE(OutlineRow, outline_row, G_TYPE_OBJECT)

/* The headings of one document as a list model, counting only the ones announced so far */
struct _OutlineEntries {
	GObject parent_instance;
	GPtrArray *entries;
	guint count;
};

#define OUTLINE_ENTRIES_TYPE (outline_entries_get_type())
G_DECLARE_FINAL_TYPE(OutlineEntries, outline_entries, CRIMSON, OUTLINE_ENTRIES, GObject)

static void outline_entries_list_init(GListModelInterface *interface);

G_DEFINE_FINAL_TYPE_WITH_CODE(OutlineEntries, outline_entries, G_TYPE_OBJECT,
	G_IMPLEMENT_INTERFACE(G_TYPE_LIST_MODEL, outline_entries_list_init))


static void finalize_outline_row(GObject *object) {
	g_free(CRIMSON_OUTLINE_ROW(object)->title);
	G_OBJECT_CLASS(outline_row_parent_class)->finalize(object);
}


static void outline_row_class_init(OutlineRowClass *class) {
	G_OBJECT_CLASS(class)->finalize = finalize_outline_row;
}


static void outline_row_init(OutlineRow *row) {
}


static GType get_entry_type(GListModel *model) {
	return OUTLINE_ROW_TYPE;
}


static guint count_entries(GListModel *model) {
	return CRIMSON_OUTLINE_ENTRIES(model)->count;
}


static gpointer get_entry(GListModel *model, guint position) {
	OutlineEntries *entries = CRIMSON_OUTLINE_ENTRIES(model);
	if (position >= entries->count)
		return NULL;
	const OutlineEntry *entry = g_ptr_array_index(entries->entries, position);
	OutlineRow *row = g_object_new(OUTLINE_ROW_TYPE, NULL);
	row->title = g_strdup(entry->title);
	row->level = entry->level;
	return row;
}


static void outline_entries_list_init(GListModelInterface *interface) {
	interface->get_item_type = get_entry_type;
	interface->get_n_items = count_entries;
	interface->get_item = get_entry;
}


static void finalize_outline_entries(GObject *object) {
	g_clear_pointer(&CRIMSON_OUTLINE_ENTRIES(object)->entries, g_ptr_array_unref);
	G_OBJECT_CLASS(outline_entries_parent_class)->finalize(object);
}


static void outline_entries_class_init(OutlineEntriesClass *class) {
	G_OBJECT_CLASS(class)->finalize = finalize_outline_entries;
}


static void outline_entries_init(OutlineEntries *entries) {
}


static void set_up_row(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer data) {
	GtkWidget *label = gtk_label_new(NULL);
	gtk_label_set_xalign(GTK_LABEL(label), 0);
	gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
	gtk_list_item_set_child(item, label);
}


static void bind_row(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer data) {
	OutlineRow *row = gtk_list_item_get_item(item);
	GtkWidget *label = gtk_list_item_get_child(item);
	gtk_label_set_text(GTK_LABEL(label), row->title);
	gtk_widget_set_tooltip_text(label, row->title);
	gtk_widget_set_margin_start(label, OUTLINE_INDENT * CLAMP(row->level - 1, 0, DEEPEST_INDENTED_LEVEL - 1));
	if (row->level == 1)
		gtk_widget_add_css_class(label, "outline-top");
	else
		gtk_widget_remove_css_class(label, "outline-top");
}


static OutlineEntries *get_entries(GtkWidget *list) {
	return CRIMSON_OUTLINE_ENTRIES(gtk_no_selection_get_model(GTK_NO_SELECTION(
		gtk_list_view_get_model(GTK_LIST_VIEW(list)))));
}


/* The chosen function and its data, kept with the list for the activate signal */
typedef struct {
	OutlineChosen chosen;
	gpointer data;
} OutlineChoice;


static void choose_heading(GtkListView *list, guint position, OutlineChoice *choice) {
	choice->chosen(position, choice->data);
}


static void free_choice(gpointer choice, GClosure *closure) {
	g_free(choice);
}


GtkWidget *create_outline_list(OutlineChosen chosen, gpointer data) {
	GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
	g_signal_connect(factory, "setup", G_CALLBACK(set_up_row), NULL);
	g_signal_connect(factory, "bind", G_CALLBACK(bind_row), NULL);
	OutlineEntries *entries = g_object_new(OUTLINE_ENTRIES_TYPE, NULL);
	GtkWidget *list = gtk_list_view_new(GTK_SELECTION_MODEL(gtk_no_selection_new(G_LIST_MODEL(entries))), factory);
	gtk_list_view_set_single_click_activate(GTK_LIST_VIEW(list), TRUE);
	OutlineChoice *choice = g_new(OutlineChoice, 1);
	*choice = (OutlineChoice){chosen, data};
	g_signal_connect_data(list, "activate", G_CALLBACK(choose_heading), choice, free_choice, 0);
	return list;
}


void show_outline_entries(GtkWidget *list, GPtrArray *entries, gboolean is_new_document) {
	OutlineEntries *model = get_entries(list);
	if (is_new_document || model->entries != entries) {
		guint removed = model->count;
		g_clear_pointer(&model->entries, g_ptr_array_unref);
		model->entries = entries ? g_ptr_array_ref(entries) : NULL;
		model->count = 0;
		if (removed)
			g_list_model_items_changed(G_LIST_MODEL(model), 0, removed, 0);
	}
	guint total = model->entries ? model->entries->len : 0;
	if (total <= model->count)
		return;
	guint added = total - model->count;
	model->count = total;
	g_list_model_items_changed(G_LIST_MODEL(model), total - added, 0, added);
}


guint count_outline_entries(GtkWidget *list) {
	return get_entries(list)->count;
}
