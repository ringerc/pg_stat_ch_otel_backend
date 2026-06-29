// psch_span.cc — otel_api producer integration for pg_stat_ch
//
// Converts a PschEvent (collected by the hook layer) into an OtelSpan and
// dispatches it through otel_api's exporter chain.  No network I/O here:
// exporters registered with otel_api handle delivery asynchronously in their
// own threads/bgworkers (e.g. the Rust OTLP batch exporter).

extern "C" {
#include "postgres.h"

#include <inttypes.h>
#include <string.h>
#include <sys/types.h>

#include "miscadmin.h"
#include "port.h"
#include "utils/elog.h"

#include <otel_api/otel_api.h>

#include "config/guc.h"
#include "export/psch_span.h"
#include "queue/event.h"
}  // extern "C"

// ---------------------------------------------------------------------------
// Module-static state — trivial types only (no global constructors).
// ---------------------------------------------------------------------------

// InstrumentationScope handle for pg_stat_ch.  Set at _PG_init via the
// pending-registration mechanism; may be NULL if otel_api was absent when
// _PG_init ran AND the pending registration was never drained.  Checked
// defensively before use.
static const OtelInstrumentationScope *psch_tracer = NULL;

// Pending registration node (file-static, zero-initialized).  Used if
// otel_api loads after pg_stat_ch's _PG_init.
static OtelPendingRegistration psch_pending_reg;

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

// Convert 16 raw bytes to 32 lowercase hex chars + NUL.
static void
bytes_to_hex32(const unsigned char *src, char dst[33])
{
	static const char hex[] = "0123456789abcdef";
	for (int i = 0; i < 16; i++)
	{
		dst[i * 2]     = hex[(src[i] >> 4) & 0xf];
		dst[i * 2 + 1] = hex[src[i] & 0xf];
	}
	dst[32] = '\0';
}

// Lazily get a valid otel_api handle, or NULL if unavailable.
static const OtelTracingApi *
get_api(void)
{
	return otel_api_get();
}

// Lazily ensure psch_tracer is registered.  Called each time we need to emit
// so that late-loading (otel_api after pg_stat_ch) works without a startup
// hook.  No-op once psch_tracer is set.
static void
ensure_tracer(const OtelTracingApi *api)
{
	if (psch_tracer == NULL)
		psch_tracer = api->tracer_register("pg_stat_ch", PG_STAT_CH_VERSION, NULL);
}

// Add a string attribute to the span. Skips empty/NULL values.
// Key must be a string literal (not copied). Value pointer must remain valid
// until span_emit() returns — callers use stack buffers or PschEvent fields.
static void
add_attr(const OtelTracingApi *api, OtelSpan *span,
		 const char *key, const char *value)
{
	if (value == NULL || value[0] == '\0')
		return;
	if (span->n_attrs < OTEL_INLINE_ATTRS)
	{
		span->attrs[span->n_attrs].key   = key;
		span->attrs[span->n_attrs].value = value;
		span->n_attrs++;
	}
	else
	{
		api->span_add_attribute_string(span, key, value);
	}
}

