/*
 * Counts.h
 *
 * Header file for managing part counts in bins.
 * Provides functions to get the latest part count and low-stock threshold for each bin.
 */

#ifndef COUNTS_H
#define COUNTS_H

#include <stdbool.h>

#ifndef NUM_BINS
#define NUM_BINS 5
#endif

/* Latest part count for a bin (0..NUM_BINS-1). */
int counts_get(int bin);

/* Low-stock threshold for a bin, in parts. */
int counts_low_level(int bin);

#endif /* COUNTS_H */