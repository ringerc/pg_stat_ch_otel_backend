// pg_stat_ch GUC declarations
#ifndef PG_STAT_CH_GUC_H
#define PG_STAT_CH_GUC_H

#ifdef __cplusplus
extern "C" {
#endif

// Whether to collect telemetry at all.
extern bool psch_enabled;

// Only sample queries at or above this duration (microseconds). 0 = all.
extern int psch_min_duration_us;

// Fraction of queries below min_duration_us to sample. 1.0 = all, 0.0 = none.
extern double psch_sample_rate;

// Minimum ereport level to capture as error events.
extern int psch_log_min_elevel;

// Maximum number of normalized query texts to cache per backend.
extern int psch_normalize_cache_max;

void PschInitGuc(void);

#ifdef __cplusplus
}
#endif

#endif  // PG_STAT_CH_GUC_H
