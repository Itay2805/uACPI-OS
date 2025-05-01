#pragma once
#include <stdbool.h>

char* cmdline_get_option_value(const char* cmdline, const char* key);
bool cmdline_has_flag(const char* cmdline, const char* key);
