#pragma once

#include "window/tab.h"

void start_code_editor(Tab *tab, const char *language);
void add_code_block(Tab *tab, const Block *block);
void append_code_text(Tab *tab, const char *text);
void finish_code_loading(Tab *tab);
