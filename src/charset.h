#pragma once

#include <glib.h>

gboolean has_byte_order_mark(const char *bytes, gsize length);
char *decode_text(const char *bytes, gsize length, char **charset);
GBytes *encode_text(const char *text, const char *charset, GError **error);
