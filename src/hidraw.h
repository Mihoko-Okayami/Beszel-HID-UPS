#ifndef HIDRAW_H
#define HIDRAW_H

#include "hidparser.h"

enum reading { LOST, STALE, HELD, FRESH };

int open_ups(struct ups *ups);
int has(const struct ups *ups, unsigned int f);
int poll_ups(struct ups *ups, unsigned int *values);

#endif
