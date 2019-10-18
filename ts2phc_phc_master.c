/**
 * @file ts2phc_phc_master.c
 * @note Copyright (C) 2019 Richard Cochran <richardcochran@gmail.com>
 * @note SPDX-License-Identifier: GPL-2.0+
 */
#include <linux/ptp_clock.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>

#include "print.h"
#include "missing.h"
#include "ts2phc_master_private.h"
#include "ts2phc_phc_master.h"
#include "util.h"

#define PIN_INDEX	0
#define PIN_FUNC	PTP_PF_PEROUT
#define INDEX		0
#define ONE_SECOND	2 // hack, the i210 slave time stamps both edges!

struct ts2phc_phc_master {
	struct ts2phc_master master;
	clockid_t clkid;
	int fd;
};

static int ts2phc_phc_master_activate(struct ts2phc_phc_master *master)
{
	struct ptp_perout_request perout_request;
	struct ptp_pin_desc desc;
	struct timespec ts;

	memset(&desc, 0, sizeof(desc));
	desc.index = PIN_INDEX;
	desc.func = PIN_FUNC;
	desc.chan = INDEX;

	if (ioctl(master->fd, PTP_PIN_SETFUNC, &desc)) {
		pr_err("PTP_PIN_SETFUNC failed: %m");
		return -1;
	}

	if (clock_gettime(master->clkid, &ts)) {
		perror("clock_gettime");
		return -1;
	}
	memset(&perout_request, 0, sizeof(perout_request));
	perout_request.index = INDEX;
	perout_request.start.sec = ts.tv_sec + 2;
	perout_request.start.nsec = 0;
	perout_request.period.sec = ONE_SECOND;
	perout_request.period.nsec = 0;

	if (ioctl(master->fd, PTP_PEROUT_REQUEST, &perout_request)) {
		pr_err("PTP_PEROUT_REQUEST failed: %m");
		return -1;
	}
	return 0;
}

static void ts2phc_phc_master_destroy(struct ts2phc_master *master)
{
	struct ts2phc_phc_master *s =
		container_of(master, struct ts2phc_phc_master, master);
	posix_clock_close(s->clkid);
	free(s);
}

struct ts2phc_master *ts2phc_phc_master_create(struct config *cfg, char *dev)
{
	struct ts2phc_phc_master *master;
	int junk;

	master = calloc(1, sizeof(*master));
	if (!master) {
		return NULL;
	}
	master->master.destroy = ts2phc_phc_master_destroy;

	master->clkid = posix_clock_open(dev, &junk);
	if (master->clkid == CLOCK_INVALID) {
		free(master);
		return NULL;
	}
	master->fd = CLOCKID_TO_FD(master->clkid);

	if (ts2phc_phc_master_activate(master)) {
		ts2phc_phc_master_destroy(&master->master);
		return NULL;
	}

	return &master->master;
}
