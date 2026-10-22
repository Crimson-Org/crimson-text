#include <math.h>

#include "../lang/en.h"
#include "images.h"
#include "picture-flow.h"

#define PICTURE_SPACING 4
#define MISSING_PICTURE_SIZE 32
#define MAXIMUM_LINE_PICTURES 64

/* Lays out a run of pictures in wrapping lines, scaling each one down to fit and never demanding width */
struct _PictureFlow {
	GtkWidget parent_instance;
	gboolean is_centered;
	/* One context menu shared by the pictures, with the picture it was opened on */
	GtkWidget *menu, *menu_picture;
};

G_DEFINE_FINAL_TYPE(PictureFlow, picture_flow, GTK_TYPE_WIDGET)

/* Shown size of one picture for an available width */
typedef struct {
	GtkWidget *picture;
	double width, height;
} PictureSize;


/* Positive requested widths are pixels and negative ones are percentages of the available width */
static PictureSize get_picture_size(GtkWidget *picture, double available) {
	PictureSize size = {picture, 0, 0};
	GdkPaintable *paintable = gtk_picture_get_paintable(GTK_PICTURE(picture));
	if (!paintable)
		return size;
	double intrinsic_width = gdk_paintable_get_intrinsic_width(paintable);
	double intrinsic_height = gdk_paintable_get_intrinsic_height(paintable);
	if (intrinsic_width <= 0 || intrinsic_height <= 0)
		intrinsic_width = intrinsic_height = MISSING_PICTURE_SIZE;
	int requested = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(picture), "requested-width"));
	size.width = intrinsic_width;
	if (requested > 0)
		size.width = requested;
	else if (requested < 0 && isfinite(available))
		size.width = available * -requested / 100.0 - PICTURE_SPACING;
	size.width = floor(MAX(1, MIN(size.width, available)));
	size.height = floor(size.width * intrinsic_height / intrinsic_width);
	return size;
}


static void place_line(PictureFlow *flow, PictureSize *line, int count, double line_width, double available,
	double y) {
	double x = flow->is_centered ? floor((available - line_width) / 2) : 0;
	for (int index = 0; index < count; index++) {
		GtkAllocation allocation = {(int)x, (int)y, (int)line[index].width, (int)line[index].height};
		gtk_widget_size_allocate(line[index].picture, &allocation, -1);
		x += line[index].width + PICTURE_SPACING;
	}
}


/* Fills lines left to right and returns the total height, placing the pictures when asked */
static double lay_out_pictures(PictureFlow *flow, double available, gboolean should_place) {
	PictureSize line[MAXIMUM_LINE_PICTURES];
	int count = 0;
	double y = 0, line_width = 0, line_height = 0;
	GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(flow));
	for (; child; child = gtk_widget_get_next_sibling(child)) {
		if (!GTK_IS_PICTURE(child))
			continue;
		PictureSize size = get_picture_size(child, available);
		gboolean is_full = count == MAXIMUM_LINE_PICTURES
			|| (count > 0 && line_width + PICTURE_SPACING + size.width > available);
		if (is_full) {
			if (should_place)
				place_line(flow, line, count, line_width, available, y);
			y += line_height + PICTURE_SPACING;
			count = 0;
			line_width = line_height = 0;
		}
		line_width += (count > 0 ? PICTURE_SPACING : 0) + size.width;
		line_height = MAX(line_height, size.height);
		line[count++] = size;
	}
	if (should_place && count > 0)
		place_line(flow, line, count, line_width, available, y);
	return y + line_height;
}


static void measure_flow(GtkWidget *widget, GtkOrientation orientation, int for_size, int *minimum, int *natural,
	int *minimum_baseline, int *natural_baseline) {
	PictureFlow *flow = PICTURE_FLOW(widget);
	if (orientation == GTK_ORIENTATION_HORIZONTAL) {
		double width = 0;
		for (GtkWidget *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child))
			if (GTK_IS_PICTURE(child))
				width += get_picture_size(child, INFINITY).width + PICTURE_SPACING;
		*minimum = 0;
		*natural = (int)MAX(0, width - PICTURE_SPACING);
		return;
	}
	if (for_size < 0)
		measure_flow(widget, GTK_ORIENTATION_HORIZONTAL, -1, minimum, &for_size, NULL, NULL);
	*minimum = *natural = (int)ceil(lay_out_pictures(flow, MAX(for_size, 1), FALSE));
}


