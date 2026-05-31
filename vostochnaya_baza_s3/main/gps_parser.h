#pragma once
#include <stdbool.h>

void gps_parser_init(void);

extern double gps_lat;
extern double gps_lon;
extern bool gps_has_fix;
