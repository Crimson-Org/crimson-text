#pragma once

#include <gtk/gtk.h>

#define PICTURE_FLOW_TYPE (picture_flow_get_type())
G_DECLARE_FINAL_TYPE(PictureFlow, picture_flow, PICTURE, FLOW, GtkWidget)

GtkWidget *create_picture_flow(gboolean is_centered);
void add_flow_picture(PictureFlow *flow, GtkWidget *picture, int requested_width);
