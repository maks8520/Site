#pragma once

typedef void (*button_callback_t)(void);

void buttons_init(button_callback_t button1_callback, button_callback_t button2_callback);
