// pg_stat_ch - Query telemetry producer for otel_api
//
// Collects per-statement execution telemetry via PostgreSQL hooks and emits
// OtelSpans through otel_api's exporter chain (OTLP, JSON log, etc.).

#include "postgres.h"

#include "fmgr.h"
#include "miscadmin.h"
#include "utils/builtins.h"

#include "pg_stat_ch/pg_stat_ch.h"

#include "config/guc.h"
#include "export/psch_span.h"
#include "hooks/hooks.h"

PG_MODULE_MAGIC;

void _PG_init(void) {
  if (!process_shared_preload_libraries_in_progress) {
    elog(WARNING, "pg_stat_ch must be loaded via shared_preload_libraries");
    return;
  }

  elog(LOG, "pg_stat_ch %s: initializing", PG_STAT_CH_VERSION);

  PschInitGuc();
  PschInstallHooks();
  PschInitSpanEmitter();
}

PG_FUNCTION_INFO_V1(pg_stat_ch_version);
Datum pg_stat_ch_version(PG_FUNCTION_ARGS) {
  (void)fcinfo;
  PG_RETURN_TEXT_P(cstring_to_text(PG_STAT_CH_VERSION));
}
