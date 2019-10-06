/**
 * @file phc_pps_source.h
 * @note Copyright (C) 2019 Richard Cochran <richardcochran@gmail.com>
 * @note SPDX-License-Identifier: GPL-2.0+
 */
#ifndef HAVE_PHC_PPS_SOURCE_H
#define HAVE_PHC_PPS_SOURCE_H

#include "ts2phc_master.h"

struct ts2phc_master *phc_pps_source_create(struct config *cfg, char *dev);

#endif
