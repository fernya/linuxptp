/**
 * @file phc_pps_source.c
 * @note Copyright (C) 2019 Richard Cochran <richardcochran@gmail.com>
 * @note SPDX-License-Identifier: GPL-2.0+
 */
#include <stdlib.h>
#include <string.h>

#include "missing.h"
#include "phc_pps_source.h"
#include "ts2phc_master_private.h"
#include "util.h"

struct phc_pps_source {
	struct ts2phc_master master;
	clockid_t clock;
	int fd;
};

static void phc_pps_source_destroy(struct ts2phc_master *master)
{
	struct phc_pps_source *s =
		container_of(master, struct phc_pps_source, master);
	free(s);
}

struct ts2phc_master *phc_pps_source_create(struct config *cfg, char *dev)
{
	struct phc_pps_source *s;
	int junk;

	s = calloc(1, sizeof(*s));
	if (!s) {
		return NULL;
	}
	s->master.destroy = phc_pps_source_destroy;

	s->clock = posix_clock_open(dev, &junk);
	if (s->clock == CLOCK_INVALID) {
		free(s);
		return NULL;
	}
	s->fd = CLOCKID_TO_FD(s->clock);

	return &s->master;
}
