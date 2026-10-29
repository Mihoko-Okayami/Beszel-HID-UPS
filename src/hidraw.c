#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <linux/hidraw.h>

#include "hidraw.h"
#include "main.h"

#define MAX_FAILURES 3

static char problem[80];

static int is_ups(const char *node, struct ups *ups) {
	static unsigned char desc[HID_MAX_DESCRIPTOR_SIZE];
	char path[PATH_MAX];
	ssize_t length;
	int fd;
	snprintf(path, sizeof(path), "/sys/class/hidraw/%s/device/report_descriptor", node);
	fd = open(path, O_RDONLY);
	if (fd < 0) return 0;
	length = read(fd, desc, sizeof(desc));
	close(fd);
	return length > 0 && parse(ups, desc, (unsigned int)length);
}

static void read_model(const char *node, char *model, int size) {
	char path[PATH_MAX];
	FILE *file;
	snprintf(path, sizeof(path), "/sys/class/hidraw/%s/device/../../product", node);
	file = fopen(path, "r");
	if (!file || !fgets(model, size, file)) {
		snprintf(model, (size_t)size, "UPS");
	}
	if (file) {
		fclose(file);
	}
	model[strcspn(model, "\n")] = '\0';
}

int open_ups(struct ups *ups) {
	static int told;
	char path[PATH_MAX];
	struct dirent *entry;
	DIR *dir;
	ups->fd = -1;
	dir = opendir("/sys/class/hidraw");
	if (!dir) return 0;
	while (ups->fd < 0 && (entry = readdir(dir))) {
		if (strncmp(entry->d_name, "hidraw", 6) || !is_ups(entry->d_name, ups)) continue;
		snprintf(path, sizeof(path), "/dev/%s", entry->d_name);
		ups->fd = open(path, O_RDONLY | O_NONBLOCK);
		if (ups->fd < 0) {
			if (!told) {
				note("%s: %s", path, strerror(errno));
			}
			told = 1;
			continue;
		}
		told = 0;
		read_model(entry->d_name, ups->model, sizeof(ups->model));
		note("UPS found on %s: %s", entry->d_name, ups->model);
	}
	closedir(dir);
	return ups->fd >= 0;
}

static unsigned int extract(const struct ups *ups, const struct field *field) {
	const unsigned char *data = ups->reports[field->report].data + 1;
	unsigned int i, bit, value = 0;
	for (i = 0; i < field->size; i++) {
		bit = field->offset + i;
		if ((data[bit / 8] >> (bit % 8)) & 1) {
			value |= 1u << i;
		}
	}
	return value;
}

static int get_report(int fd, struct report *report) {
	unsigned int size = report->bytes;
	unsigned char data[MAX_REPORT];
	int length, error;
	for (;;) {
		data[0] = (unsigned char)report->id;
		length = ioctl(fd, HIDIOCGFEATURE(size), data);
		error = length < 0 ? errno : 0;
		if (length >= (int)report->need && data[0] == report->id) {
			if (size != report->bytes) {
				note("report 0x%02x: using %u-byte reads", report->id, size);
			}
			report->bytes = size;
			memcpy(report->data, data, (size_t)length);
			return 1;
		}
		if (error) {
			snprintf(problem, sizeof(problem), "report 0x%02x: %s", report->id, strerror(error));
		} else {
			snprintf(problem, sizeof(problem), "report 0x%02x: unusable answer (length %d, need %u, id 0x%02x)", report->id, length, report->need, data[0]);
		}
		if (error == ENODEV) return -1;
		if (size == MAX_REPORT || (error && error != EOVERFLOW)) return 0;
		size = MAX_REPORT;
	}
}

int has(const struct ups *ups, unsigned int f) {
	return ups->fields[f].size && ups->reports[ups->fields[f].report].valid;
}

int poll_ups(struct ups *ups, unsigned int *values) {
	struct report *report;
	unsigned int i;
	int result, held = 0;
	ups->retry = 0;
	for (i = 0; i < ups->report_count; i++) {
		report = &ups->reports[i];
		result = get_report(ups->fd, report);
		if (result < 0) return LOST;
		if (result && report->failing) {
			note("report 0x%02x: readable again", report->id);
		} else if (!result && !report->failing) {
			note("%s", problem);
		}
		report->valid |= result;
		report->failing = result ? 0 : report->failing + 1;
		if (!result && i == ups->fields[CHARGE].report) {
			ups->retry |= report->valid || report->failing < MAX_FAILURES;
			if (report->valid && report->failing < MAX_FAILURES) return HELD;
			while (++i < ups->report_count) {
				ups->reports[i].valid = ups->reports[i].valid ? 2 : 0;
			}
			return STALE;
		}
	}
	for (i = FULL_CHARGE; i < FIELD_COUNT; i++) {
		report = &ups->reports[ups->fields[i].report];
		if (!ups->fields[i].size || !report->failing) continue;
		ups->retry |= report->failing < MAX_FAILURES || (report->valid && i != FULL_CHARGE);
		held |= !report->valid && report->failing < MAX_FAILURES;
		if ((report->valid == 2 || (report->valid && report->failing >= MAX_FAILURES)) && i != FULL_CHARGE) return STALE;
	}
	if (held) return HELD;
	for (i = 0; i < FIELD_COUNT; i++) {
		values[i] = has(ups, i) ? extract(ups, &ups->fields[i]) : 0;
	}
	return ups->fields[FULL_CHARGE].size && !values[FULL_CHARGE] && values[CHARGE] > 100 ? STALE : FRESH;
}
