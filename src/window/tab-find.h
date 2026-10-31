#pragma once

#include "window/tab.h"

/* Which way a search moves from the current match */
/* Which way a search moves from the current match, with same only counting again */
typedef enum { FIND_FIRST, FIND_NEXT, FIND_PREVIOUS, FIND_SAME } FindDirection;

void stop_find_in_tab(Tab *tab);
int find_in_tab(Tab *tab, const char *query, FindDirection direction, int *total);
