#pragma once

#include <stdbool.h>

void web_server_init(void);
bool web_server_send_text_async(const char *text);
