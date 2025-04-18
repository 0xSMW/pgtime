-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\\echo Use "CREATE EXTENSION pgtime" to load this file. \\quit

-- Create the schema for the extension
CREATE SCHEMA IF NOT EXISTS pgtime;

-- Metadata table to track managed time-series tables
CREATE TABLE pgtime.tables (
    parent_table REGCLASS PRIMARY KEY,
    time_column NAME NOT NULL,
    partition_interval INTERVAL NOT NULL,
    retention_interval INTERVAL, -- If NULL, no retention
    compression_interval INTERVAL, -- If NULL, no compression (PG16+)
    last_partition_created_at TIMESTAMPTZ, -- Tracked by worker
    created_at TIMESTAMPTZ DEFAULT NOW()
);

COMMENT ON TABLE pgtime.tables IS 'Metadata for tables managed by the pgtime extension worker.';

-- Function to set up a table for time-series management
CREATE FUNCTION pgtime.create_timeseries_table(
    _parent_table REGCLASS,
    _time_column NAME,
    _partition_interval INTERVAL,
    _retention_interval INTERVAL DEFAULT NULL,
    _compression_interval INTERVAL DEFAULT NULL
) RETURNS VOID LANGUAGE plpgsql AS $$
DECLARE
    _parent_schema NAME;
    _parent_table_name NAME;
    _partition_check RECORD;
    _initial_partition_name TEXT;
    _initial_partition_start TIMESTAMPTZ;
    _initial_partition_end TIMESTAMPTZ;
BEGIN
    -- Basic validation
    IF _parent_table IS NULL OR _time_column IS NULL OR _partition_interval IS NULL THEN
        RAISE EXCEPTION 'Parent table, time column, and partition interval must be provided';
    END IF;
    IF _partition_interval <= interval '0' THEN
        RAISE EXCEPTION 'Partition interval must be positive';
    END IF;
    IF _retention_interval IS NOT NULL AND _retention_interval <= interval '0' THEN
        RAISE EXCEPTION 'Retention interval must be positive if provided';
    END IF;
    IF _compression_interval IS NOT NULL AND _compression_interval <= interval '0' THEN
        RAISE EXCEPTION 'Compression interval must be positive if provided';
    END IF;
    IF _retention_interval IS NOT NULL AND _compression_interval IS NOT NULL AND _compression_interval >= _retention_interval THEN
        RAISE EXCEPTION 'Compression interval must be less than retention interval';
    END IF;

    -- Check if table exists and is partitioned by the specified column
    SELECT n.nspname, c.relname INTO _parent_schema, _parent_table_name
    FROM pg_class c JOIN pg_namespace n ON c.relnamespace = n.oid
    WHERE c.oid = _parent_table;

    IF NOT FOUND THEN
        RAISE EXCEPTION 'Table % does not exist', _parent_table;
    END IF;

    SELECT pt.partstrat, a.attname INTO _partition_check
    FROM pg_partitioned_table pt
    JOIN pg_attribute a ON a.attrelid = pt.partrelid AND a.attnum = ANY(pt.partattrs)
    WHERE pt.partrelid = _parent_table AND a.attname = _time_column;

    IF NOT FOUND THEN
        RAISE EXCEPTION 'Table % is not partitioned by range on column %', _parent_table, _time_column;
    ELSIF _partition_check.partstrat <> 'r' THEN
         RAISE EXCEPTION 'Table % is not partitioned by RANGE (found strategy %)', _parent_table, _partition_check.partstrat;
    END IF;

    -- Check if already managed
    IF EXISTS (SELECT 1 FROM pgtime.tables WHERE parent_table = _parent_table) THEN
        RAISE NOTICE 'Table % is already managed by pgtime. Updating configuration.', _parent_table;
        UPDATE pgtime.tables
        SET time_column = _time_column,
            partition_interval = _partition_interval,
            retention_interval = _retention_interval,
            compression_interval = _compression_interval
        WHERE parent_table = _parent_table;
        RETURN;
    END IF;

    -- Insert metadata for the worker
    INSERT INTO pgtime.tables (parent_table, time_column, partition_interval, retention_interval, compression_interval)
    VALUES (_parent_table, _time_column, _partition_interval, _retention_interval, _compression_interval);

    RAISE NOTICE 'Table % configured for pgtime management.', _parent_table;
    RAISE NOTICE 'Background worker will manage partitions, retention, and compression.';

    -- Optional: Create the *very first* partition immediately if needed?
    -- The worker should handle this, but could be done here for instant usability.
    -- Example: Create partition for the current interval
    -- _initial_partition_start := date_trunc(... based on interval ..., now());
    -- _initial_partition_end := _initial_partition_start + _partition_interval;
    -- _initial_partition_name := format('%s_%s', _parent_table_name, to_char(_initial_partition_start, 'YYYYMMDDHH24MISS'));
    -- EXECUTE format('CREATE TABLE IF NOT EXISTS %I.%I PARTITION OF %I.%I FOR VALUES FROM (%L) TO (%L)',
    --                _parent_schema, _initial_partition_name, _parent_schema, _parent_table_name,
    --                _initial_partition_start, _initial_partition_end);
    -- RAISE NOTICE 'Created initial partition %', _initial_partition_name;

END;
$$;

