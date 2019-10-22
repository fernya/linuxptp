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
#include <stdint.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/queue.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "clockadj.h"
#include "missing.h"
#include "print.h"
#include "servo.h"
#include "ts2phc_master.h"
#include "ts2phc_slave.h"
#include "util.h"

#define NS_PER_SEC		1000000000LL
#define SAMPLE_WEIGHT		1.0

struct ts2phc_slave {
	char *name;
	STAILQ_ENTRY(ts2phc_slave) list;
	struct ptp_pin_desc pin_desc;
	enum servo_state state;
	unsigned int polarity;
	uint32_t ignore_lower;
	uint32_t ignore_upper;
	struct servo *servo;
	clockid_t clk;
	int fd;
};

struct ts2phc_slave_array {
	struct ts2phc_slave **slave;
	struct pollfd *pfd;
} polling_array;

enum extts_result {
	EXTTS_ERROR	= -1,
	EXTTS_OK	= 0,
	EXTTS_IGNORE	= 1,
};

static enum extts_result ts2phc_slave_read_extts(struct ts2phc_slave *slave,
						 struct ts2phc_master *master,
						 int64_t *offset,
						 uint64_t *local_ts);

static STAILQ_HEAD(slave_ifaces_head, ts2phc_slave) ts2phc_slaves =
	STAILQ_HEAD_INITIALIZER(ts2phc_slaves);

static unsigned int ts2phc_n_slaves;

static int ts2phc_slave_array_create(void)
{
	struct ts2phc_slave *slave;
	unsigned int i;

	if (polling_array.slave) {
		return 0;
	}
	polling_array.slave = malloc(ts2phc_n_slaves * sizeof(*polling_array.slave));
	if (!polling_array.slave) {
		pr_err("low memory");
		return -1;
	}
	polling_array.pfd = malloc(ts2phc_n_slaves * sizeof(*polling_array.pfd));
	if (!polling_array.pfd) {
		pr_err("low memory");
		free(polling_array.slave);
		polling_array.slave = NULL;
		return -1;
	}
	i = 0;
	STAILQ_FOREACH(slave, &ts2phc_slaves, list) {
		polling_array.slave[i] = slave;
		i++;
	}
	for (i = 0; i < ts2phc_n_slaves; i++) {
		polling_array.pfd[i].events = POLLIN | POLLPRI;
		polling_array.pfd[i].fd = polling_array.slave[i]->fd;
	}
	return 0;
}

static void ts2phc_slave_array_destroy(void)
{
	free(polling_array.slave);
	free(polling_array.pfd);
	polling_array.slave = NULL;
	polling_array.pfd = NULL;
}

static struct ts2phc_slave *ts2phc_slave_create(struct config *cfg, const char *device)
{
	int err, fadj, junk, pulsewidth;
	struct ptp_extts_request extts;
	struct ts2phc_slave *slave;

	slave = calloc(1, sizeof(*slave));
	if (!slave) {
		pr_err("low memory");
		return NULL;
	}
	slave->name = strdup(device);
	if (!slave->name) {
		pr_err("low memory");
		free(slave);
		return NULL;
	}
	slave->pin_desc.index = config_get_int(cfg, device, "ts2phc.pin_index");
	slave->pin_desc.func = PTP_PF_EXTTS;
	slave->pin_desc.chan = config_get_int(cfg, device, "ts2phc.extts_index");
	slave->polarity = config_get_int(cfg, device, "ts2phc.extts_polarity");

	pulsewidth = config_get_int(cfg, device, "ts2phc.pulsewidth");
	pulsewidth /= 2;
	slave->ignore_upper = 1000000000 - pulsewidth;
	slave->ignore_lower = pulsewidth;

	slave->clk = posix_clock_open(device, &junk);
	if (slave->clk == CLOCK_INVALID) {
		pr_err("failed to open clock");
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
		pr_err("failed to create servo");
		goto no_servo;
	}
	servo_sync_interval(slave->servo, 1.0);

