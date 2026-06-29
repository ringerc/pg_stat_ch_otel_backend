#ifndef PG_STAT_CH_INCLUDE_PG_STAT_CH_PG_STAT_CH_H_
#define PG_STAT_CH_INCLUDE_PG_STAT_CH_PG_STAT_CH_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "postgres.h"
#include "fmgr.h"

#ifndef PG_STAT_CH_VERSION
#define PG_STAT_CH_VERSION "unknown"
#endif

PGDLLEXPORT Datum pg_stat_ch_version(PG_FUNCTION_ARGS);

#ifdef __cplusplus
}
#endif

#endif  // PG_STAT_CH_INCLUDE_PG_STAT_CH_PG_STAT_CH_H_
