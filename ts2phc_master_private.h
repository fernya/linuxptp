/**
 * @file ts2phc_master_private.h
 * @note Copyright (C) 2019 Richard Cochran <richardcochran@gmail.com>
 * @note SPDX-License-Identifier: GPL-2.0+
 */
#ifndef HAVE_TS2PHC_MASTER_PRIVATE_H
#define HAVE_TS2PHC_MASTER_PRIVATE_H

#include <stdint.h>

#include "contain.h"
#include "ts2phc_master.h"

struct pps_master {
	void (*destroy)(struct pps_master *pps_master);
};

#endif
