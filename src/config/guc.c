// pg_stat_ch GUC implementation

#include "postgres.h"

#include <limits.h>

#include "utils/guc.h"

#include "config/guc.h"

bool psch_enabled = true;
int  psch_min_duration_us = 0;
double psch_sample_rate = 1.0;
int  psch_log_min_elevel = WARNING;
int  psch_normalize_cache_max = 32768;

static const struct config_enum_entry log_elevel_options[] = {
    {"debug5",  DEBUG5,  false},
    {"debug4",  DEBUG4,  false},
    {"debug3",  DEBUG3,  false},
    {"debug2",  DEBUG2,  false},
    {"debug1",  DEBUG1,  false},
    {"log",     LOG,     false},
    {"info",    INFO,    false},
    {"notice",  NOTICE,  false},
    {"warning", WARNING, false},
    {"error",   ERROR,   false},
    {"fatal",   FATAL,   false},
    {"panic",   PANIC,   false},
    {NULL,      0,       false},
};

void PschInitGuc(void) {
  DefineCustomBoolVariable(
      "pg_stat_ch.enabled",
      "Enable or disable pg_stat_ch query telemetry collection.",
      NULL,
      &psch_enabled,
      true,
      PGC_SIGHUP,
      0,
      NULL, NULL, NULL);

  DefineCustomIntVariable(
      "pg_stat_ch.min_duration_us",
      "Minimum query duration in microseconds to always capture. 0 captures all.",
      NULL,
      &psch_min_duration_us,
      0,
      0, INT_MAX,
      PGC_SIGHUP,
      0,
      NULL, NULL, NULL);

  DefineCustomRealVariable(
      "pg_stat_ch.sample_rate",
      "Fraction of queries below min_duration_us to sample (0.0-1.0).",
      NULL,
      &psch_sample_rate,
      1.0,
      0.0, 1.0,
      PGC_SIGHUP,
      0,
      NULL, NULL, NULL);

  DefineCustomEnumVariable(
      "pg_stat_ch.log_min_elevel",
      "Minimum ereport level to capture as an error event.",
      NULL,
      &psch_log_min_elevel,
      WARNING,
      log_elevel_options,
      PGC_USERSET,
      0,
      NULL, NULL, NULL);

  DefineCustomIntVariable(
      "pg_stat_ch.normalize_cache_max",
      "Maximum number of normalized query texts cached per backend.",
      NULL,
      &psch_normalize_cache_max,
      32768,
      1, INT_MAX,
      PGC_POSTMASTER,
      0,
      NULL, NULL, NULL);

  MarkGUCPrefixReserved("pg_stat_ch");
}
