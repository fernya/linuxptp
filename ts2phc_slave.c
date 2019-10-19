/**
 * @file ts2phc_slave.c
 * @brief Utility program to synchronize the PHC clock to external events
 * @note Copyright (C) 2019 Balint Ferencz <fernya@sch.bme.hu>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */
#include <errno.h>
#include <linux/ptp_clock.h>
#include <poll.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "clockadj.h"
#include "missing.h"
#include "print.h"
#include "servo.h"
#include "ts2phc_slave.h"
#include "util.h"

#define MAX_TS2PHC_SLAVES	1
#define NS_PER_SEC		1000000000LL
#define SAMPLE_WEIGHT		1.0

struct ts2phc_slave {
	char *name;
	struct ts2phc_master *master;
	struct ptp_pin_desc pin_desc;
	enum servo_state state;
	struct servo *servo;
	clockid_t clk;
	int fd;
};

static int read_extts(struct ts2phc_slave *slave, int64_t *offset,
		      uint64_t *local_ts)
{
	struct ptp_extts_event event;
	uint64_t event_ns, source_ns;
	struct timespec source_ts;
	int cnt;

	cnt = read(slave->fd, &event, sizeof(event));
	if (cnt != sizeof(event)) {
		pr_err("read extts event failed: %m");
		return -1;
	}
	if (event.index != slave->pin_desc.chan) {
		pr_err("extts on unexpected channel");
		return -1;
	}
	source_ts = ts2phc_master_getppstime(slave->master);
	source_ns = source_ts.tv_sec * NS_PER_SEC + source_ts.tv_nsec;

	event_ns = event.t.sec * NS_PER_SEC;
	event_ns += event.t.nsec;

	*offset = event_ns - source_ns;
	*local_ts = event_ns;

	pr_debug("%s extts index %u at %lld.%09u src %" PRIi64 ".%ld diff %" PRId64,
		 slave->name, event.index, event.t.sec, event.t.nsec,
		 (int64_t) source_ts.tv_sec, source_ts.tv_nsec, *offset);

	return 0;
}

static int ts2phc_slave_event(struct ts2phc_slave *slave)
{
	uint64_t extts_ts;
	int64_t offset;
	double adj;

	if (read_extts(slave, &offset, &extts_ts)) {
		return -1;
	}
	adj = servo_sample(slave->servo, offset, extts_ts,
			   SAMPLE_WEIGHT, &slave->state);

	pr_info("%s master offset %10" PRId64 " s%d freq %+7.0f",
		slave->name, offset, slave->state, adj);

	switch (slave->state) {
	case SERVO_UNLOCKED:
		break;
	case SERVO_JUMP:
		clockadj_set_freq(slave->clk, -adj);
		clockadj_step(slave->clk, -offset);
		break;
	case SERVO_LOCKED:
	case SERVO_LOCKED_STABLE:
		clockadj_set_freq(slave->clk, -adj);
		break;
	}
	return 0;
}

/* public methods */

struct ts2phc_slave *ts2phc_slave_create(struct config *cfg, char *device,
					 struct ts2phc_master *master,
					 int extts_index)
{
	struct ptp_extts_request extts;
	struct ts2phc_slave *slave;
	int err, fadj, junk;

	slave = calloc(1, sizeof(*slave));
	if (!slave) {
		return NULL;
	}
	slave->name = strdup(device);
	if (!slave->name) {
		return NULL;
	}
	slave->master = master;
	slave->pin_desc.index = 0;
	slave->pin_desc.func = PTP_PF_EXTTS;
	slave->pin_desc.chan = extts_index;
	slave->clk = posix_clock_open(device, &junk);
	if (slave->clk == CLOCK_INVALID) {
		goto no_posix_clock;
	}
	slave->fd = CLOCKID_TO_FD(slave->clk);

	pr_debug("PHC slave %s has ptp index %d", device, junk);

	fadj = (int) clockadj_get_freq(slave->clk);
	/* Due to a bug in older kernels, the reading may silently fail
	   and return 0. Set the frequency back to make sure fadj is
	   the actual frequency of the clock. */
	clockadj_set_freq(slave->clk, fadj);

	slave->servo = servo_create(cfg, CLOCK_SERVO_PI, -fadj, 100000, 0);
	if (!slave->servo) {
		goto no_servo;
	}
	servo_sync_interval(slave->servo, 1.0);
	err = ioctl(slave->fd, PTP_PIN_SETFUNC, &slave->pin_desc);
	if (err < 0) {
		pr_err("PTP_PIN_SETFUNC request failed: %m");
		goto no_pin_func;
	}
	memset(&extts, 0, sizeof(extts));
	extts.index = slave->pin_desc.chan;
	extts.flags = PTP_RISING_EDGE | PTP_ENABLE_FEATURE;
	err = ioctl(slave->fd, PTP_EXTTS_REQUEST, &extts);
	if (err < 0) {
		pr_err("PTP_EXTTS_REQUEST failed: %m");
		goto no_ext_ts;
	}
	return slave;
no_ext_ts:
no_pin_func:
	servo_destroy(slave->servo);
no_servo:
	close(slave->fd);
no_posix_clock:
	free(slave->name);
	free(slave);
	return NULL;
}

void ts2phc_slave_destroy(struct ts2phc_slave *slave)
{
	struct ptp_extts_request extts;

	memset(&extts, 0, sizeof(extts));
	extts.index = slave->pin_desc.chan;
	extts.flags = 0;
	if (ioctl(slave->fd, PTP_EXTTS_REQUEST, &extts)) {
		pr_err("PTP_EXTTS_REQUEST failed: %m");
	}
	posix_clock_close(slave->clk);
	free(slave->name);
	free(slave);
}

int ts2phc_slave_poll(struct ts2phc_slave *slaves, unsigned int n_slaves)
{
	struct pollfd pollfd[MAX_TS2PHC_SLAVES];
	unsigned int i;
	int cnt;

	if (n_slaves > MAX_TS2PHC_SLAVES) {
		return -1;
	}
	for (i = 0; i < n_slaves; i++) {
		pollfd[i].events = POLLIN | POLLPRI;
		pollfd[i].fd = slaves[i].fd;
	}
	cnt = poll(pollfd, n_slaves, 1000);
	if (cnt < 0) {
		if (EINTR == errno) {
			return 0;
		} else {
			pr_emerg("poll failed");
			return -1;
		}
	} else if (!cnt) {
		pr_debug("poll returns zero, no events");
		return 0;
	}
	for (i = 0; i < n_slaves; i++) {
		if (pollfd[i].revents & (POLLIN|POLLPRI)) {
			ts2phc_slave_event(&slaves[i]);
		}
	}
	return 0;
}
