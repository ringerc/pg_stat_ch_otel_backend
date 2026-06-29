# Code review brief: pg_stat_ch otel_api port

Summarises what was done and what to look at for a review. Not a permanent doc —
discard after review.

## What this is

pg_stat_ch (upstream: github.com/ClickHouse/pg_stat_ch) is a PostgreSQL extension
that captures per-statement telemetry via executor/utility/log hooks. Upstream it
ships data via a ring-buffer → bgworker → ClickHouse pipeline. This port replaces
that output path with a call to `otel_api->span_emit()`, turning pg_stat_ch into
an otel_api *producer*. The hooks, stats collection, and query normalization are
unchanged.

## What was removed

- Ring buffer + DSA interner: `src/queue/{shmem,psch_dsa,local_batch,ring_entry,query_intern}.*`
- Bgworker: `src/worker/bgworker.*`
- ClickHouse / OTLP / Arrow exporters: `src/export/{clickhouse*,arrow_batch,otel_exporter,stats_exporter,exporter_interface}.*`
- ClickHouse and OTLP GUCs from `src/config/guc.c` (5 survive: enabled, min_duration_us, sample_rate, log_min_elevel, normalize_cache_max)
- SQL functions `pg_stat_ch_stats()`, `pg_stat_ch_reset()`, `pg_stat_ch_flush()`
- `pg_stat_ch_stats` declaration from the public header
- The `psch_shared_state` check from `ShouldCaptureLog()` (no shared memory anymore)
- `PschSuppressErrorCapture()` (was a re-entry guard for the enqueue path)

## What was added

**`src/export/psch_span.h`** — two-function API:
- `PschInitSpanEmitter()` — registers a `OtelPendingRegistration` so the tracer
  handle is claimed regardless of library load order.
- `PschEmitSpan(const PschEvent *event)` — builds and emits an OtelSpan.

**`src/export/psch_span.cc`** — the C++ implementation:
- Calls `otel_api_get()` on the hot path (lazy, no-op if API absent).
- `span_init` with kind=SERVER, overrides `start_time`/`end_time` from the event.
- Reads W3C trace context via `get_root_context_snapshot()`; if absent, synthesizes
  a standalone trace with `pg_strong_random()`.
- Adds ~31 string attributes using a `char bufs[30][24]` stack buffer and a
  `NBUF(fmt, val)` macro for numerics; pointers valid until `span_emit()` returns.
- Calls `span_emit()` directly — **no `span_push`** — so the span never touches
  the active span stack that `otel_postgres_tracing` manages.
- Adds an `OtelSpanEvent` for error spans from `emit_log_hook`.
- Span names: `"pg.statement"`, `"pg.utility"`, `"pg.error"`.
- InstrumentationScope: `"pg_stat_ch"` + git-derived version string.

**`src/pg_stat_ch.c`** — rewritten to call `PschInitGuc()`, `PschInstallHooks()`,
`PschInitSpanEmitter()` in `_PG_init`. No shmem request, no bgworker registration.

## PG19 compatibility fixes (hooks.c, query_normalize.c)

- `QueryDesc->totaltime` → `QueryDesc->query_instr` (PG19; macro `PSCH_QUERY_INSTR`)
- `InstrAlloc(1, INSTRUMENT_ALL, false)` → `InstrAlloc(INSTRUMENT_ALL)` (PG19 drops count + outer_instrument args)
- `InstrEndLoop(totaltime)` → skipped on PG19 (ExecutorRun already finalized timing; calling InstrStop would fault with "InstrStop called without start")
- `post_parse_analyze_hook` function signature: `JumbleState*` → `const JumbleState*`
- `PschNormalizeQuery` and `FillInConstantLengths` updated to `const JumbleState*`
- `core_yy_extra_type.escape_string_warning` removed in PG19; guarded with `#if PG_VERSION_NUM < 190000`
- Added `#include "storage/proc.h"` for `MyProc` (was implicit via shmem.h previously)

## CMakeLists.txt

Removed: `find_package(opentelemetry-cpp)`, `find_package(Arrow)`, `find_package(lz4)`,
`find_package(zstd)`, `find_package(OpenSSL)` and all `target_link_libraries` for those.

Added: `target_include_directories(SYSTEM PRIVATE ${CMAKE_SOURCE_DIR}/../../postgres_otel_api)`
so `psch_span.cc` can `#include <otel_api/otel_api.h>`. The path is relative to
the workspace layout; the operand Dockerfile needs a `-DOTEL_API_INCLUDE_DIR=`
override (see `hcp-operand/pg_stat_ch-operand-handover.md`).

## Smoke test results (PG19, otel_api.emit_spans_to_log = on)

```
SELECT count(*) FROM pg_class WHERE relkind = 'r':
  → pg.statement, scope=pg_stat_ch
    blocks.shared.hit=12, blocks.shared.read=3, cpu.user_us=1093
    db.statement="SELECT count(*) FROM pg_class WHERE relkind = $1"

SELECT 1/0:
  → pg.error, status=2, scope=pg_stat_ch
    db.postgresql.err.sqlstate="22012", err.message="division by zero"
    events=[{sqlstate:"22012", message:"division by zero"}]
```

Both loaded alongside `otel_postgres_tracing` without conflict. Two spans per
statement — different `scope.name`, same trace_id if a traceparent was propagated.

## Things to look for in review

1. **`psch_span.cc` attribute coverage** — check that all 31 attributes match the
   pg_stat_ch `PschEvent` fields and use the right OTel semantic convention names
   (the `db.postgresql.*` namespace is non-standard; confirm it's intentional
   rather than `db.query.*` or similar).

2. **Numeric buffer correctness** — `NBUF` snprintfs into `bufs[slot][24]`, 30
   slots, manual slot counter. Verify the slot count is ≥ the number of `add_attr`
   calls that use it (currently ~23 numeric attrs) and doesn't overflow 24 chars
   for any realistic value (uint64 max is 20 digits + NUL = 21 bytes; fine).

3. **Standalone trace synthesis** — when no W3C context is present,
   `pg_strong_random()` fills 16 bytes → `bytes_to_hex32()` → `trace_id[33]`. The
   span_id is generated by `span_init`. Verify both are set on the span before
   `span_emit`.

4. **Error path timing** — `pg.error` spans (from `emit_log_hook`) have
   `start_time == end_time` (both set to the log event timestamp). This is
   intentional — there's no duration for a log event. Confirm it's not zero.

5. **`PSCH_QUERY_INSTR` macro scope** — the macro is defined inside
   `BuildEventFromQueryDesc` and then reused in `PschExecutorStart` and
   `PschExecutorEnd` (which are in a different scope in the same TU). Because it's
   a `#define` it's fine, but it's implicit. Could be made file-static or moved to
   the top of the file.

6. **PG18 path** — the operand targets PG18. The version guards should select:
   `totaltime` (not `query_instr`), `InstrAlloc(1,…,false)`, `InstrEndLoop`, and
   non-const `JumbleState*`. Not tested on PG18; worth a quick build check.
