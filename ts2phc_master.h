/**
 * @file ts2phc_master.h
 * @note Copyright (C) 2019 Richard Cochran <richardcochran@gmail.com>
 * @note SPDX-License-Identifier: GPL-2.0+
 */
#ifndef HAVE_TS2PHC_MASTER_H
#define HAVE_TS2PHC_MASTER_H

struct config;

/**
 * Opaque type
 */
struct pps_master_clock;

/**
 * Defines the available PPS master clocks.
 */
enum pps_master_type {
	CLOCK_PPS_MASTER_GENERIC,
	CLOCK_PPS_MASTER_GPSD,
	CLOCK_PPS_MASTER_PHC,
	CLOCK_PPS_MASTER_UART,
};

/**
 * Create a new instance of a PPS master clock.
 * @param cfg	Pointer to a valid configuration. 
 * @param type	The type of the clock to create.
 * @return	A pointer to a new PPS master clock on success, NULL otherwise.
 */
struct pps_master *pps_master_create(struct config *cfg,
				     enum pps_master_type type);

/**
 * Destroy an instance of a PPS master clock.
 * @param master Pointer to a master obtained via @ref pps_master_create().
 */
void pps_master_destroy(struct pps_master *master);

#endif

