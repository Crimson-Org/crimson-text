#pragma once

#include <gtk/gtk.h>

void load_picture(GtkPicture *picture, const char *source, const char *base_directory);
void open_picture_outside(GtkPicture *picture);
void save_picture(GtkPicture *picture);
void copy_picture(GtkPicture *picture);
