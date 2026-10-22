#include <string.h>

#include "charset.h"

/* Charsets tried for files that are not UTF-8, with the byte order mark that must start them */
static const struct {
	const char *charset, *mark;
	gsize mark_length;
} CHARSETS[] = {
	{"UTF-16LE", "\xff\xfe", 2},
	{"UTF-16BE", "\xfe\xff", 2},
	{"WINDOWS-1252", "", 0},
	{"ISO-8859-1", "", 0},
};


static int find_charset(const char *charset) {
	for (guint index = 0; index < G_N_ELEMENTS(CHARSETS); index++)
		if (g_strcmp0(charset, CHARSETS[index].charset) == 0)
			return (int)index;
	return -1;
}


/* UTF-16 text holds zero bytes, so its mark is what separates it from binary files */
gboolean has_byte_order_mark(const char *bytes, gsize length) {
	for (guint index = 0; index < G_N_ELEMENTS(CHARSETS); index++)
		if (CHARSETS[index].mark_length && length >= CHARSETS[index].mark_length
			&& memcmp(bytes, CHARSETS[index].mark, CHARSETS[index].mark_length) == 0)
			return TRUE;
	return FALSE;
}


GBytes *encode_text(const char *text, const char *charset, GError **error) {
	int index = find_charset(charset);
	if (index < 0)
		return g_bytes_new(text, strlen(text));
	gsize length = 0;
	g_autofree char *converted = g_convert(text, -1, CHARSETS[index].charset, "UTF-8", NULL, &length, error);
	if (!converted)
		return NULL;
	GByteArray *bytes = g_byte_array_sized_new(length + CHARSETS[index].mark_length);
	g_byte_array_append(bytes, (const guint8 *)CHARSETS[index].mark, CHARSETS[index].mark_length);
	g_byte_array_append(bytes, (const guint8 *)converted, length);
	return g_byte_array_free_to_bytes(bytes);
}


/* Turns file bytes into UTF-8 and names the charset they were in, with none for UTF-8 itself
 * Only a charset that writes the very same bytes back counts, so saving never changes the file unasked */
char *decode_text(const char *bytes, gsize length, char **charset) {
	*charset = NULL;
	if (g_utf8_validate_len(bytes, length, NULL))
		return g_strndup(bytes, length);
	for (guint index = 0; index < G_N_ELEMENTS(CHARSETS); index++) {
		gsize mark_length = CHARSETS[index].mark_length;
		if (length < mark_length || memcmp(bytes, CHARSETS[index].mark, mark_length) != 0)
			continue;
		g_autofree char *decoded = g_convert(bytes + mark_length, length - mark_length, "UTF-8",
			CHARSETS[index].charset, NULL, NULL, NULL);
		g_autoptr(GBytes) encoded = decoded ? encode_text(decoded, CHARSETS[index].charset, NULL) : NULL;
		if (!encoded || g_bytes_get_size(encoded) != length || memcmp(g_bytes_get_data(encoded, NULL), bytes, length))
			continue;
		*charset = g_strdup(CHARSETS[index].charset);
		return g_steal_pointer(&decoded);
	}
	return NULL;
}
