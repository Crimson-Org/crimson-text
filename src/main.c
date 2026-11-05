/* Start the Crimson Text viewer and editor */

#include <glib.h>

#include "app/application.h"

/* The desktop files set this, since GTK's printing library starts GnuTLS as it loads, which takes part of the
 * startup, while nothing here needs it then */
#define GNUTLS_SKIP_VARIABLE "GNUTLS_NO_IMPLICIT_INIT"

int main(int argument_count, char **arguments) {
	/* Programs opened from here, such as a browser for a link, start GnuTLS as usual */
	g_unsetenv(GNUTLS_SKIP_VARIABLE);
	return run_application(&APPLICATION_SETUP, argument_count, arguments);
}
