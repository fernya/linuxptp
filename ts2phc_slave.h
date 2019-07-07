/**
 * @file ts2phc_slave.h
 * @brief Utility program to synchronize the PHC clock to external events
 * @note Copyright (C) 2019 Balint Ferencz <fernya@sch.bme.hu>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */
#ifndef HAVE_TS2PHC_SLAVE_H
#define HAVE_TS2PHC_SLAVE_H

#include "ts2phc_master.h"

int ts2phc_slave_add(struct config *cfg, const char *name);

void ts2phc_slave_cleanup(void);

int ts2phc_slave_poll(struct ts2phc_master *master);

#endif