static void allocate_flow(GtkWidget *widget, int width, int height, int baseline) {
	PictureFlow *flow = PICTURE_FLOW(widget);
	lay_out_pictures(flow, MAX(width, 1), TRUE);
	if (flow->menu)
		gtk_popover_present(GTK_POPOVER(flow->menu));
}


static GtkSizeRequestMode get_flow_request_mode(GtkWidget *widget) {
	return GTK_SIZE_REQUEST_HEIGHT_FOR_WIDTH;
}


static void dispose_flow(GObject *object) {
	GtkWidget *child;
	while ((child = gtk_widget_get_first_child(GTK_WIDGET(object))))
		gtk_widget_unparent(child);
	G_OBJECT_CLASS(picture_flow_parent_class)->dispose(object);
}


static void picture_flow_class_init(PictureFlowClass *class) {
	G_OBJECT_CLASS(class)->dispose = dispose_flow;
	GTK_WIDGET_CLASS(class)->measure = measure_flow;
	GTK_WIDGET_CLASS(class)->size_allocate = allocate_flow;
	GTK_WIDGET_CLASS(class)->get_request_mode = get_flow_request_mode;
}


static void run_menu_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	PictureFlow *flow = data;
	const char *name = g_action_get_name(G_ACTION(action));
	if (!flow->menu_picture)
		return;
	if (g_str_equal(name, "open"))
		open_picture_outside(GTK_PICTURE(flow->menu_picture));
	else if (g_str_equal(name, "save"))
		save_picture(GTK_PICTURE(flow->menu_picture));
	else
		copy_picture(GTK_PICTURE(flow->menu_picture));
}


static void picture_flow_init(PictureFlow *flow) {
	static const GActionEntry ENTRIES[] = {
		{"open", run_menu_action, NULL, NULL, NULL, {0}},
		{"save", run_menu_action, NULL, NULL, NULL, {0}},
		{"copy", run_menu_action, NULL, NULL, NULL, {0}},
	};
	g_autoptr(GSimpleActionGroup) actions = g_simple_action_group_new();
	g_action_map_add_action_entries(G_ACTION_MAP(actions), ENTRIES, G_N_ELEMENTS(ENTRIES), flow);
	gtk_widget_insert_action_group(GTK_WIDGET(flow), "picture", G_ACTION_GROUP(actions));
}


static void show_picture_menu(GtkGestureClick *gesture, int presses, double x, double y, PictureFlow *flow) {
	GtkWidget *picture = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
	if (!flow->menu) {
		g_autoptr(GMenu) model = g_menu_new();
		g_menu_append(model, TEXT_OPEN_PICTURE, "picture.open");
		g_menu_append(model, TEXT_SAVE_PICTURE, "picture.save");
		g_menu_append(model, TEXT_COPY_PICTURE, "picture.copy");
		flow->menu = gtk_popover_menu_new_from_model(G_MENU_MODEL(model));
		gtk_popover_set_has_arrow(GTK_POPOVER(flow->menu), FALSE);
		gtk_widget_set_parent(flow->menu, GTK_WIDGET(flow));
	}
	graphene_point_t point;
	if (!gtk_widget_compute_point(picture, GTK_WIDGET(flow), &GRAPHENE_POINT_INIT(x, y), &point))
		return;
	flow->menu_picture = picture;
	gtk_popover_set_pointing_to(GTK_POPOVER(flow->menu), &(GdkRectangle){(int)point.x, (int)point.y, 1, 1});
	gtk_popover_popup(GTK_POPOVER(flow->menu));
}


GtkWidget *create_picture_flow(gboolean is_centered) {
	PictureFlow *flow = g_object_new(PICTURE_FLOW_TYPE, NULL);
	flow->is_centered = is_centered;
	return GTK_WIDGET(flow);
}


void add_flow_picture(PictureFlow *flow, GtkWidget *picture, int requested_width) {
	g_object_set_data(G_OBJECT(picture), "requested-width", GINT_TO_POINTER(requested_width));
	gtk_picture_set_content_fit(GTK_PICTURE(picture), GTK_CONTENT_FIT_FILL);
	gtk_picture_set_can_shrink(GTK_PICTURE(picture), TRUE);
	GtkGesture *menu_click = gtk_gesture_click_new();
	gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(menu_click), GDK_BUTTON_SECONDARY);
	g_signal_connect(menu_click, "pressed", G_CALLBACK(show_picture_menu), flow);
	gtk_widget_add_controller(picture, GTK_EVENT_CONTROLLER(menu_click));
	gtk_widget_set_parent(picture, GTK_WIDGET(flow));
}
