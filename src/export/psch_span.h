// psch_span.h — OtelSpan emission API for pg_stat_ch
//
// pg_stat_ch registers as an otel_api producer: it collects rich per-statement
// metrics via its own PostgreSQL hooks and emits OtelSpans that are dispatched
// to whatever exporter hooks are registered with otel_api (OTLP, JSON log, etc.).
//
// PschEmitSpan() is the single exit point for all collected events. It builds
// an OtelSpan from the supplied PschEvent and calls otel_api->span_emit().
// Statement events (cmd_type != PSCH_CMD_UNKNOWN) become "pg.statement" spans;
// error-only events (cmd_type == PSCH_CMD_UNKNOWN, from emit_log_hook) become
// "pg.error" spans.
//
// If otel_api is not loaded, PschEmitSpan() is a no-op.

#ifndef PG_STAT_CH_EXPORT_PSCH_SPAN_H_
#define PG_STAT_CH_EXPORT_PSCH_SPAN_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "queue/event.h"

// Call once from _PG_init to register pg_stat_ch's InstrumentationScope with
// otel_api. Uses the order-independent pending-registration mechanism so it
// works whether otel_api loaded before or after pg_stat_ch.
void PschInitSpanEmitter(void);

// Emit a span for one collected event. Thread of execution: ExecutorEnd or
// ProcessUtility (for statements) / emit_log_hook (for errors). Synchronous:
// returns after all registered otel_api emit hooks have been called.
// No-op when otel_api is absent or the event should be dropped.
void PschEmitSpan(const PschEvent *event);

#ifdef __cplusplus
}
#endif

#endif  // PG_STAT_CH_EXPORT_PSCH_SPAN_H_
