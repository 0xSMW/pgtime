# pgtime PostgreSQL Extension

A PostgreSQL extension providing simplified time-series utilities.

## Description

`pgtime` aims to simplify common tasks associated with time-series data in PostgreSQL. It includes features such as:

*   **Automatic Time-Based Partitioning:** Helper functions to manage time-partitioned tables.
*   **Time-Series Aggregates:** Custom aggregate functions useful for time-series analysis (e.g., gap-filling).
*   **Background Worker:** A background worker process (details on its specific function TBD, possibly for maintenance or continuous aggregation).

## Dependencies

*   PostgreSQL (version compatibility TBD - requires development libraries for building)
*   `pg_cron` extension (required for certain features, like scheduled aggregation helpers)

## Installation

### Prerequisites

Ensure you have PostgreSQL installed, along with its development headers (e.g., `postgresql-server-dev-XX` on Debian/Ubuntu, `postgresql-devel` on RHEL/CentOS). You also need the `pg_cron` extension installed and enabled in your target database.

### Build & Install

1.  Make sure `pg_config` is in your `PATH`.
2.  Clone the repository (if applicable).
3.  Navigate to the extension's source directory.
4.  Run the standard PGXS build commands:
    ```bash
    make
    sudo make install
    ```

### Database Setup

1.  **Enable `pg_cron`:** If not already done, run `CREATE EXTENSION IF NOT EXISTS pg_cron;` in your database.
2.  **Enable `pgtime`:** Connect to your target database as a superuser and run:
    ```sql
    CREATE EXTENSION pgtime;
    ```
3.  **Configure Server:** Add `pgtime` to the `shared_preload_libraries` setting in your `postgresql.conf` file:
    ```ini
    shared_preload_libraries = 'pg_cron,pgtime' # Add pgtime, preserving existing entries
    ```
4.  **Restart PostgreSQL:** A server restart is required for the changes in `shared_preload_libraries` to take effect.

## Usage

This extension provides SQL functions and potentially custom types/operators.

*   Explore the functions and objects created by the extension by examining the `pgtime--0.1.sql` file.
*   Refer to the regression tests (e.g., `test/sql/basic_usage.sql` and `test/expected/basic_usage.out` if present) for practical examples.

## Contributing

Contributions are welcome! Please refer to the contribution guidelines (TODO: Add link or details).

## License

(TODO: Add license information here) 