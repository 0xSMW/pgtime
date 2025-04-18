# pgtime — PostgreSQL Time‑Series Utilities

`pgtime` is a lightweight extension that helps you manage time‑series data in
PostgreSQL. It provides:

| Feature | Description |
|---------|-------------|
| **Automatic partition maintenance** | A background worker creates future partitions and drops or compresses old ones according to policy. |
| **Gap‑filling helpers** | SQL/C functions such as `time_bucket`, `first`, `last`, and (coming soon) `interpolate`. |
| **Continuous aggregates** | A wrapper around `pg_cron` to refresh materialized views on a schedule. |

## 1  Building & Installing

```bash
make
sudo make install
```

This copies:
	•	pgtime.so → $libdir
	•	pgtime.control, migration scripts → $SHAREDIR/extension
	•	This document → $DOCDIR/extension

```
Hint: on macOS with Homebrew, $SHAREDIR is
/opt/homebrew/opt/postgresql@<ver>/share/postgresql@<ver>.
```

## 2  Enabling the Extension

```sql
-- One‑time in each database
CREATE EXTENSION IF NOT EXISTS pg_cron;  -- dependency
CREATE EXTENSION pgtime;
```

Edit postgresql.conf and restart so the background worker loads:
```
shared_preload_libraries = 'pgtime,pg_cron'
```

## 3  Managing a Table

```sql
-- Suppose you already have a RANGE‑partitioned hypertable:
CREATE TABLE sensor_data (
    ts   timestamptz NOT NULL,
    id   int         NOT NULL,
    val  double precision
) PARTITION BY RANGE (ts);

Tell pgtime how to look after it:

SELECT pgtime.create_timeseries_table(
    'sensor_data',      -- parent table
    'ts',               -- time column
    interval '1 day',   -- partition width
    interval '12 months',
    interval '6 months'
);
```

The worker will:
- keep two days of partitions ahead,
- drop anything older than one year,
- compress partitions older than six months (PG 16+).

## 4  Gap‑Filling Functions

```sql
-- Bucket timestamps into 5‑minute slots
SELECT time_bucket('5 min', ts) AS bucket, avg(val)
FROM   sensor_data
GROUP  BY bucket
ORDER  BY bucket;

-- First reading per sensor per hour
SELECT id,
       first(val ORDER BY ts) AS first_val
FROM   sensor_data
GROUP  BY id, date_trunc('hour', ts);
```

## 5  Background Worker Logging

Look for messages like:

```
pgtime: maintenance cycle
pgtime: created partition sensor_data_20250419
pgtime: dropped partition sensor_data_20230419
```

in postgresql.log.  Set log_min_messages = info to see them.

## 6  Contributing

Bug reports and pull requests are welcome at
https://github.com/0xSMW/pgtime. Please include:

- PostgreSQL version
- Steps to reproduce
- Relevant log excerpts

Licensed under the MIT License. © 2025 Stephen Walker