	// TODO - only set if ioctl is supported by device
	err = ioctl(slave->fd, PTP_PIN_SETFUNC, &slave->pin_desc);
	if (err < 0) {
		pr_err("PTP_PIN_SETFUNC request failed: %m");
		goto no_pin_func;
	}

	// TODO - disable extts first, then read out fifo, then enable
	memset(&extts, 0, sizeof(extts));
	extts.index = slave->pin_desc.chan;
	extts.flags = slave->polarity | PTP_ENABLE_FEATURE;
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

static void ts2phc_slave_destroy(struct ts2phc_slave *slave)
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

static int ts2phc_slave_event(struct ts2phc_slave *slave,
			      struct ts2phc_master *master)
{
	enum extts_result result;
	uint64_t extts_ts;
	int64_t offset;
	double adj;

	result = ts2phc_slave_read_extts(slave, master, &offset, &extts_ts);
	switch (result) {
	case EXTTS_ERROR:
		return -1;
	case EXTTS_OK:
		break;
	case EXTTS_IGNORE:
		return 0;
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

static enum extts_result ts2phc_slave_read_extts(struct ts2phc_slave *slave,
						 struct ts2phc_master *master,
						 int64_t *offset,
						 uint64_t *local_ts)
{
	struct ptp_extts_event event;
	uint64_t event_ns, source_ns;
	struct timespec source_ts;
	int cnt;

	cnt = read(slave->fd, &event, sizeof(event));
	if (cnt != sizeof(event)) {
		pr_err("read extts event failed: %m");
		return EXTTS_ERROR;
	}
	if (event.index != slave->pin_desc.chan) {
		pr_err("extts on unexpected channel");
		return EXTTS_ERROR;
	}
	source_ts = ts2phc_master_getppstime(master);
	source_ns = source_ts.tv_sec * NS_PER_SEC + source_ts.tv_nsec;

	event_ns = event.t.sec * NS_PER_SEC;
	event_ns += event.t.nsec;

	*offset = event_ns - source_ns;
	*local_ts = event_ns;

	pr_debug("%s extts index %u at %lld.%09u src %" PRIi64 ".%ld diff %" PRId64,
		 slave->name, event.index, event.t.sec, event.t.nsec,
		 (int64_t) source_ts.tv_sec, source_ts.tv_nsec, *offset);

	if (slave->polarity == (PTP_RISING_EDGE | PTP_FALLING_EDGE) &&
	    event.t.nsec > slave->ignore_lower &&
	    event.t.nsec < slave->ignore_upper) {
		return EXTTS_IGNORE;
	}
	return EXTTS_OK;
}

/* public methods */

int ts2phc_slave_add(struct config *cfg, const char *name)
{
	struct ts2phc_slave *slave;

	/* Create each interface only once. */
	STAILQ_FOREACH(slave, &ts2phc_slaves, list) {
		if (0 == strcmp(name, slave->name)) {
			return 0;
		}
	}
	slave = ts2phc_slave_create(cfg, name);
	if (!slave) {
		pr_err("failed to create slave");
		return -1;
	}
	STAILQ_INSERT_TAIL(&ts2phc_slaves, slave, list);
	ts2phc_n_slaves++;

	return 0;
}

void ts2phc_slave_cleanup(void)
{
	struct ts2phc_slave *slave;

	ts2phc_slave_array_destroy();

	while ((slave = STAILQ_FIRST(&ts2phc_slaves))) {
		STAILQ_REMOVE_HEAD(&ts2phc_slaves, list);
		ts2phc_slave_destroy(slave);
		ts2phc_n_slaves--;
	}
}

int ts2phc_slave_poll(struct ts2phc_master *master)
{
	unsigned int i;
	int cnt;

	if (ts2phc_slave_array_create()) {
		return -1;
	}
	cnt = poll(polling_array.pfd, ts2phc_n_slaves, 2000);
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
	for (i = 0; i < ts2phc_n_slaves; i++) {
		if (polling_array.pfd[i].revents & (POLLIN|POLLPRI)) {
			ts2phc_slave_event(polling_array.slave[i], master);
		}
	}
	return 0;
}
