// psch_span.cc — otel_api producer integration for pg_stat_ch
//
// Converts a PschEvent (collected by the hook layer) into a span through the
// otel_api producer API.  The span is started with the event's captured start
// time and ended with its end time, so it is exported already complete.  No
// network I/O here: exporters registered with otel_api handle delivery.

extern "C" {
#include "postgres.h"

#include <inttypes.h>

#include "miscadmin.h"
#include "utils/elog.h"

#ifdef HAVE_OTEL_API
#include <otel_api/otel_producer.h>
#else
#include "otel_producer_stub.h"
#endif

#include "config/guc.h"
#include "export/psch_span.h"
#include "queue/event.h"
}  // extern "C"

// ---------------------------------------------------------------------------
// Module-static state — trivial types only (no global constructors).
// ---------------------------------------------------------------------------

// InstrumentationScope for pg_stat_ch; otel_api fills in .scope on first use.
static OtelTracer psch_tracer = {"pg_stat_ch", PG_STAT_CH_VERSION, NULL, NULL};

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

// OTEL_ATTR_STR() is a C compound literal; fill the struct field by field.
static void
str_attr(OtelAttribute *a, const char *key, const char *value)
{
	a->key  = key;
	a->type = OTEL_ATTR_STRING;
	a->v.s  = value;
}

