#include <string.h>
#include <linux/hid.h>

#include "hidparser.h"

#define MAX_USAGES HID_MAX_DESCRIPTOR_SIZE
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

static void add_fields(struct ups *ups, const struct state *state, const unsigned int *usages, unsigned int usage_count, unsigned int *bits) {
	unsigned int i, f, usage;
	for (i = 0; state->size && i < state->count && bits[state->id] <= MAX_REPORT * 8; i++) {
		usage = usage_count ? usages[i < usage_count ? i : usage_count - 1] : 0;
		for (f = 0; f < FIELD_COUNT; f++) {
			if (usage != field_usages[f] || ups->fields[f].size) continue;
			ups->fields[f].id = state->id;
			ups->fields[f].offset = bits[state->id];
			ups->fields[f].size = state->size;
		}
		bits[state->id] += state->size;
	}
}

static int link_reports(struct ups *ups, const unsigned int *bits) {
	struct field *field;
	unsigned int f, r, bytes, need;
	for (f = 0; f < FIELD_COUNT; f++) {
		field = &ups->fields[f];
		bytes = 1 + (bits[field->id] + 7) / 8;
		if (field->size > 32 || bytes > MAX_REPORT) {
			field->size = 0;
		}
		if (!field->size) continue;
		for (r = 0; r < ups->report_count && ups->reports[r].id != field->id; r++) continue;
		if (r == ups->report_count) {
			ups->reports[ups->report_count++] = (struct report){ .id = field->id, .bytes = bytes };
		}
		need = 1 + (field->offset + field->size + 7) / 8;
		if (ups->reports[r].need < need) {
			ups->reports[r].need = need;
		}
		field->report = r;
	}
	return ups->fields[CHARGE].size != 0;
}

int parse(struct ups *ups, const unsigned char *desc, unsigned int length) {
	struct state state = { 0 }, stack[MAX_DEPTH];
	unsigned int usages[MAX_USAGES], bits[256] = { 0 }, i, k, n, value, depth = 0, usage_count = 0;
	memset(ups->fields, 0, sizeof(ups->fields));
	ups->report_count = 0;
	for (i = 0; i < length; i += n + 1) {
		if (desc[i] == ITEM_LONG) {
			n = i + 1 < length ? desc[i + 1] + 2u : length;
			continue;
		}
		n = desc[i] & 0x03u;
		if (n == 3) {
			n = 4;
		}
		if (i + n >= length) break;
		value = 0;
		for (k = 0; k < n; k++) {
			value |= (unsigned int)desc[i + 1 + k] << (8 * k);
		}
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
			if (depth < MAX_DEPTH) {
				stack[depth++] = state;
			}
			break;
		case ITEM_POP:
			if (depth) {
				state = stack[--depth];
			}
			break;
		case ITEM_USAGE:
			if (usage_count < MAX_USAGES) {
				usages[usage_count++] = n == 4 ? value : state.page << 16 | value;
			}
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
