#include "postgres.h"

// Required background worker headers
#include "miscadmin.h"          // MyBgworkerEntry, IsBackgroundWorker
#include "postmaster/bgworker.h" // BackgroundWorker structure, RegisterBackgroundWorker
#include "storage/ipc.h"        // Defines for various IPC mechanisms (Latch)
#include "storage/latch.h"      // WaitLatch, SetLatch
#include "storage/lwlock.h"     // Lightweight locks (if needed for shared memory)
#include "storage/proc.h"       // MyProc
#include "storage/shmem.h"      // Shared memory access (if needed)

// Database connection
#include "access/xact.h"        // StartTransactionCommand, CommitTransactionCommand
#include "executor/spi.h"       // Server Programming Interface for running SQL queries
#include "libpq/pqsignal.h"     // Signal handling
#include "pgstat.h"             // pgstat_report_appname
#include "utils/builtins.h"     // Text functions, etc.
#include "utils/guc.h"          // For defining custom GUCs
#include "utils/snapmgr.h"      // Snapshot management for transactions
#include "utils/timestamp.h"    // Timestamp functions
#include "tcop/utility.h"       // ProcessUtility hook (if needed)

PG_MODULE_MAGIC;

// Function prototypes
void _PG_init(void);
void pgtime_worker_main(Datum main_arg) pg_attribute_noreturn();

// Global variables for signal handling and latch
static volatile sig_atomic_t got_sigterm = false;
static Latch *MyLatch = NULL; // Use MyBgworkerEntry->bgw_latch after registration

/* Signal handler for SIGTERM */
static void
worker_sigterm_handler(SIGNAL_ARGS)
{
    int save_errno = errno;
    got_sigterm = true;
    if (MyLatch)
        SetLatch(MyLatch); // Wake up the worker loop
    errno = save_errno;
}

/* Signal handler for SIGHUP (optional, for config reload) */
static void
worker_sighup_handler(SIGNAL_ARGS)
{
    // Process configuration changes if needed
    // ProcessConfigFile(PGC_SIGHUP);
    if (MyLatch)
        SetLatch(MyLatch); // Wake up to potentially reload config
}

/* Entry point of the library loading */
void
_PG_init(void)
{
    BackgroundWorker worker;

    // Don't register if not loaded via shared_preload_libraries
    if (!process_shared_preload_libraries_in_progress)
    {
        elog(DEBUG1, "pgtime: not registering worker because not loaded via shared_preload_libraries");
        return;
    }

    // Define the background worker properties
    MemSet(&worker, 0, sizeof(BackgroundWorker));
    worker.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
    worker.bgw_start_time = BgWorkerStart_RecoveryFinished; // Start after DB is ready
    worker.bgw_restart_time = 30; // Restart after 30 seconds if it crashes
    snprintf(worker.bgw_library_name, BGW_MAXLEN, "pgtime");
    snprintf(worker.bgw_function_name, BGW_MAXLEN, "pgtime_worker_main");
    snprintf(worker.bgw_name, BGW_MAXLEN, "MyTimeseries Maintenance Worker");
    snprintf(worker.bgw_type, BGW_MAXLEN, "MyTimeseries Worker");
    worker.bgw_main_arg = Int32GetDatum(0); // Optional argument
    worker.bgw_notify_pid = 0; // No specific process to notify on start

    // Register the worker
    RegisterBackgroundWorker(&worker);
    elog(LOG, "pgtime: background worker registered");
}

/* Main loop of the background worker */
void
pgtime_worker_main(Datum main_arg)
{
    char *dbname = "postgres"; // TODO: Make this configurable (GUC?)
    char *appname = "MyTimeseries Worker";
    long wait_timeout_ms = 300 * 1000L; // 5 minutes TODO: Make configurable (GUC?)

    // Setup signal handlers
    pqsignal(SIGTERM, worker_sigterm_handler);
    pqsignal(SIGHUP, worker_sighup_handler); // Optional: handle SIGHUP for config reload
    BackgroundWorkerUnblockSignals();

    // Initialize latch reference AFTER registration
    MyLatch = &MyBgworkerEntry->bgw_latch;

    // Connect to the database
    elog(LOG, "pgtime worker starting connection to database \"%s\"", dbname);
    BackgroundWorkerInitializeConnection(dbname, NULL, 0);
    elog(LOG, "pgtime worker connected successfully");

    // Set application name for monitoring
    pgstat_report_appname(appname);

    // Main loop
    while (!got_sigterm)
    {
        int rc;

        // Wait for latch signal or timeout
        rc = WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH, wait_timeout_ms, PG_WAIT_EXTENSION);
        ResetLatch(MyLatch);

        // Emergency bailout if postmaster died
        if (rc & WL_POSTMASTER_DEATH)
        {
            elog(LOG, "pgtime worker exiting because postmaster died");
            proc_exit(1);
        }

        // Check for SIGTERM again after waking up
        if (got_sigterm)
        {
            elog(LOG, "pgtime worker received SIGTERM, shutting down.");
            break;
        }

        // --- Start Maintenance Work ---
        elog(LOG, "pgtime worker starting maintenance cycle");
        SetCurrentStatementStartTimestamp();
        StartTransactionCommand();
        // Ensure we have a snapshot for SPI operations
        PushActiveSnapshot(GetTransactionSnapshot());
        SPI_connect();

        // TODO: Implement maintenance logic here
        // 1. Query pgtime.tables metadata table
        //    const char *query = "SELECT parent_table::text, time_column, partition_interval, retention_interval, compression_interval FROM pgtime.tables;";
        //    SPI_execute(query, true, 0);
        //    if (SPI_processed > 0) { ... loop through results ... }

        // 2. For each table:
        //    - Check and create future partitions (e.g., 1-2 intervals ahead)
        //      - Calculate required partition bounds
        //      - Check if partition exists (e.g., query pg_inherits or pg_partitions)
        //      - If not exists: SPI_execute(CREATE TABLE ... PARTITION OF ...)
        //      - Update metadata (e.g., last_partition_created_at)
        //
        //    - Check and apply retention policy
        //      - Identify partitions older than retention_interval
        //      - If found: SPI_execute(DETACH PARTITION ...), SPI_execute(DROP TABLE ...)
        //
        //    - Check and apply compression policy (PG16+)
        //      - Identify partitions older than compression_interval (but newer than retention)
        //      - If found and not compressed: SPI_execute(ALTER TABLE ... ALTER COLUMN ... SET COMPRESSION ...)
        //      - Schedule VACUUM (FREEZE, ANALYZE) if needed

        // Example placeholder log
        elog(LOG, "pgtime worker: Placeholder for partition management, retention, compression logic.");


        // Cleanup SPI and transaction
        SPI_finish();
        PopActiveSnapshot();
        CommitTransactionCommand(); // Use AbortTransactionCommand() on error
        pgstat_report_activity(STATE_IDLE, NULL); // Report idle state
        elog(LOG, "pgtime worker finished maintenance cycle");
        // --- End Maintenance Work ---

        // Handle SIGHUP if needed (e.g., reload config from GUCs)
        if (ConfigReloadPending)
        {
            ConfigReloadPending = false;
            ProcessConfigFile(PGC_SIGHUP);
            elog(LOG, "pgtime worker reloaded configuration on SIGHUP");
            // Update wait_timeout_ms, dbname etc. from GUCs if they changed
        }
    }

    elog(LOG, "pgtime worker exiting cleanly.");
    proc_exit(0);
}