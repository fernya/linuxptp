/**
 * @file ts2phc_master.c
 * @note Copyright (C) 2019 Richard Cochran <richardcochran@gmail.com>
 * @note SPDX-License-Identifier: GPL-2.0+
 */
#include "phc_pps_source.h"
#include "ts2phc_master_private.h"

struct pps_master *pps_master_create(struct config *cfg, char *dev,
				     enum pps_master_type type)
{
	struct pps_master *master = NULL;

	switch (type) {
	case CLOCK_PPS_MASTER_GENERIC:
		break;
	case CLOCK_PPS_MASTER_GPSD:
		break;
	case CLOCK_PPS_MASTER_PHC:
		master = phc_pps_source_create(cfg, dev);
		break;
	case CLOCK_PPS_MASTER_UART:
		break;
	}
	return master;
}

void pps_master_destroy(struct pps_master *master)
{
	master->destroy(master);
}
