/**
 * @file ts2phc_generic_master.c
 * @note Copyright (C) 2019 Richard Cochran <richardcochran@gmail.com>
 * @note SPDX-License-Identifier: GPL-2.0+
 */
#include <linux/ptp_clock.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>

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

struct ts2phc_master *ts2phc_generic_master_create(struct config *cfg, char *dev)
{
	struct ts2phc_generic_master *master;

	master = calloc(1, sizeof(*master));
	if (!master) {
		return NULL;
	}
	master->master.destroy = ts2phc_generic_master_destroy;

	return &master->master;
}
