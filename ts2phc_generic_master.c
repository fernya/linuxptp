/**
 * @file ts2phc_generic_master.c
 * @note Copyright (C) 2019 Richard Cochran <richardcochran@gmail.com>
 * @note SPDX-License-Identifier: GPL-2.0+
 */
#include <stdlib.h>
#include <time.h>

#include "missing.h"
#include "print.h"
#include "ts2phc_generic_master.h"
#include "ts2phc_master_private.h"
#include "util.h"

struct ts2phc_generic_master {
	struct ts2phc_master master;
};

static void ts2phc_generic_master_destroy(struct ts2phc_master *master)
{
	struct ts2phc_generic_master *s =
		container_of(master, struct ts2phc_generic_master, master);
	free(s);
}

/*
 * Returns the time on the PPS source device at which the most recent
 * PPS event was generated.  This implementation assumes that the
 * system time is approximately correct.
 */
static struct timespec ts2phc_generic_master_getppstime(struct ts2phc_master *m)
{
	struct timespec now;
	clock_gettime(CLOCK_TAI, &now);
	return now;
}

struct ts2phc_master *ts2phc_generic_master_create(struct config *cfg, char *dev)
{
	struct ts2phc_generic_master *master;

	master = calloc(1, sizeof(*master));
	if (!master) {
		return NULL;
	}
	master->master.destroy = ts2phc_generic_master_destroy;
	master->master.getppstime = ts2phc_generic_master_getppstime;

	return &master->master;
}
