#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <linux/hidraw.h>

#define RUN_DIR "/run/beszel-hid-ups"
#define INTERVAL 30
#define SETTLE 1
#define MAX_REPORT 64
#define MAX_USAGES 16
#define MAX_DEPTH 4

enum item {
	ITEM_USAGE_PAGE = 0x04,
	ITEM_USAGE = 0x08,
	ITEM_REPORT_SIZE = 0x74,
	ITEM_INPUT = 0x80,
	ITEM_REPORT_ID = 0x84,
	ITEM_OUTPUT = 0x90,
	ITEM_REPORT_COUNT = 0x94,
	ITEM_COLLECTION = 0xa0,
	ITEM_PUSH = 0xa4,
	ITEM_FEATURE = 0xb0,
	ITEM_POP = 0xb4,
	ITEM_END_COLLECTION = 0xc0,
	ITEM_LONG = 0xfe,
};

enum {
	CHARGE,
	FULL_CHARGE,
	CHARGING,
	DISCHARGING,
	AC_PRESENT,
	FIELD_COUNT,
};

static const unsigned int field_usages[FIELD_COUNT] = {
	[CHARGE] = 0x850066,
	[FULL_CHARGE] = 0x850067,
	[CHARGING] = 0x850044,
	[DISCHARGING] = 0x850045,
	[AC_PRESENT] = 0x8500d0,
};

struct state {
	unsigned int page;
	unsigned int size;
	unsigned int count;
	unsigned int id;
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
	unsigned char data[MAX_REPORT];
};

struct ups {
	int fd;
	unsigned int report_count;
	struct field fields[FIELD_COUNT];
	struct report reports[FIELD_COUNT];
	char model[128];
};

static volatile sig_atomic_t running = 1;

static void stop(int sig)
{
	(void)sig;
	running = 0;
}

static void put(int dir, const char *name, const char *value)
{
	char tmp[32];
	int fd, ok;

	snprintf(tmp, sizeof(tmp), ".%s", name);
	fd = openat(dir, tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0)
		return;
	ok = dprintf(fd, "%s\n", value) > 0;
	if (close(fd) == 0 && ok)
		renameat(dir, tmp, dir, name);
	else
		unlinkat(dir, tmp, 0);
}

static void add_fields(struct ups *ups, const struct state *state, const unsigned int *usages,
	unsigned int usage_count, unsigned int *bits)
{
	unsigned int i, f, usage;

	for (i = 0; state->size && i < state->count && bits[state->id] <= MAX_REPORT * 8; i++) {
		usage = usage_count ? usages[i < usage_count ? i : usage_count - 1] : 0;
		for (f = 0; f < FIELD_COUNT; f++) {
			if (usage != field_usages[f] || ups->fields[f].size)
				continue;
			ups->fields[f].id = state->id;
			ups->fields[f].offset = bits[state->id];
			ups->fields[f].size = state->size;
		}
		bits[state->id] += state->size;
	}
}

static int link_reports(struct ups *ups, const unsigned int *bits)
{
	struct field *field;
	unsigned int f, r, bytes;

	for (f = 0; f < FIELD_COUNT; f++) {
		field = &ups->fields[f];
		bytes = 1 + (bits[field->id] + 7) / 8;
		if (field->size > 32 || bytes > MAX_REPORT)
			field->size = 0;
		if (!field->size)
			continue;
		r = 0;
		while (r < ups->report_count && ups->reports[r].id != field->id)
			r++;
		if (r == ups->report_count)
			ups->report_count++;
		ups->reports[r].id = field->id;
		ups->reports[r].bytes = bytes;
		field->report = r;
	}
	return ups->fields[CHARGE].size != 0;
}

static int parse(struct ups *ups, const unsigned char *desc, unsigned int length)
{
	struct state state = { 0 }, stack[MAX_DEPTH];
	unsigned int usages[MAX_USAGES], bits[256] = { 0 };
	unsigned int i, k, n, value, depth = 0, usage_count = 0;

	memset(ups->fields, 0, sizeof(ups->fields));
	ups->report_count = 0;
	for (i = 0; i < length; i += n + 1) {
		if (desc[i] == ITEM_LONG) {
			n = i + 1 < length ? desc[i + 1] + 2u : length;
			continue;
		}
		n = desc[i] & 0x03u;
		if (n == 3)
			n = 4;
		if (i + n >= length)
			break;
		value = 0;
		for (k = 0; k < n; k++)
			value |= (unsigned int)desc[i + 1 + k] << (8 * k);
		switch (desc[i] & 0xfc) {
		case ITEM_USAGE_PAGE:
			state.page = value;
			break;
		case ITEM_REPORT_SIZE:
			state.size = value;
			break;
		case ITEM_REPORT_ID:
			state.id = value & 0xff;
			break;
		case ITEM_REPORT_COUNT:
			state.count = value;
			break;
		case ITEM_PUSH:
			if (depth < MAX_DEPTH)
				stack[depth++] = state;
			break;
		case ITEM_POP:
			if (depth)
				state = stack[--depth];
			break;
		case ITEM_USAGE:
			if (usage_count < MAX_USAGES)
				usages[usage_count++] = n == 4 ? value : state.page << 16 | value;
			break;
		case ITEM_FEATURE:
			add_fields(ups, &state, usages, usage_count, bits);
			usage_count = 0;
			break;
		case ITEM_INPUT:
		case ITEM_OUTPUT:
		case ITEM_COLLECTION:
		case ITEM_END_COLLECTION:
			usage_count = 0;
			break;
		}
	}
	return link_reports(ups, bits);
}

