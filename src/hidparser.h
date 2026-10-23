#ifndef HIDPARSER_H
#define HIDPARSER_H

#define MAX_REPORT 64

enum {
	CHARGE,
	FULL_CHARGE,
	CHARGING,
	DISCHARGING,
	AC_PRESENT,
	FIELD_COUNT,
};

struct field {
	unsigned int id;
	unsigned int offset;
	unsigned int size;
	unsigned int report;
};

struct report {
	unsigned int id;
	unsigned int bytes;
	unsigned int need;
	int valid;
	int failing;
	unsigned char data[MAX_REPORT];
};

struct ups {
	int fd;
	unsigned int report_count;
	int retry;
	struct field fields[FIELD_COUNT];
	struct report reports[FIELD_COUNT];
	char model[128];
};

int parse(struct ups *ups, const unsigned char *desc, unsigned int length);

#endif