COMMENT ON FUNCTION pgtime.create_timeseries_table(REGCLASS, NAME, INTERVAL, INTERVAL, INTERVAL)
IS 'Configures an existing partitioned table for management by the pgtime background worker.';


-- SQL function bindings for C implementations in gapfill.c

-- time_bucket function (placeholder binding)
CREATE FUNCTION time_bucket(bucket_width interval, ts timestamptz)
  RETURNS timestamptz
  AS '$libdir/pgtime', 'pg_time_bucket_c'
  LANGUAGE C IMMUTABLE STRICT;

COMMENT ON FUNCTION time_bucket(interval, timestamptz) IS 'Truncates the timestamp to the specified interval width (C implementation).';

-- 'first' aggregate state transition function (placeholder binding)
CREATE FUNCTION first_sfunc(internal, anyelement, timestamptz)
  RETURNS internal
  AS '$libdir/pgtime', 'pg_first_sfunc_c'
  LANGUAGE C; -- Not immutable due to state modification

-- 'first' aggregate final function (placeholder binding)
CREATE FUNCTION first_ffunc(internal)
  RETURNS anyelement
  AS '$libdir/pgtime', 'pg_first_ffunc_c'
  LANGUAGE C IMMUTABLE STRICT; -- Final func can be immutable

-- 'first' aggregate definition
CREATE AGGREGATE first (anyelement ORDER BY timestamptz) (
    SFUNC = first_sfunc,
    STYPE = internal,
    FINALFUNC = first_ffunc
    -- Add parallel support functions (MSFUNC, MINVFUNC, MFINALFUNC, COMBINEFUNC, SERIALFUNC, DESERIALFUNC)
    -- if implemented in C for parallel aggregation. Example:
    -- COMBINEFUNC = first_combinefunc,
    -- SERIALFUNC = first_serialfunc,
    -- DESERIALFUNC = first_deserialfunc,
    -- PARALLEL = SAFE
);

COMMENT ON AGGREGATE first(anyelement ORDER BY timestamptz) IS 'Returns the first value of anyelement based on the earliest timestamptz (C implementation).';

-- TODO: Define 'last' aggregate similarly (last_sfunc, last_ffunc, CREATE AGGREGATE last ...)


-- Helper function to create continuous aggregates using pg_cron
CREATE FUNCTION pgtime.create_continuous_aggregate(
    _view_name NAME,
    _aggregate_query TEXT, -- The SELECT query defining the view
    _refresh_schedule TEXT -- Cron syntax, e.g., '*/5 * * * *'
) RETURNS VOID LANGUAGE plpgsql AS $$
DECLARE
    _refresh_command TEXT;
    _job_name TEXT := 'pgtime_refresh_' || _view_name;
    _view_schema NAME;
    _view_unqualified_name NAME;
    _job_exists BOOLEAN;
BEGIN
    -- Basic validation
    IF _view_name IS NULL OR _aggregate_query IS NULL OR _refresh_schedule IS NULL THEN
        RAISE EXCEPTION 'View name, query, and schedule must be provided';
    END IF;

    -- Check if pg_cron is available
    IF NOT EXISTS (SELECT 1 FROM pg_extension WHERE extname = 'pg_cron') THEN
        RAISE EXCEPTION 'pg_cron extension is required but not installed. Run CREATE EXTENSION pg_cron;';
    END IF;

    -- Parse schema and view name if qualified
    SELECT schemaname, objectname INTO _view_schema, _view_unqualified_name
    FROM pg_catalog.pg_identify_object('pg_class'::regclass, _view_name::regclass::oid, 0);

    IF _view_schema IS NULL THEN
        _view_schema := current_schema();
        _view_unqualified_name := _view_name;
    END IF;

    -- Create the materialized view
    RAISE NOTICE 'Creating materialized view %.%', quote_ident(_view_schema), quote_ident(_view_unqualified_name);
    EXECUTE format('CREATE MATERIALIZED VIEW %I.%I AS %s', _view_schema, _view_unqualified_name, _aggregate_query);

    -- Schedule the refresh job using pg_cron
    -- Recommend REFRESH CONCURRENTLY, which requires a UNIQUE index on the view
    _refresh_command := format('REFRESH MATERIALIZED VIEW %I.%I;', _view_schema, _view_unqualified_name);
    RAISE NOTICE 'Scheduling refresh job "%" with command: %', _job_name, _refresh_command;

    -- Check if job already exists (pg_cron allows duplicate names, but let's avoid it)
    SELECT EXISTS (SELECT 1 FROM cron.job WHERE jobname = _job_name) INTO _job_exists;
    IF _job_exists THEN
        RAISE NOTICE 'Job "%" already exists, unscheduling before creating new one.', _job_name;
        PERFORM cron.unschedule(_job_name);
    END IF;

    -- Schedule the new job
    PERFORM cron.schedule(_job_name, _refresh_schedule, _refresh_command);

    RAISE NOTICE 'Created materialized view %I.%I and scheduled refresh job "%" with schedule "%"',
                 _view_schema, _view_unqualified_name, _job_name, _refresh_schedule;
    RAISE NOTICE 'For CONCURRENT refresh, create a UNIQUE index on the materialized view.';

END;
$$;

COMMENT ON FUNCTION pgtime.create_continuous_aggregate(NAME, TEXT, TEXT)
IS 'Creates a materialized view and schedules a pg_cron job to refresh it.';