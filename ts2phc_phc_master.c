/**
 * @file ts2phc_phc_master.c
 * @note Copyright (C) 2019 Richard Cochran <richardcochran@gmail.com>
 * @note SPDX-License-Identifier: GPL-2.0+
 */
#include <stdlib.h>
#include <string.h>

#include "missing.h"
#include "ts2phc_master_private.h"
#include "ts2phc_phc_master.h"
#include "util.h"

struct ts2phc_phc_master {
	struct ts2phc_master master;
	clockid_t clock;
	int fd;
};

static void ts2phc_phc_master_destroy(struct ts2phc_master *master)
{
	struct ts2phc_phc_master *s =
		container_of(master, struct ts2phc_phc_master, master);
	free(s);
}

struct ts2phc_master *ts2phc_phc_master_create(struct config *cfg, char *dev)
{
	struct ts2phc_phc_master *s;
	int junk;

	s = calloc(1, sizeof(*s));
	if (!s) {
		return NULL;
	}
	s->master.destroy = ts2phc_phc_master_destroy;

	s->clock = posix_clock_open(dev, &junk);
	if (s->clock == CLOCK_INVALID) {
		free(s);
		return NULL;
	}
	s->fd = CLOCKID_TO_FD(s->clock);

	return &s->master;
}
