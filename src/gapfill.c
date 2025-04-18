#include "postgres.h"

#include "fmgr.h"
#include "miscadmin.h"

#include "catalog/pg_type.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"

PG_MODULE_MAGIC;

/* ------------------------------------------------------------------------
 *  Helpers
 * ------------------------------------------------------------------------*/
#define MICROSECONDS_PER_SECOND  ((int64) 1000000)
#define MICROSECONDS_PER_MINUTE  (MICROSECONDS_PER_SECOND * 60)
#define MICROSECONDS_PER_HOUR    (MICROSECONDS_PER_MINUTE * 60)
#define MICROSECONDS_PER_DAY     (MICROSECONDS_PER_HOUR   * 24)

/* ------------------------------------------------------------------------
 *  time_bucket(bucket_width interval, ts timestamptz) → timestamptz
 * ------------------------------------------------------------------------*/
PG_FUNCTION_INFO_V1(pg_time_bucket_c);
Datum
pg_time_bucket_c(PG_FUNCTION_ARGS)
{
    Interval    *bucket_width = PG_GETARG_INTERVAL_P(0);
    TimestampTz  ts           = PG_GETARG_TIMESTAMPTZ(1);

    /* Reject intervals that contain months or days — supporting them would
    * require calendar‑aware arithmetic. */
    if (bucket_width->month != 0 || bucket_width->day != 0)
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                errmsg("bucket_width containing months or days is not supported")));

    /* Interval->time is stored in microseconds. */
    int64 width_usec = bucket_width->time;

    if (width_usec <= 0)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                errmsg("bucket_width must be greater than zero")));

    /* Timestamptz is also microseconds since PostgreSQL epoch (2000‑01‑01). */
    int64 ts_usec      = ts;
    int64 bucket_start = (ts_usec / width_usec) * width_usec;

    PG_RETURN_TIMESTAMPTZ((TimestampTz) bucket_start);
}

/* ------------------------------------------------------------------------
 *  first(anyelement, timestamptz) aggregate
 * ------------------------------------------------------------------------*/
typedef struct FirstLastState
{
    TimestampTz ts;
    Datum       value;
    bool        is_null;
    bool        is_set;
} FirstLastState;

/* Transition function */
PG_FUNCTION_INFO_V1(pg_first_sfunc_c);
Datum
pg_first_sfunc_c(PG_FUNCTION_ARGS)
{
    MemoryContext aggctx;
    FirstLastState *state;

    /* Locate (or create) the aggregate state */
    if (PG_ARGISNULL(0))
    {
        if (!AggCheckCallContext(fcinfo, &aggctx))
            aggctx = CurrentMemoryContext; /* should not happen */

        state = (FirstLastState *) MemoryContextAllocZero(aggctx,
                                                        sizeof(FirstLastState));
    }
    else
    {
        state = (FirstLastState *) PG_GETARG_POINTER(0);
    }

    /* If the incoming timestamp is NULL we ignore this row */
    if (PG_ARGISNULL(2))
        PG_RETURN_POINTER(state);

    TimestampTz ts = PG_GETARG_TIMESTAMPTZ(2);

    /* Decide whether to store this row */
    if (!state->is_set || ts < state->ts)
    {
        state->ts      = ts;
        state->is_set  = true;
        state->is_null = PG_ARGISNULL(1);

        if (!state->is_null)
        {
            Datum val   = PG_GETARG_DATUM(1);
            Oid   valty = get_fn_expr_argtype(fcinfo->flinfo, 1);
            int16 typlen;
            bool  typbyval;
            get_typlenbyval(valty, &typlen, &typbyval);

            state->value = datumCopy(val, typbyval, typlen);
        }
    }

    PG_RETURN_POINTER(state);
}

/* Final function */
PG_FUNCTION_INFO_V1(pg_first_ffunc_c);
Datum
pg_first_ffunc_c(PG_FUNCTION_ARGS)
{
    FirstLastState *state;

    if (PG_ARGISNULL(0))
        PG_RETURN_NULL();

    state = (FirstLastState *) PG_GETARG_POINTER(0);

    if (!state->is_set || state->is_null)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(state->value);
}