#ifndef LL_SETUP_H___
#define LL_SETUP_H___

#include <stdbool.h>

bool is_valid_baudrate(int baudrate);

int setup_link_layer(const char* name);

int reset_link_layer(int fd);

#endif // LL_SETUP_H___