static int is_ups(int fd, struct ups *ups)
{
	static struct hidraw_report_descriptor desc;

	if (ioctl(fd, HIDIOCGRDESCSIZE, &desc.size) < 0)
		return 0;
	if (ioctl(fd, HIDIOCGRDESC, &desc) < 0)
		return 0;
	return parse(ups, desc.value, desc.size);
}

static void read_model(const char *node, char *model, int size)
{
	char path[PATH_MAX];
	FILE *file;

	snprintf(path, sizeof(path), "/sys/class/hidraw/%s/device/../../product", node);
	file = fopen(path, "r");
	if (!file || !fgets(model, size, file))
		snprintf(model, (size_t)size, "UPS");
	if (file)
		fclose(file);
	model[strcspn(model, "\n")] = '\0';
}

static int open_ups(struct ups *ups)
{
	char path[PATH_MAX];
	struct dirent *entry;
	DIR *dir;
	int fd;

	ups->fd = -1;
	dir = opendir("/sys/class/hidraw");
	if (!dir)
		return 0;
	while (ups->fd < 0 && (entry = readdir(dir))) {
		if (strncmp(entry->d_name, "hidraw", 6))
			continue;
		snprintf(path, sizeof(path), "/dev/%s", entry->d_name);
		fd = open(path, O_RDONLY | O_NONBLOCK);
		if (fd < 0)
			continue;
		if (is_ups(fd, ups)) {
			ups->fd = fd;
			read_model(entry->d_name, ups->model, sizeof(ups->model));
		} else {
			close(fd);
		}
	}
	closedir(dir);
	return ups->fd >= 0;
}

static unsigned int extract(const struct ups *ups, const struct field *field)
{
	const unsigned char *data = ups->reports[field->report].data + 1;
	unsigned int i, bit, value = 0;

	for (i = 0; i < field->size; i++) {
		bit = field->offset + i;
		if ((data[bit / 8] >> (bit % 8)) & 1)
			value |= 1u << i;
	}
	return value;
}

static int get_report(int fd, struct report *report)
{
	int length = (int)report->bytes;

	report->data[0] = (unsigned char)report->id;
	if (ioctl(fd, HIDIOCGFEATURE(report->bytes), report->data) != length)
		return 0;
	return report->data[0] == report->id;
}

static int poll_ups(struct ups *ups, unsigned int *values)
{
	unsigned int i;

	for (i = 0; i < ups->report_count; i++)
		if (!get_report(ups->fd, &ups->reports[i]))
			return 0;
	for (i = 0; i < FIELD_COUNT; i++)
		values[i] = ups->fields[i].size ? extract(ups, &ups->fields[i]) : 0;
	return 1;
}

static int charge_level(const unsigned int *values)
{
	unsigned long long level = values[CHARGE];

	if (values[FULL_CHARGE])
		level = level * 100 / values[FULL_CHARGE];
	return level < 100 ? (int)level : 100;
}

static const char *status_name(const struct ups *ups, const unsigned int *values, int level)
{
	if (values[DISCHARGING])
		return "Discharging";
	if (values[CHARGING])
		return "Charging";
	if (!ups->fields[AC_PRESENT].size)
		return "Unknown";
	if (!values[AC_PRESENT])
		return "Discharging";
	return level == 100 ? "Full" : "Not charging";
}

static void wait_change(int fd)
{
	struct pollfd event = { .fd = fd, .events = POLLIN };
	unsigned char report[MAX_REPORT];

	if (fd < 0) {
		sleep(INTERVAL);
		return;
	}
	if (poll(&event, 1, INTERVAL * 1000) <= 0)
		return;
	sleep(SETTLE);
	while (read(fd, report, sizeof(report)) > 0)
		continue;
}

int main(int argc, char **argv)
{
	struct sigaction action = { .sa_handler = stop };
	const char *path = argc > 1 ? argv[1] : RUN_DIR;
	const char *status, *last_status = NULL;
	unsigned int values[FIELD_COUNT];
	struct ups ups = { .fd = -1 };
	int base, out = -1, level, last_level = -1;
	char text[8];

	mkdir(path, 0755);
	base = open(path, O_RDONLY | O_DIRECTORY);
	if (base >= 0) {
		mkdirat(base, "ups", 0755);
		out = openat(base, "ups", O_RDONLY | O_DIRECTORY);
		close(base);
	}
	if (out < 0) {
		perror(path);
		return 1;
	}
	sigaction(SIGINT, &action, NULL);
	sigaction(SIGTERM, &action, NULL);
	put(out, "type", "Battery");

	while (running) {
		if (ups.fd < 0 && open_ups(&ups))
			put(out, "model_name", ups.model);
		if (ups.fd >= 0 && poll_ups(&ups, values)) {
			level = charge_level(values);
			status = status_name(&ups, values, level);
			if (status != last_status) {
				put(out, "status", status);
				last_status = status;
			}
			if (level != last_level) {
				snprintf(text, sizeof(text), "%d", level);
				put(out, "capacity", text);
				last_level = level;
			}
		} else {
			if (ups.fd >= 0)
				close(ups.fd);
			ups.fd = -1;
			last_status = NULL;
			last_level = -1;
			unlinkat(out, "capacity", 0);
		}
		wait_change(ups.fd);
	}
	unlinkat(out, "capacity", 0);
	return 0;
}
