/**
 * @file ts2phc_master.c
 * @note Copyright (C) 2019 Richard Cochran <richardcochran@gmail.com>
 * @note SPDX-License-Identifier: GPL-2.0+
 */
#include "phc_pps_source.h"
#include "ts2phc_master_private.h"

struct ts2phc_master *ts2phc_master_create(struct config *cfg, char *dev,
					   enum ts2phc_master_type type)
{
	struct ts2phc_master *master = NULL;

	switch (type) {
	case TS2PHC_MASTER_GENERIC:
		break;
	case TS2PHC_MASTER_GPSD:
		break;
	case TS2PHC_MASTER_PHC:
		master = phc_pps_source_create(cfg, dev);
		break;
	case TS2PHC_MASTER_UART:
		break;
	}
	return master;
}

void ts2phc_master_destroy(struct ts2phc_master *master)
{
	master->destroy(master);
}
