#ifndef DTERRAIN_H
#define DTERRAIN_H

/*
 * Stable, all-in-one C SDK entry point.
 *
 * Applications that do not enable GDAL may still include this header. The
 * declarations in dt_gdal_api.h remain available, but calls return
 * DT_E_UNSUPPORTED when the DLL was built without DT_WITH_GDAL.
 */
#include "dt_api.h"
#include "dt_terrain_api.h"
#include "dt_task_api.h"
#include "dt_cdt_api.h"
#include "dt_gdal_api.h"

#endif
