#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

void gps_parser_init(void);
bool gps_parser_get_fix(double *latitude, double *longitude, char *utc_time, size_t utc_time_len);
bool gps_parser_has_fix(void);
