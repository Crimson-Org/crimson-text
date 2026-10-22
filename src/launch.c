#include <adwaita.h>

#include "launch.h"


/* Shows a short message in the toast area of the window holding the widget */
void show_widget_message(GtkWidget *widget, const char *message) {
	GtkWidget *toasts = gtk_widget_get_ancestor(widget, ADW_TYPE_TOAST_OVERLAY);
	if (toasts)
		adw_toast_overlay_add_toast(ADW_TOAST_OVERLAY(toasts), adw_toast_new(message));
	else
		g_warning("%s", message);
}


static void finish_launch(GObject *source, GAsyncResult *result, gpointer data) {
	g_autoptr(GtkWidget) widget = data;
	g_autoptr(GError) error = NULL;
	if (!g_app_info_launch_default_for_uri_finish(result, &error))
		show_widget_message(widget, error->message);
}


/* Opens a location in the user's default application directly, as a file manager does, without a chooser */
void launch_default(GtkWidget *widget, const char *uri) {
	g_autoptr(GdkAppLaunchContext) context = gdk_display_get_app_launch_context(gtk_widget_get_display(widget));
	g_app_info_launch_default_for_uri_async(uri, G_APP_LAUNCH_CONTEXT(context), NULL, finish_launch,
		g_object_ref(widget));
}
