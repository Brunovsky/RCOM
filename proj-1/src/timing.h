#ifndef TIMING_H___
#define TIMING_H___

#include <stddef.h>

size_t number_of_packets(size_t filesize);

double average_packetsize(size_t filesize);

void print_stats(size_t i, size_t filesize);

void begin_timing(size_t i);

void end_timing(size_t i);

#endif // TIMING_H___
