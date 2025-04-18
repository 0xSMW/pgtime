#include "postgres.h"

#include "access/xact.h"
#include "executor/spi.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "tcop/utility.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/snapmgr.h"
#include "utils/timestamp.h"

PG_MODULE_MAGIC;

/* ------------------------------------------------------------------------
 *  Function prototypes
 * ------------------------------------------------------------------------*/
void  _PG_init(void);
void  pgtime_worker_main(Datum main_arg) pg_attribute_noreturn();
static void perform_maintenance(void);

/* ------------------------------------------------------------------------
 *  Globals
 * ------------------------------------------------------------------------*/
static volatile sig_atomic_t got_sigterm = false;
static Latch *MyLatch = NULL;

/* ------------------------------------------------------------------------
 *  Signal handlers
 * ------------------------------------------------------------------------*/
static void
worker_sigterm_handler(SIGNAL_ARGS)
{
    int save_errno = errno;
    got_sigterm = true;
    if (MyLatch)
        SetLatch(MyLatch);
    errno = save_errno;
}

static void
worker_sighup_handler(SIGNAL_ARGS)
{
    int save_errno = errno;
    if (MyLatch)
        SetLatch(MyLatch);
    errno = save_errno;
}

/* ------------------------------------------------------------------------
 *  _PG_init — register background worker
 * ------------------------------------------------------------------------*/
void
_PG_init(void)
{
    BackgroundWorker worker;

    if (!process_shared_preload_libraries_in_progress)
        return;

    MemSet(&worker, 0, sizeof(worker));
    worker.bgw_flags       = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
    worker.bgw_start_time  = BgWorkerStart_RecoveryFinished;
    worker.bgw_restart_time= 30;                       /* seconds */
    snprintf(worker.bgw_library_name,  BGW_MAXLEN, "pgtime");
    snprintf(worker.bgw_function_name, BGW_MAXLEN, "pgtime_worker_main");
    snprintf(worker.bgw_name,          BGW_MAXLEN, "pgtime maintenance worker");
    snprintf(worker.bgw_type,          BGW_MAXLEN, "pgtime");
    worker.bgw_main_arg   = Int32GetDatum(0);
    worker.bgw_notify_pid = 0;

    RegisterBackgroundWorker(&worker);
}

/* ------------------------------------------------------------------------
 *  perform_maintenance — one cycle
 * ------------------------------------------------------------------------*/
static void
perform_maintenance(void)
{
    const char *query =
        "SELECT parent_table::text, time_column, partition_interval, "
        "       retention_interval, compression_interval "
        "FROM   pgtime.tables";

    if (SPI_execute(query, true, 0) != SPI_OK_SELECT)
        elog(ERROR, "pgtime: SPI_execute failed: %s", query);

    for (uint64 i = 0; i < SPI_processed; i++)
    {
        HeapTuple tuple = SPI_tuptable->vals[i];
        Datum     rel_d = SPI_getbinval(tuple, SPI_tuptable->tupdesc, 1, NULL);
        char     *rel   = TextDatumGetCString(rel_d);

        elog(LOG, "pgtime: maintenance would run for table \"%s\"", rel);
    }
}

/* ------------------------------------------------------------------------
 *  pgtime_worker_main
 * ------------------------------------------------------------------------*/
void
pgtime_worker_main(Datum main_arg)
{
    const char *dbname           = "postgres";
    const char *appname          = "pgtime maintenance worker";
    const long  wait_timeout_ms  = 5L * 60L * 1000L;   /* 5 minutes */

    pqsignal(SIGTERM, worker_sigterm_handler);
    pqsignal(SIGHUP,  worker_sighup_handler);
    BackgroundWorkerUnblockSignals();

    MyLatch = &MyBgworkerEntry->bgw_latch;

    BackgroundWorkerInitializeConnection(dbname, NULL, 0);
    pgstat_report_appname(appname);

    while (!got_sigterm)
    {
        int rc = WaitLatch(MyLatch,
                        WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH,
                        wait_timeout_ms,
                        PG_WAIT_EXTENSION);

        ResetLatch(MyLatch);

        if (rc & WL_POSTMASTER_DEATH)
            proc_exit(1);

        if (got_sigterm)
            break;

        /* ----------------------------------------------------
        *  one maintenance tick
        * ----------------------------------------------------*/
        StartTransactionCommand();
        PushActiveSnapshot(GetTransactionSnapshot());
        if (SPI_connect() != SPI_OK_CONNECT)
            elog(ERROR, "pgtime: SPI_connect failed");

        perform_maintenance();

        SPI_finish();
        PopActiveSnapshot();
        CommitTransactionCommand();
    }

    proc_exit(0);
}