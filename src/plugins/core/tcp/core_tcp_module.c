/*
 * Copyright (c) 2010 Cisco Systems, Inc.  All rights reserved.
 * Copyright © 2010-2011 UT-Battelle, LLC. All rights reserved.
 * Copyright © 2010-2011 Oak Ridge National Labs.  All rights reserved.
 *
 * See COPYING in top-level directory
 *
 * $COPYRIGHT$
 *
 */

#include "cci/config.h"

#include <stdio.h>

#include "cci.h"
#include "plugins/core/core.h"

#include "core_tcp.h"

int cci_core_tcp_post_load(cci_plugin_t * me)
{
	assert(me);
	debug(CCI_DB_DRVR, "In tcp post_load");
	return CCI_SUCCESS;
}

int cci_core_tcp_pre_unload(cci_plugin_t * me)
{
	assert(me);
	debug(CCI_DB_DRVR, "In tcp pre_unload");
	return CCI_SUCCESS;
}