// Set a string attribute, skipping empty/NULL values.
static void
set_str(OtelSpanRef span, const char *key, const char *value)
{
	if (value != NULL && value[0] != '\0')
		otel_span_set_str(span, key, value);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void
PschEmitSpan(const PschEvent *event)
{
	OtelSpanRef span;
	bool is_error_event;

	if (!psch_enabled || !otel_recording_possible())
		return;

	// Distinguish statement spans from standalone error events.
	is_error_event = (event->cmd_type == PSCH_CMD_UNKNOWN);

	// Already-complete span: back-dated start, detached (never on the active
	// stack), session-owned, ended below.  Parents to the active span or the
	// propagated W3C context; with neither it starts a new, always-recorded
	// trace (pg_stat_ch captures every event).
	OtelSpanStartArgs args = {};
	args.struct_size = sizeof(args);
	args.tracer      = &psch_tracer;
	args.name        = is_error_event ? "pg.error" :
	                   (event->cmd_type == PSCH_CMD_UTILITY ? "pg.utility" : "pg.statement");
	args.kind        = OTEL_SPAN_KIND_SERVER;
	args.parent      = OTEL_PARENT_ACTIVE;
	args.owner       = OTEL_OWNER_SESSION;
	args.detached    = true;
	args.start_time  = event->ts_start;
	args.force_sample = true;
	span = otel_span_start_args(&args);
	if (!otel_span_recording(span))
	{
		otel_span_end(span);
		return;
	}

	// Status: error if an error was captured.
	if (event->err_elevel >= ERROR)
		otel_span_set_status(span, OTEL_STATUS_ERROR,
		                     event->err_message[0] != '\0' ? event->err_message : NULL);

	// --- Attributes ---

	// Identity
	set_str(span, "db.system",          "postgresql");
	set_str(span, "db.name",            event->datname);
	set_str(span, "db.user",            event->username);
	set_str(span, "db.client.address",  event->client_addr);
	set_str(span, "application_name",   event->application_name);
	set_str(span, "db.statement",       event->query);
	otel_span_set_int(span, "db.postgresql.pid", (int64) event->pid);
	set_str(span, "db.postgresql.cmd_type", PschCmdTypeToString(event->cmd_type));

	if (!is_error_event)
	{
		// Statement-specific fields
		if (event->queryid != UINT64CONST(0))
			otel_span_set_int(span, "db.postgresql.query_id", (int64) event->queryid);
		otel_span_set_int(span, "db.postgresql.rows", (int64) event->rows);
		otel_span_set_bool(span, "db.postgresql.top_level", event->top_level);

		// Buffer usage — shared
		otel_span_set_int(span, "db.postgresql.blocks.shared.hit", (int64) event->shared_blks_hit);
		otel_span_set_int(span, "db.postgresql.blocks.shared.read", (int64) event->shared_blks_read);
		otel_span_set_int(span, "db.postgresql.blocks.shared.dirtied", (int64) event->shared_blks_dirtied);
		otel_span_set_int(span, "db.postgresql.blocks.shared.written", (int64) event->shared_blks_written);
		otel_span_set_int(span, "db.postgresql.blocks.shared.read_time_us", (int64) event->shared_blk_read_time_us);
		otel_span_set_int(span, "db.postgresql.blocks.shared.write_time_us", (int64) event->shared_blk_write_time_us);

		// Buffer usage — local
		otel_span_set_int(span, "db.postgresql.blocks.local.hit", (int64) event->local_blks_hit);
		otel_span_set_int(span, "db.postgresql.blocks.local.read", (int64) event->local_blks_read);
		otel_span_set_int(span, "db.postgresql.blocks.local.dirtied", (int64) event->local_blks_dirtied);
		otel_span_set_int(span, "db.postgresql.blocks.local.written", (int64) event->local_blks_written);
		otel_span_set_int(span, "db.postgresql.blocks.local.read_time_us", (int64) event->local_blk_read_time_us);
		otel_span_set_int(span, "db.postgresql.blocks.local.write_time_us", (int64) event->local_blk_write_time_us);

		// Buffer usage — temp
		otel_span_set_int(span, "db.postgresql.blocks.temp.read", (int64) event->temp_blks_read);
		otel_span_set_int(span, "db.postgresql.blocks.temp.written", (int64) event->temp_blks_written);
		otel_span_set_int(span, "db.postgresql.blocks.temp.read_time_us", (int64) event->temp_blk_read_time_us);
		otel_span_set_int(span, "db.postgresql.blocks.temp.write_time_us", (int64) event->temp_blk_write_time_us);

		// WAL
		otel_span_set_int(span, "db.postgresql.wal.records", (int64) event->wal_records);
		otel_span_set_int(span, "db.postgresql.wal.fpi", (int64) event->wal_fpi);
		otel_span_set_int(span, "db.postgresql.wal.bytes", (int64) event->wal_bytes);

		// CPU
		otel_span_set_int(span, "db.postgresql.cpu.user_us", (int64) event->cpu_user_time_us);
		otel_span_set_int(span, "db.postgresql.cpu.sys_us", (int64) event->cpu_sys_time_us);

		// JIT (fields are 0 when not applicable)
		if (event->jit_functions > 0)
		{
			otel_span_set_int(span, "db.postgresql.jit.functions", (int64) event->jit_functions);
			otel_span_set_int(span, "db.postgresql.jit.generation_time_us", (int64) event->jit_generation_time_us);
			otel_span_set_int(span, "db.postgresql.jit.deform_time_us", (int64) event->jit_deform_time_us);
			otel_span_set_int(span, "db.postgresql.jit.inlining_time_us", (int64) event->jit_inlining_time_us);
			otel_span_set_int(span, "db.postgresql.jit.optimization_time_us", (int64) event->jit_optimization_time_us);
			otel_span_set_int(span, "db.postgresql.jit.emission_time_us", (int64) event->jit_emission_time_us);
		}

		// Parallel workers (PG18+; fields are 0 on older versions)
		if (event->parallel_workers_planned > 0)
		{
			otel_span_set_int(span, "db.postgresql.parallel.workers_planned", (int64) event->parallel_workers_planned);
			otel_span_set_int(span, "db.postgresql.parallel.workers_launched", (int64) event->parallel_workers_launched);
		}
	}

	// Error attributes (present on both error events and errored statements)
	if (event->err_sqlstate[0] != '\0')
	{
		set_str(span, "db.postgresql.err.sqlstate", event->err_sqlstate);
		if (event->err_message[0] != '\0')
			set_str(span, "db.postgresql.err.message", event->err_message);

		// Also record as an "exception" event so OTel-native consumers see it in
		// the event stream; same attribute keys otel_api uses when it lowers an
		// ereport.  Names and values are copied.
		OtelAttribute ev_attrs[3] = {};
		int           ev_n = 0;

		if (event->err_message[0] != '\0')
			str_attr(&ev_attrs[ev_n++], "exception.message", event->err_message);
		str_attr(&ev_attrs[ev_n++], "postgres.sqlstate", event->err_sqlstate);
		ev_attrs[ev_n].key  = "postgres.elevel";
		ev_attrs[ev_n].type = OTEL_ATTR_INT;
		ev_attrs[ev_n].v.i  = event->err_elevel;
		ev_n++;
		otel_span_add_event(span, "exception", event->ts_start, ev_attrs, ev_n);
	}

	otel_span_end_at(span, event->ts_start + (TimestampTz) event->duration_us);
}
