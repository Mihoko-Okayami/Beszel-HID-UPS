#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "hidraw.h"
#include "main.h"

#define RUN_DIR "/run/beszel-hid-ups"
#define INTERVAL 30
#define RETRY 2
#define SETTLE 1
#define QUICK_RESCANS 6

static volatile sig_atomic_t running = 1;
static sigset_t unblocked;
static int onlinedischarge_onbattery;

static void stop(int sig) {
	(void)sig;
	running = 0;
}

void note(const char *format, ...) {
	char stamp[32] = "", message[256];
	time_t now = time(NULL);
	struct tm *local = localtime(&now);
	va_list args;
	if (local && !getenv("JOURNAL_STREAM")) {
		strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S ", local);
	}
	va_start(args, format);
	vsnprintf(message, sizeof(message), format, args);
	va_end(args);
	fprintf(stderr, "%s%s\n", stamp, message);
}

static int put(int dir, const char *name, const char *value) {
	static int failing;
	char tmp[32];
	int fd, ok = 0;
	snprintf(tmp, sizeof(tmp), ".%s", name);
	unlinkat(dir, tmp, 0);
	fd = openat(dir, tmp, O_WRONLY | O_CREAT | O_EXCL, 0644);
	if (fd >= 0) {
		ok = dprintf(fd, "%s\n", value) > 0;
		ok = close(fd) == 0 && ok && renameat(dir, tmp, dir, name) == 0;
	}
	if (ok) {
		failing = 0;
		return 1;
	}
	if (!failing) {
		note("cannot write %s: %s", name, strerror(errno));
	}
	failing = 1;
	if (fd >= 0) {
		unlinkat(dir, tmp, 0);
	}
	return 0;
}

static int charge_level(const unsigned int *values) {
	unsigned long long level = values[CHARGE];
	if (values[FULL_CHARGE]) {
		level = level * 100 / values[FULL_CHARGE];
	}
	return level < 100 ? (int)level : 100;
}

static const char *status_name(const struct ups *ups, const unsigned int *values, int level) {
	if (onlinedischarge_onbattery && values[DISCHARGING]) return "Discharging";
	if (values[AC_PRESENT] && level == 100) return "Full";
	if (values[DISCHARGING] || (has(ups, AC_PRESENT) && !values[AC_PRESENT])) return "Discharging";
	if (values[CHARGING]) return "Charging";
	return has(ups, AC_PRESENT) ? "Not charging" : "Unknown";
}

static void wait_change(int fd, unsigned int seconds) {
	struct pollfd event = { .fd = fd, .events = POLLIN };
	struct timespec timeout = { .tv_sec = seconds }, settle = { .tv_sec = SETTLE };
	unsigned char report[MAX_REPORT];
	if (ppoll(&event, 1, &timeout, &unblocked) <= 0) return;
	ppoll(NULL, 0, &settle, &unblocked);
	while (read(fd, report, sizeof(report)) > 0) continue;
}

int main(int argc, char **argv) {
	static const int signals[] = { SIGHUP, SIGINT, SIGTERM };
	struct sigaction action = { .sa_handler = stop };
	const char *path = NULL, *status, *last_status = NULL;
	int base, out = -1, level, last_level = -1, result, quick = 0, battery, last_battery = -1, first = 1, hinted = 0, arg;
	unsigned int values[FIELD_COUNT], i;
	struct ups ups = { .fd = -1 };
	sigset_t blocked;
	char text[8];
	for (arg = 1; arg < argc; arg++) {
		if (argv[arg][0] && argv[arg][0] != '-' && !path) {
			path = argv[arg];
		} else if (!strcmp(argv[arg], "-x") && arg + 1 < argc && !strcmp(argv[arg + 1], "onlinedischarge_onbattery")) {
			onlinedischarge_onbattery = 1;
			arg++;
		} else {
			note("usage: %s [-x onlinedischarge_onbattery] [directory]", argv[0]);
			return 1;
		}
	}
	if (!path) {
		path = RUN_DIR;
	}
	if ((mkdir(path, 0755) && errno != EEXIST) || (base = open(path, O_RDONLY | O_DIRECTORY)) < 0) {
		note("%s: %s", path, strerror(errno));
		return 1;
	}
	if ((mkdirat(base, "ups", 0755) && errno != EEXIST) || (out = openat(base, "ups", O_RDONLY | O_DIRECTORY | O_NOFOLLOW)) < 0) {
		note("%s/ups: %s", path, strerror(errno));
		return 1;
	}
	close(base);
	unlinkat(out, "capacity", 0);
	sigemptyset(&blocked);
	for (i = 0; i < sizeof(signals) / sizeof(signals[0]); i++) {
		sigaction(signals[i], &action, NULL);
		sigaddset(&blocked, signals[i]);
	}
	sigprocmask(SIG_BLOCK, &blocked, &unblocked);
	while (running) {
		if (ups.fd < 0 && !open_ups(&ups) && first) {
			note("no UPS found, retrying every %d s", INTERVAL);
		}
		first = 0;
		result = ups.fd >= 0 ? poll_ups(&ups, values) : LOST;
		if (result == FRESH) {
			battery = has(&ups, AC_PRESENT) ? !values[AC_PRESENT] || (onlinedischarge_onbattery && values[DISCHARGING]) : values[DISCHARGING] ? 1 : -1;
			if (battery != last_battery) {
				if (battery >= 0 || last_battery > 0) {
					note(battery > 0 ? "on battery" : "on mains");
				}
				last_battery = battery;
			}
			level = charge_level(values);
			if (!hinted && !onlinedischarge_onbattery && values[AC_PRESENT] && values[DISCHARGING] && level < 100) {
				note("on mains but discharging: if the UPS is in fact on battery, use -x onlinedischarge_onbattery");
				hinted = 1;
			}
			status = status_name(&ups, values, level);
			snprintf(text, sizeof(text), "%d", level);
			if (status != last_status && put(out, "type", "Battery") && put(out, "model_name", ups.model) && put(out, "status", status)) {
				last_status = status;
			}
			last_level = status == last_status && ((level == last_level && !faccessat(out, "capacity", F_OK, 0)) || put(out, "capacity", text)) ? level : -1;
			if (last_level < 0) {
				unlinkat(out, "capacity", 0);
			}
		} else if (result != HELD) {
			if (result == LOST && ups.fd >= 0) {
				note("UPS lost: unplugged");
				quick = QUICK_RESCANS;
				close(ups.fd);
				ups.fd = -1;
			} else if (result == STALE && last_status) {
				note("no usable data from the UPS, battery hidden");
			}
			last_status = NULL;
			last_level = -1;
			last_battery = -1;
			unlinkat(out, "capacity", 0);
		}
		if (ups.fd < 0 && quick > 0) {
			quick--;
			wait_change(ups.fd, SETTLE);
		} else if (ups.fd >= 0 && ups.retry) {
			wait_change(ups.fd, RETRY);
		} else {
			wait_change(ups.fd, INTERVAL);
		}
	}
	unlinkat(out, "capacity", 0);
	note("stopped");
	return 0;
}
