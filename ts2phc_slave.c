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

struct ts2phc_slave {
	struct ptp_pin_desc pin_desc;
	enum servo_state state;
	struct servo *servo;
	clockid_t clk;
	int fd;
};

/*
 * Returns the time on the PPS source device at which the most recent
 * PPS event was generated.
 *
 * This implementation assumes that the system time is approximately
 * correct, and it simply drops the nanoseconds field past the full
 * second.
 *
 * TODO: convert this into a proper interface that depends on the PPS
 * source device.
 */
static uint64_t pps_source_gettime(void)
{
	struct timespec now;
	uint64_t result;

	clock_gettime(CLOCK_TAI, &now);
	result = now.tv_sec * NS_PER_SEC;

	return result;
}

static int read_extts(struct ts2phc_slave *slave, int64_t *offset,
		      uint64_t *local_ts)
{
	uint64_t event_tns, pps_source_time;
	struct ptp_extts_event event;
	int cnt;

	cnt = read(slave->fd, &event, sizeof(event));
	if (cnt != sizeof(event)) {
		pr_err("read extts event failed: %m");
		return -1;
	}
	if (event.index != slave->pin_desc.chan) {
		return -1;
	}
	pps_source_time = pps_source_gettime();
	event_tns = event.t.sec * NS_PER_SEC;
	event_tns += event.t.nsec;
	*offset = event_tns - pps_source_time;
	*local_ts = event_tns;

	return 0;
}

static void ts2phc_slave_event(struct ts2phc_slave *slave)
{
	uint64_t extts_ts;
	int64_t offset;
	double adj;

	if (read_extts(slave, &offset, &extts_ts)) {
		return;
	}
	adj = servo_sample(slave->servo, offset, extts_ts, 0.0, &slave->state);
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
}

/* public methods */

struct ts2phc_slave *ts2phc_slave_create(struct config *cfg, char *device,
					 int extts_index)
{
	struct ptp_extts_request extts;
	struct ts2phc_slave *slave;
	int err, junk;

	slave = calloc(1, sizeof(*slave));
	if (!slave) {
		return NULL;
	}
	slave->pin_desc.index = 0;
	slave->pin_desc.func = PTP_PF_EXTTS;
	slave->pin_desc.chan = extts_index;
	slave->clk = posix_clock_open(device, &junk);
	if (slave->clk == CLOCK_INVALID) {
		goto no_posix_clock;
	}
	slave->fd = CLOCKID_TO_FD(slave->clk);
	slave->servo = servo_create(cfg, CLOCK_SERVO_PI, 0, 100000, 0);
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
		return 0;
	}
	for (i = 0; i < n_slaves; i++) {
		if (pollfd[i].revents & (POLLIN|POLLPRI)) {
			ts2phc_slave_event(&slaves[i]);
		}
	}
	return 0;
}