// Populate the trace_id / parent_span_id / trace_flags fields of a span from
// the current W3C root context, or generate a fresh standalone trace when no
// context is present.
static void
link_trace_context(const OtelTracingApi *api, OtelSpan *span)
{
	OtelRootContextSnapshot rc;

	api->get_root_context_snapshot(&rc);

	if (rc.is_set)
	{
		memcpy(span->trace_id, rc.trace_id, sizeof(span->trace_id));
		memcpy(span->parent_span_id, rc.span_id, sizeof(span->parent_span_id));
		memcpy(span->trace_flags, rc.trace_flags, sizeof(span->trace_flags));
		span->tracestate = rc.tracestate;
	}
	else
	{
		// No propagated context: synthesize a standalone trace.
		// pg_stat_ch always-captures, so we still want a valid trace_id.
		unsigned char buf[16];
		if (!pg_strong_random(buf, sizeof(buf)))
		{
			uint64 fallback = (uint64) GetCurrentTimestamp() ^ (uint64) MyProcPid;
			memcpy(buf, &fallback, sizeof(fallback));
			memset(buf + sizeof(fallback), 0xa5, sizeof(buf) - sizeof(fallback));
		}
		bytes_to_hex32(buf, span->trace_id);
		span->parent_span_id[0] = '\0';
		strcpy(span->trace_flags, "01");  // sampled
	}
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void
PschInitSpanEmitter(void)
{
	psch_pending_reg.tracer_name    = "pg_stat_ch";
	psch_pending_reg.tracer_version = PG_STAT_CH_VERSION;
	psch_pending_reg.tracer_out     = &psch_tracer;
	otel_api_register_when_ready(&psch_pending_reg);
}

void
PschEmitSpan(const PschEvent *event)
{
	const OtelTracingApi *api;
	OtelSpan span;
	bool is_error_event;

	if (!psch_enabled)
		return;

	api = get_api();
	if (api == NULL)
		return;		// otel_api not loaded; silent no-op

	ensure_tracer(api);

	// Distinguish statement spans from standalone error events.
	is_error_event = (event->cmd_type == PSCH_CMD_UNKNOWN);

	// Stack buffers for numeric attribute values.  All must outlive span_emit().
	// 40 slots × 24 chars covers every numeric field in PschEvent (max ~32 emitted
	// when JIT + parallel-worker attrs are both present) with headroom.
	char bufs[40][24];
	int  bi = 0;
#define NBUF(fmt, val) \
	(AssertMacro(bi < (int) lengthof(bufs)), \
	 snprintf(bufs[bi], sizeof(bufs[bi]), fmt, (val)), bufs[bi++])

	// --- Initialize span ---
	api->span_init(&span, psch_tracer,
				   is_error_event ? "pg.error" :
				   (event->cmd_type == PSCH_CMD_UTILITY ? "pg.utility" : "pg.statement"),
				   OTEL_SPAN_KIND_SERVER);

	// Override start/end time with captured event timing.
	// span_init sets start_time = now; we want the actual statement time.
	span.start_time = event->ts_start;
	span.end_time   = event->ts_start + (TimestampTz) event->duration_us;

	// Status: error if an error was captured.
	span.status = (event->err_elevel >= ERROR) ? OTEL_STATUS_ERROR : OTEL_STATUS_UNSET;
	if (span.status == OTEL_STATUS_ERROR && event->err_message[0] != '\0')
		span.status_description = event->err_message;

	// Trace context (W3C parent linkage or fresh trace).
	link_trace_context(api, &span);

	// --- Attributes ---

	// Identity
	add_attr(api, &span, "db.system",          "postgresql");
	add_attr(api, &span, "db.name",            event->datname);
	add_attr(api, &span, "db.user",            event->username);
	add_attr(api, &span, "db.client.address",  event->client_addr);
	add_attr(api, &span, "application_name",   event->application_name);
	add_attr(api, &span, "db.statement",       event->query);
	add_attr(api, &span, "db.postgresql.pid",  NBUF("%d", event->pid));
	add_attr(api, &span, "db.postgresql.cmd_type",
	         PschCmdTypeToString(event->cmd_type));

	if (!is_error_event)
	{
		// Statement-specific fields
		if (event->queryid != UINT64CONST(0))
			add_attr(api, &span, "db.postgresql.query_id",
					 NBUF("%" PRIu64, event->queryid));
		add_attr(api, &span, "db.postgresql.rows",
				 NBUF("%" PRIu64, event->rows));
		add_attr(api, &span, "db.postgresql.top_level",
				 event->top_level ? "true" : "false");

		// Buffer usage — shared
		add_attr(api, &span, "db.postgresql.blocks.shared.hit",
				 NBUF("%" PRId64, event->shared_blks_hit));
		add_attr(api, &span, "db.postgresql.blocks.shared.read",
				 NBUF("%" PRId64, event->shared_blks_read));
		add_attr(api, &span, "db.postgresql.blocks.shared.dirtied",
				 NBUF("%" PRId64, event->shared_blks_dirtied));
		add_attr(api, &span, "db.postgresql.blocks.shared.written",
				 NBUF("%" PRId64, event->shared_blks_written));
		add_attr(api, &span, "db.postgresql.blocks.shared.read_time_us",
				 NBUF("%" PRId64, event->shared_blk_read_time_us));
		add_attr(api, &span, "db.postgresql.blocks.shared.write_time_us",
				 NBUF("%" PRId64, event->shared_blk_write_time_us));

		// Buffer usage — local
		add_attr(api, &span, "db.postgresql.blocks.local.hit",
				 NBUF("%" PRId64, event->local_blks_hit));
		add_attr(api, &span, "db.postgresql.blocks.local.read",
				 NBUF("%" PRId64, event->local_blks_read));
		add_attr(api, &span, "db.postgresql.blocks.local.dirtied",
				 NBUF("%" PRId64, event->local_blks_dirtied));
		add_attr(api, &span, "db.postgresql.blocks.local.written",
				 NBUF("%" PRId64, event->local_blks_written));
		add_attr(api, &span, "db.postgresql.blocks.local.read_time_us",
				 NBUF("%" PRId64, event->local_blk_read_time_us));
		add_attr(api, &span, "db.postgresql.blocks.local.write_time_us",
				 NBUF("%" PRId64, event->local_blk_write_time_us));

		// Buffer usage — temp
		add_attr(api, &span, "db.postgresql.blocks.temp.read",
				 NBUF("%" PRId64, event->temp_blks_read));
		add_attr(api, &span, "db.postgresql.blocks.temp.written",
				 NBUF("%" PRId64, event->temp_blks_written));
		add_attr(api, &span, "db.postgresql.blocks.temp.read_time_us",
				 NBUF("%" PRId64, event->temp_blk_read_time_us));
		add_attr(api, &span, "db.postgresql.blocks.temp.write_time_us",
				 NBUF("%" PRId64, event->temp_blk_write_time_us));

		// WAL
		add_attr(api, &span, "db.postgresql.wal.records",
				 NBUF("%" PRId64, event->wal_records));
		add_attr(api, &span, "db.postgresql.wal.fpi",
				 NBUF("%" PRId64, event->wal_fpi));
		add_attr(api, &span, "db.postgresql.wal.bytes",
				 NBUF("%" PRIu64, event->wal_bytes));

		// CPU
		add_attr(api, &span, "db.postgresql.cpu.user_us",
				 NBUF("%" PRId64, event->cpu_user_time_us));
		add_attr(api, &span, "db.postgresql.cpu.sys_us",
				 NBUF("%" PRId64, event->cpu_sys_time_us));

		// JIT (fields are 0 when not applicable)
		if (event->jit_functions > 0)
		{
			add_attr(api, &span, "db.postgresql.jit.functions",
					 NBUF("%d", event->jit_functions));
			add_attr(api, &span, "db.postgresql.jit.generation_time_us",
					 NBUF("%d", event->jit_generation_time_us));
			add_attr(api, &span, "db.postgresql.jit.deform_time_us",
					 NBUF("%d", event->jit_deform_time_us));
			add_attr(api, &span, "db.postgresql.jit.inlining_time_us",
					 NBUF("%d", event->jit_inlining_time_us));
			add_attr(api, &span, "db.postgresql.jit.optimization_time_us",
					 NBUF("%d", event->jit_optimization_time_us));
			add_attr(api, &span, "db.postgresql.jit.emission_time_us",
					 NBUF("%d", event->jit_emission_time_us));
		}

		// Parallel workers (PG18+; fields are 0 on older versions)
		if (event->parallel_workers_planned > 0)
		{
			add_attr(api, &span, "db.postgresql.parallel.workers_planned",
					 NBUF("%d", event->parallel_workers_planned));
			add_attr(api, &span, "db.postgresql.parallel.workers_launched",
					 NBUF("%d", event->parallel_workers_launched));
		}
	}

	// Error attributes (present on both error events and errored statements)
	if (event->err_sqlstate[0] != '\0')
	{
		add_attr(api, &span, "db.postgresql.err.sqlstate", event->err_sqlstate);
		if (event->err_message[0] != '\0')
			add_attr(api, &span, "db.postgresql.err.message", event->err_message);

		// Also record as a span event so OTel-native consumers see it in the
		// event stream rather than only as flat attributes.
		span.inline_event_used        = true;
		span.inline_event.core.time   = event->ts_start;
		span.inline_event.core.elevel = event->err_elevel;
		memcpy(span.inline_event.core.sqlstate, event->err_sqlstate, 6);
		span.inline_event.message =
			event->err_message[0] != '\0' ? event->err_message : NULL;
	}

#undef NBUF

	// Dispatch through otel_api's exporter chain.  span_emit() is synchronous
	// and fires all registered emit hooks before returning.  The span was never
	// pushed onto the active stack, so span_emit simply calls dispatch_span()
	// and returns — no pop, no stack interaction.
	api->span_emit(&span);
}
