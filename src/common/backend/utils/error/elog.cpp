/* -------------------------------------------------------------------------
 *
 * elog.c
 *	  error logging and reporting
 *
 * Because of the extremely high rate at which log messages can be generated,
 * we need to be mindful of the performance cost of obtaining any information
 * that may be logged.	Also, it's important to keep in mind that this code may
 * get called from within an aborted transaction, in which case operations
 * such as syscache lookups are unsafe.
 *
 * Some notes about recursion and errors during error processing:
 *
 * We need to be robust about recursive-error scenarios --- for example,
 * if we run out of memory, it's important to be able to report that fact.
 * There are a number of considerations that go into this.
 *
 * First, distinguish between re-entrant use and actual recursion.	It
 * is possible for an error or warning message to be emitted while the
 * parameters for an error message are being computed.	In this case
 * errstart has been called for the outer message, and some field values
 * may have already been saved, but we are not actually recursing.	We handle
 * this by providing a (small) stack of ErrorData records.	The inner message
 * can be computed and sent without disturbing the state of the outer message.
 * (If the inner message is actually an error, this isn't very interesting
 * because control won't come back to the outer message generator ... but
 * if the inner message is only debug or log data, this is critical.)
 *
 * Second, actual recursion will occur if an error is reported by one of
 * the elog.c routines or something they call.	By far the most probable
 * scenario of this sort is "out of memory"; and it's also the nastiest
 * to handle because we'd likely also run out of memory while trying to
 * report this error!  Our escape hatch for this case is to reset the
 * ErrorContext to empty before trying to process the inner error.	Since
 * ErrorContext is guaranteed to have at least 8K of space in it (see mcxt.c),
 * we should be able to process an "out of memory" message successfully.
 * Since we lose the prior error state due to the reset, we won't be able
 * to return to processing the original error, but we wouldn't have anyway.
 * (NOTE: the escape hatch is not used for recursive situations where the
 * inner message is of less than ERROR severity; in that case we just
 * try to process it and return normally.  Usually this will work, but if
 * it ends up in infinite recursion, we will PANIC due to error stack
 * overflow.)
 *
 *
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/error/elog.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"
#include "knl/knl_variable.h"

#include <fcntl.h>
#ifdef HAVE_SYSLOG
#include <syslog.h>
#endif

#include "access/transam.h"
#include "access/xact.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "parser/gramparse.h"
#include "parser/parser.h"
#include "postmaster/postmaster.h"
#include "postmaster/syslogger.h"
#include "storage/ipc.h"
#include "storage/proc.h"
#include "tcop/tcopprot.h"
#include "utils/be_module.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/ps_status.h"
#include "utils/selfuncs.h"
#include "auditfuncs.h"
#include "utils/elog.h"
#ifdef PGXC
#include "pgxc/pgxc.h"
#include "pgxc/execRemote.h"
#endif
#include "executor/execStream.h"
#include "executor/executor.h"
#include "workload/workload.h"
#include "../bin/gsqlerr/errmsg.h"
#include "optimizer/randomplan.h"
#include <execinfo.h>

#include "tcop/stmt_retry.h"

#undef _
#define _(x) err_gettext(x)

static const char* err_gettext(const char* str)
    /* This extension allows gcc to check the format string for consistency with
       the supplied arguments. */
    __attribute__((format_arg(1)));

extern bool StreamThreadAmI();
extern bool StreamTopConsumerAmI();
#ifndef ENABLE_LLT
extern void clean_ec_conn();
extern void delete_ec_ctrl();
#endif

#ifdef HAVE_SYSLOG

/*
 * Max string length to send to syslog().  Note that this doesn't count the
 * sequence-number prefix we add, and of course it doesn't count the prefix
 * added by syslog itself.	Solaris and sysklogd truncate the final message
 * at 1024 bytes, so this value leaves 124 bytes for those prefixes.  (Most
 * other syslog implementations seem to have limits of 2KB or so.)
 */
#ifndef PG_SYSLOG_LIMIT
#define PG_SYSLOG_LIMIT 900
#endif

static void write_syslog(int level, const char* line);
#endif

static void write_console(const char* line, int len);

#ifdef WIN32
extern THR_LOCAL char* g_instance.attr.attr_common.event_source;
static void write_eventlog(int level, const char* line, int len);
#endif

/* Macro for checking t_thrd.log_cxt.errordata_stack_depth is reasonable */
#define CHECK_STACK_DEPTH()                                               \
    do {                                                                  \
        if (t_thrd.log_cxt.errordata_stack_depth < 0) {                   \
            t_thrd.log_cxt.errordata_stack_depth = -1;                    \
            ereport(ERROR, (errmsg_internal("errstart was not called"))); \
        }                                                                 \
    } while (0)

static void log_line_prefix(StringInfo buf, ErrorData* edata);
static void send_message_to_server_log(ErrorData* edata);
static void send_message_to_frontend(ErrorData* edata);
static char* expand_fmt_string(const char* fmt, ErrorData* edata);
static const char* useful_strerror(int errnum);
static const char* error_severity(int elevel);
static void append_with_tabs(StringInfo buf, const char* str);
static bool is_log_level_output(int elevel, int log_min_level);
static void write_pipe_chunks(char* data, int len, int dest);
static void write_csvlog(ErrorData* edata);
static void setup_formatted_log_time(void);
static void setup_formatted_start_time(void);
static char* mask_Password_internal(const char* query_string);
static void mask_espaced_character(char* source_str);
static void erase_single_quotes(char* query_string);
static int output_backtrace_to_log(StringInfoData* pOutBuf);

/*
 * in_error_recursion_trouble --- are we at risk of infinite error recursion?
 *
 * This function exists to provide common control of various fallback steps
 * that we take if we think we are facing infinite error recursion.  See the
 * callers for details.
 */
bool in_error_recursion_trouble(void)
{
    /* Pull the plug if recurse more than once */
    return (t_thrd.log_cxt.recursion_depth > 2);
}

/*
 * One of those fallback steps is to stop trying to localize the error
 * message, since there's a significant probability that that's exactly
 * what's causing the recursion.
 */
static inline const char* err_gettext(const char* str)
{
#ifdef ENABLE_NLS
    if (in_error_recursion_trouble())
        return str;
    else
        return gettext(str);
#else
    return str;
#endif
}

/*
 * errstart --- begin an error-reporting cycle
 *
 * Create a stack entry and store the given parameters in it.  Subsequently,
 * errmsg() and perhaps other routines will be called to further populate
 * the stack entry.  Finally, errfinish() will be called to actually process
 * the error report.
 *
 * Returns TRUE in normal case.  Returns FALSE to short-circuit the error
 * report (if it's a warning or lower and not to be reported anywhere).
 */
bool errstart(int elevel, const char* filename, int lineno, const char* funcname, const char* domain)
{
    ErrorData* edata = NULL;
    bool output_to_server = false;
    bool output_to_client = false;
    int i = 0;
    bool verbose = false;

#ifdef ENABLE_UT
    if (t_thrd.log_cxt.disable_log_output)
        return false;
#endif

    /*
     * Check some cases in which we want to promote an error into a more
     * severe error.  None of this logic applies for non-error messages.
     */
    if (elevel >= ERROR) {
        /*
         * If we are inside a critical section, all errors become PANIC
         * errors.	See miscadmin.h.
         */
        if (t_thrd.int_cxt.CritSectionCount > 0)
            elevel = PANIC;

        /*
         * Check reasons for treating ERROR as FATAL:
         *
         * 1. we have no handler to pass the error to (implies we are in the
         * postmaster or in backend startup).
         *
         * 2. u_sess->attr.attr_common.ExitOnAnyError mode switch is set (initdb uses this).
         *
         * 3. the error occurred after proc_exit has begun to run.	(It's
         * proc_exit's responsibility to see that this doesn't turn into
         * infinite recursion!)
         */
        if (elevel == ERROR) {
            if (t_thrd.log_cxt.PG_exception_stack == NULL || t_thrd.proc_cxt.proc_exit_inprogress)
                elevel = FATAL;

            if (u_sess->attr.attr_common.ExitOnAnyError && !AmPostmasterProcess()) {
                /*
                 * The following processes rely on u_sess->attr.attr_common.ExitOnAnyError to terminate successfully,
                 * during which panic is not expected.
                 */
                if (AmCheckpointerProcess() || AmBackgroundWriterProcess() || AmWalReceiverWriterProcess() ||
                    AmDataReceiverWriterProcess())
                    elevel = FATAL;
                else
                    elevel = PANIC;
            }

            if (u_sess->utils_cxt.test_err_type >= 3) {
                int save_type = u_sess->utils_cxt.test_err_type;

                u_sess->utils_cxt.test_err_type = 0;
                force_backtrace_messages = false;
                ereport(ERROR, (errmsg_internal("ERR CONTAINS ERR, %d", save_type)));
            }
        }

        /*
         * If the error level is ERROR or more, errfinish is not going to
         * return to caller; therefore, if there is any stacked error already
         * in progress it will be lost.  This is more or less okay, except we
         * do not want to have a FATAL or PANIC error downgraded because the
         * reporting process was interrupted by a lower-grade error.  So check
         * the stack and make sure we panic if panic is warranted.
         */
        for (i = 0; i <= t_thrd.log_cxt.errordata_stack_depth; i++)
            elevel = Max(elevel, t_thrd.log_cxt.errordata[i].elevel);
    }

    /*
     * Now decide whether we need to process this report at all; if it's
     * warning or less and not enabled for logging, just return FALSE without
     * starting up any error logging machinery.
     *
     * Determine whether message is enabled for server log output 
     */
    if (IsPostmasterEnvironment)
        output_to_server = is_log_level_output(elevel, log_min_messages);
    else
        /* In bootstrap/standalone case, do not sort LOG out-of-order */
        output_to_server = (elevel >= log_min_messages);

    /* Determine whether message is enabled for client output */
    if (t_thrd.postgres_cxt.whereToSendOutput == DestRemote && elevel != COMMERROR) {
        /*
         * client_min_messages is honored only after we complete the
         * authentication handshake.  This is required both for security
         * reasons and because many clients can't handle NOTICE messages
         * during authentication.
         */
        if (u_sess && u_sess->ClientAuthInProgress)
            output_to_client = (elevel >= ERROR);
        else
            output_to_client = (elevel >= client_min_messages || elevel == INFO);
    }

    /* send to client for NOTICE messages in Stream thread */
    if (StreamThreadAmI() && elevel == NOTICE)
        output_to_client = true;

#ifdef ENABLE_QUNIT
    if (u_sess->utils_cxt.qunit_case_number != 0 && elevel >= WARNING)
        output_to_client = true;
#endif

    if (VERBOSEMESSAGE == elevel) {
        output_to_client = true;
        verbose = true;

        /* for CN  elevel is restored to INFO for the coming opetions. */
        if (IS_PGXC_COORDINATOR)
            elevel = INFO;
    }

    if ((AmWLMWorkerProcess() || AmWLMMonitorProcess() || AmWLMArbiterProcess() || AmCPMonitorProcess()) &&
        elevel >= ERROR) {
        output_to_client = false;
    }

    /* Skip processing effort if non-error message will not be output */
    if (elevel < ERROR && !output_to_server && !output_to_client)
        return false;

    /*
     * We need to do some actual work.  Make sure that memory context
     * initialization has finished, else we can't do anything useful.
     */
    if (ErrorContext == NULL) {
        /* Ooops, hard crash time; very little we can do safely here */
        write_stderr("error occurred at %s:%d before error message processing is available\n"
                     " ERRORContext is NULL now! Thread is exiting.\n",
            filename ? filename : "(unknown file)",
            lineno);

        /*
         * Libcomm permanent thread must be not exit,
         * don't allow to call ereport in libcomm thread, abort for generating core file.
         * In other cases, restart process now.
         */
        if (t_thrd.comm_cxt.LibcommThreadType != LIBCOMM_NONE)
            abort();

        if (!IsPostmasterEnvironment || t_thrd.proc_cxt.MyProcPid == PostmasterPid) {
            write_stderr("Gaussdb exit code is 2.\n");
            pg_usleep(1000);
            _exit(2);
        } else {
            /* release the Top memory context */
            force_backtrace_messages = false;
            MemoryContextDestroyAtThreadExit(t_thrd.top_mem_cxt);
            ThreadExitCXX(2);
        }
    }

    /*
     * Okay, crank up a stack entry to store the info in.
     */
    if (t_thrd.log_cxt.recursion_depth++ > 0 && elevel >= ERROR) {
        /*
         * Ooops, error during error processing.  Clear ErrorContext as
         * discussed at top of file.  We will not return to the original
         * error's reporter or handler, so we don't need it.
         */
        MemoryContextReset(ErrorContext);

        /*
         * Infinite error recursion might be due to something broken in a
         * context traceback routine.  Abandon them too.  We also abandon
         * attempting to print the error statement (which, if long, could
         * itself be the source of the recursive failure).
         */
        if (in_error_recursion_trouble()) {
            t_thrd.log_cxt.error_context_stack = NULL;
            t_thrd.postgres_cxt.debug_query_string = NULL;
        }
    }
    if (++t_thrd.log_cxt.errordata_stack_depth >= ERRORDATA_STACK_SIZE) {
        /*
         * Wups, stack not big enough.	We treat this as a PANIC condition
         * because it suggests an infinite loop of errors during error
         * recovery.
         */
        force_backtrace_messages = false;
        t_thrd.log_cxt.errordata_stack_depth = -1; /* make room on stack */

        /* Stack full, abort() directly instead of using erreport which goes to a deadloop */
        t_thrd.int_cxt.ImmediateInterruptOK = false;
        abort();
    }

    /* Initialize data for this error frame */
    edata = &t_thrd.log_cxt.errordata[t_thrd.log_cxt.errordata_stack_depth];
    errno_t rc = memset_s(edata, sizeof(ErrorData), 0, sizeof(ErrorData));
    securec_check(rc, "", "");
    edata->elevel = elevel;
    if (verbose)
        edata->verbose = true;
    edata->output_to_server = output_to_server;
    edata->output_to_client = output_to_client;
    if (filename != NULL) {
        const char* slash = NULL;

        /* keep only base name, useful especially for vpath builds */
        slash = strrchr(filename, '/');
        if (slash != NULL) {
            filename = slash + 1;
        }
    }
    edata->lineno = lineno;
    edata->filename = (char*)filename;
    edata->funcname = (char*)funcname;
    /* the default text domain is the backend's */
    edata->domain = domain ? domain : PG_TEXTDOMAIN("postgres");
    /* Select default errcode based on elevel */
    if (elevel >= ERROR)
        edata->sqlerrcode = ERRCODE_WRONG_OBJECT_TYPE;
    else if (elevel == WARNING)
        edata->sqlerrcode = ERRCODE_WARNING;
    else
        edata->sqlerrcode = ERRCODE_SUCCESSFUL_COMPLETION;
    /* errno is saved here so that error parameter eval can't change it */
    edata->saved_errno = errno;

    /* default module name will be used */
    edata->mod_id = MOD_MAX;
    edata->backtrace_log = NULL;

    t_thrd.log_cxt.recursion_depth--;
    return true;
}

/*
 * errfinish --- end an error-reporting cycle
 *
 * Produce the appropriate error report(s) and pop the error stack.
 *
 * If elevel is ERROR or worse, control does not return to the caller.
 * See elog.h for the error level definitions.
 */
void errfinish(int dummy, ...)
{
    ErrorData* edata = &t_thrd.log_cxt.errordata[t_thrd.log_cxt.errordata_stack_depth];
    ErrorData* producer_save_edata = NULL;
    int elevel = edata->elevel;
    MemoryContext oldcontext;

    ErrorContextCallback* econtext = NULL;

    t_thrd.log_cxt.recursion_depth++;
    force_backtrace_messages = false;

    CHECK_STACK_DEPTH();

    /*
     * If procuer thread save a edata when report ERROR,
     * now top consumer need use the saved edata.
     */
    if (StreamTopConsumerAmI() && u_sess->stream_cxt.global_obj != NULL && elevel >= ERROR) {
        producer_save_edata = u_sess->stream_cxt.global_obj->getProducerEdata();
        /*
         * In executing stream operator, when top consumer's elevel is greater than
         * producer's elevel, we can't update top consumer's elevel, because that operator
         * may decrease top consumer's elevel in some scene.
         */
        if (producer_save_edata != NULL && producer_save_edata->elevel >= elevel) {
            UpdateErrorData(edata, producer_save_edata);
            elevel = edata->elevel;
        }
    }

    /*
     * Do processing in ErrorContext, which we hope has enough reserved space
     * to report an error.
     */
    oldcontext = MemoryContextSwitchTo(ErrorContext);

    /*
     * Call any context callback functions.  Errors occurring in callback
     * functions will be treated as recursive errors --- this ensures we will
     * avoid infinite recursion (see errstart).
     */
    for (econtext = t_thrd.log_cxt.error_context_stack; econtext != NULL; econtext = econtext->previous)
        (*econtext->callback)(econtext->arg);
    /* Database Security: Support database audit */
    /* Audit beyond privileges */
    if (edata->sqlerrcode == ERRCODE_INSUFFICIENT_PRIVILEGE) {
        pgaudit_user_no_privileges(NULL, edata->message);
    }

    /*
     * Because ErrorContext will be reset during FlushErrorState,
     * so we can reset NULL here
     */
    edata->backtrace_log = NULL;

    /* get backtrace info */
    if (edata->elevel >= u_sess->attr.attr_common.backtrace_min_messages) {
        StringInfoData buf;
        initStringInfo(&buf);
        int ret = output_backtrace_to_log(&buf);
        if (0 == ret) {
            edata->backtrace_log = pstrdup(buf.data);
        }
        pfree(buf.data);
    }

#ifdef MEMORY_CONTEXT_CHECKING
    /* Check all memory contexts when there is an error or a fatal */
    if (elevel >= ERROR) {
        MemoryContextCheck(t_thrd.top_mem_cxt, false);
    }
#endif

    /*
     * If ERROR (not more nor less) we pass it off to the current handler.
     * Printing it and popping the stack is the responsibility of the handler.
     */
    if (elevel == ERROR) {
        /*
         * We do some minimal cleanup before longjmp'ing so that handlers can
         * execute in a reasonably sane state.
         *
         *
         * This is just in case the error came while waiting for input
         */
        t_thrd.int_cxt.ImmediateInterruptOK = false;

        /*
         * Reset t_thrd.int_cxt.InterruptHoldoffCount in case we ereport'd from inside an
         * interrupt holdoff section.  (We assume here that no handler will
         * itself be inside a holdoff section.	If necessary, such a handler
         * could save and restore t_thrd.int_cxt.InterruptHoldoffCount for itself, but this
         * should make life easier for most.)
         */
        t_thrd.int_cxt.InterruptHoldoffCount = 0;

        t_thrd.int_cxt.CritSectionCount = 0; /* should be unnecessary, but... */

        /*
         * Note that we leave CurrentMemoryContext set to ErrorContext. The
         * handler should reset it to something else soon.
         */
        t_thrd.log_cxt.recursion_depth--;
        PG_RE_THROW();
    }

    /*
     * If we are doing FATAL or PANIC, abort any old-style COPY OUT in
     * progress, so that we can report the message before dying.  (Without
     * this, pq_putmessage will refuse to send the message at all, which is
     * what we want for NOTICE messages, but not for fatal exits.) This hack
     * is necessary because of poor design of old-style copy protocol.
     */
    if (elevel >= FATAL && t_thrd.postgres_cxt.whereToSendOutput == DestRemote)
        pq_endcopyout(true);

    bool isVerbose = false;
    if (edata->elevel == VERBOSEMESSAGE) {
        edata->elevel = INFO;
        handle_in_client(true);
        isVerbose = true;
    }

    if (StreamThreadAmI() && u_sess->stream_cxt.producer_obj != NULL && elevel == FATAL) {
        /*
         * Just like reportError() in longjump point of StreamMain(),
         * report FATAL error to consumer here.
         */
        ((StreamProducer*)u_sess->stream_cxt.producer_obj)->reportError();
    } else if (StreamThreadAmI() && u_sess->stream_cxt.producer_obj != NULL && elevel < ERROR) {
        /* Send to server log, if enabled */
        if (edata->output_to_server && is_errmodule_enable(edata->elevel, edata->mod_id))
            send_message_to_server_log(edata);

        /* Send to client, if enabled */
        if (edata->output_to_client) {
            /* report NOTICE to consumer here. */
            u_sess->stream_cxt.producer_obj->reportNotice();
        }
    } else {
        /* Emit the message to the right places */
        EmitErrorReport();
    }

#ifdef ENABLE_MULTIPLE_NODES
    if (elevel >= ERROR) {
        clean_ec_conn();
        delete_ec_ctrl();
    }
#endif

    if (isVerbose)
        handle_in_client(false);

    /* Now free up subsidiary data attached to stack entry, and release it */
    if (edata->message)
        pfree(edata->message);
    if (edata->detail)
        pfree(edata->detail);
    if (edata->detail_log)
        pfree(edata->detail_log);
    if (edata->hint)
        pfree(edata->hint);
    if (edata->context)
        pfree(edata->context);
    if (edata->internalquery)
        pfree(edata->internalquery);
    if (edata->backtrace_log)
        pfree(edata->backtrace_log);

    t_thrd.log_cxt.errordata_stack_depth--;

    /* Exit error-handling context */
    MemoryContextSwitchTo(oldcontext);
    t_thrd.log_cxt.recursion_depth--;

    /*
     * Perform error recovery action as specified by elevel.
     */
    if (elevel == FATAL) {
        /*
         * For a FATAL error, we let proc_exit clean up and exit.
         */
        t_thrd.int_cxt.ImmediateInterruptOK = false;

        /*
         * If we just reported a startup failure, the client will disconnect
         * on receiving it, so don't send any more to the client.
         */
        if (t_thrd.log_cxt.PG_exception_stack == NULL && t_thrd.postgres_cxt.whereToSendOutput == DestRemote)
            t_thrd.postgres_cxt.whereToSendOutput = DestNone;

        /*
         * fflush here is just to improve the odds that we get to see the
         * error message, in case things are so hosed that proc_exit crashes.
         * Any other code you might be tempted to add here should probably be
         * in an on_proc_exit or on_shmem_exit callback instead.
         */
        fflush(stdout);
        fflush(stderr);

        /* release operator-level hash table in memory */
        releaseExplainTable();

        if (StreamTopConsumerAmI() && u_sess->debug_query_id != 0) {
            gs_close_all_stream_by_debug_id(u_sess->debug_query_id);
        }

        /*
         * Do normal process-exit cleanup, then return exit code 1 to indicate
         * FATAL termination.  The postmaster may or may not consider this
         * worthy of panic, depending on which subprocess returns it.
         */
        proc_exit(1);
    }

    if (elevel >= PANIC) {
        /*
         * Serious crash time. Postmaster will observe SIGABRT process exit
         * status and kill the other backends too.
         *
         * XXX: what if we are *in* the postmaster?  abort() won't kill our
         * children...
         */
        t_thrd.int_cxt.ImmediateInterruptOK = false;
        fflush(stdout);
        fflush(stderr);
        abort();
    }

    /*
     * We reach here if elevel <= WARNING. OK to return to caller.
     *
     * But check for cancel/die interrupt first --- this is so that the user
     * can stop a query emitting tons of notice or warning messages, even if
     * it's in a loop that otherwise fails to check for interrupts.
     * Just check for interrupts when ignore_interrupt is not set to true.
     *
     * Well, I think CHECK_FOR_INTERRUPTS() here is somewhat _terrible_. Programmers have put elog nearly everywhere in
     * the code, including critical section which should be executed atomically. CHECK_FOR_INTERRUPTS() here have the
     * probability to break such critical section. This will result in unexpected behaviors!
     */
}

/*
 * @Description: set module id for logging
 * @IN id: module id
 * @Return:
 * @See also:
 */
int errmodule(ModuleId id)
{
    ErrorData* edata = &t_thrd.log_cxt.errordata[t_thrd.log_cxt.errordata_stack_depth];

    /* we don't bother incrementing t_thrd.log_cxt.recursion_depth */
    CHECK_STACK_DEPTH();

    /* set module id */
    Assert(VALID_SINGLE_MODULE(id));
    edata->mod_id = id;

    /* return value does not matter */
    return 0;
}

/*
 * errcode --- add SQLSTATE error code to the current error
 *
 * The code is expected to be represented as per MAKE_SQLSTATE().
 */
int errcode(int sqlerrcode)
{
    ErrorData* edata = &t_thrd.log_cxt.errordata[t_thrd.log_cxt.errordata_stack_depth];

    /* we don't bother incrementing t_thrd.log_cxt.recursion_depth */
    CHECK_STACK_DEPTH();

    edata->sqlerrcode = sqlerrcode;

    return 0; /* return value does not matter */
}

/*
 * save error message for history session info
 */
void save_error_message(void)
{
    if (IS_PGXC_DATANODE || IsConnFromCoord() || !u_sess->wlm_cxt->wlm_params.memtrack ||
        t_thrd.wlm_cxt.collect_info->sdetail.msg)
        return;

    START_CRIT_SECTION();
    for (int i = t_thrd.log_cxt.errordata_stack_depth; i >= 0; --i) {
        ErrorData* edata = t_thrd.log_cxt.errordata + i;

        if (edata->elevel >= ERROR) {
            USE_MEMORY_CONTEXT(g_instance.wlm_cxt->query_resource_track_mcxt);

            if (edata->message)
                t_thrd.wlm_cxt.collect_info->sdetail.msg = pstrdup(edata->message);
            else
                t_thrd.wlm_cxt.collect_info->sdetail.msg = pstrdup("missing error text");

            break;
        }
    }
    END_CRIT_SECTION();
}

/*
 * errcode_for_file_access --- add SQLSTATE error code to the current error
 *
 * The SQLSTATE code is chosen based on the saved errno value.	We assume
 * that the failing operation was some type of disk file access.
 *
 * NOTE: the primary error message string should generally include %m
 * when this is used.
 */
int errcode_for_file_access(void)
{
    ErrorData* edata = &t_thrd.log_cxt.errordata[t_thrd.log_cxt.errordata_stack_depth];

    /* we don't bother incrementing t_thrd.log_cxt.recursion_depth */
    CHECK_STACK_DEPTH();

    switch (edata->saved_errno) {
            /* Permission-denied failures */
        case EPERM:  /* Not super-user */
        case EACCES: /* Permission denied */
#ifdef EROFS
        case EROFS: /* Read only file system */
#endif
            edata->sqlerrcode = ERRCODE_INSUFFICIENT_PRIVILEGE;
            break;

            /* File not found */
        case ENOENT: /* No such file or directory */
            edata->sqlerrcode = ERRCODE_UNDEFINED_FILE;
            break;

            /* Duplicate file */
        case EEXIST: /* File exists */
            edata->sqlerrcode = ERRCODE_DUPLICATE_FILE;
            break;

            /* Wrong object type or state */
        case ENOTDIR:                           /* Not a directory */
        case EISDIR:                            /* Is a directory */
#if defined(ENOTEMPTY) && (ENOTEMPTY != EEXIST) /* same code on AIX */
        case ENOTEMPTY:                         /* Directory not empty */
#endif
            edata->sqlerrcode = ERRCODE_WRONG_OBJECT_TYPE;
            break;

            /* Insufficient resources */
        case ENOSPC: /* No space left on device */
            edata->sqlerrcode = ERRCODE_DISK_FULL;
            break;

        case ENFILE: /* File table overflow */
        case EMFILE: /* Too many open files */
            edata->sqlerrcode = ERRCODE_INSUFFICIENT_RESOURCES;
            break;

            /* Hardware failure */
        case EIO: /* I/O error */
            edata->sqlerrcode = ERRCODE_IO_ERROR;
            break;

            /* All else is classified as internal errors */
        default:
            edata->sqlerrcode = ERRCODE_WRONG_OBJECT_TYPE;
            break;
    }

    return 0; /* return value does not matter */
}

/*
 * errcode_for_socket_access --- add SQLSTATE error code to the current error
 *
 * The SQLSTATE code is chosen based on the saved errno value.	We assume
 * that the failing operation was some type of socket access.
 *
 * NOTE: the primary error message string should generally include %m
 * when this is used.
 */
int errcode_for_socket_access(void)
{
    ErrorData* edata = &t_thrd.log_cxt.errordata[t_thrd.log_cxt.errordata_stack_depth];

    /* we don't bother incrementing t_thrd.log_cxt.recursion_depth */
    CHECK_STACK_DEPTH();

    switch (edata->saved_errno) {
            /* Loss of connection */
        case EPIPE:
#ifdef ECONNRESET
        case ECONNRESET:
#endif
            edata->sqlerrcode = ERRCODE_CONNECTION_FAILURE;
            break;

            /* All else is classified as internal errors */
        default:
            edata->sqlerrcode = ERRCODE_WRONG_OBJECT_TYPE;
            break;
    }

    return 0; /* return value does not matter */
}

/*
 * This macro handles expansion of a format string and associated parameters;
 * it's common code for errmsg(), errdetail(), etc.  Must be called inside
 * a routine that is declared like "const char *fmt, ..." and has an edata
 * pointer set up.	The message is assigned to edata->targetfield, or
 * appended to it if appendval is true.  The message is subject to translation
 * if translateit is true.
 *
 * Note: we pstrdup the buffer rather than just transferring its storage
 * to the edata field because the buffer might be considerably larger than
 * really necessary.
 */
#define EVALUATE_MESSAGE(targetfield, appendval, translateit)           \
    {                                                                   \
        char* fmtbuf = NULL;                                            \
        StringInfoData buf;                                             \
        /* Internationalize the error format string */                  \
        if ((translateit) && !in_error_recursion_trouble())             \
            fmt = dgettext(edata->domain, fmt);                         \
        /* Expand %m in format string */                                \
        fmtbuf = expand_fmt_string(fmt, edata);                         \
        initStringInfo(&buf);                                           \
        if ((appendval) && edata->targetfield) {                        \
            appendStringInfoString(&buf, edata->targetfield);           \
            appendStringInfoChar(&buf, '\n');                           \
        }                                                               \
        /* Generate actual output --- have to use appendStringInfoVA */ \
        for (; ; ) {                                                      \
            va_list args;                                               \
            bool success = false;                                       \
            va_start(args, fmt);                                        \
            success = appendStringInfoVA(&buf, fmtbuf, args);           \
            va_end(args);                                               \
            if (success)                                                \
                break;                                                  \
            enlargeStringInfo(&buf, buf.maxlen);                        \
        }                                                               \
        /* Done with expanded fmt */                                    \
        pfree(fmtbuf);                                                  \
        /* Save the completed message into the stack item */            \
        if (edata->targetfield)                                         \
            pfree(edata->targetfield);                                  \
        edata->targetfield = pstrdup(buf.data);                         \
        pfree(buf.data);                                                \
    }

/*
 * Same as above, except for pluralized error messages.  The calling routine
 * must be declared like "const char *fmt_singular, const char *fmt_plural,
 * unsigned long n, ...".  Translation is assumed always wanted.
 */
#define EVALUATE_MESSAGE_PLURAL(targetfield, appendval)                  \
    {                                                                    \
        const char* fmt = NULL;                                          \
        char* fmtbuf = NULL;                                             \
        StringInfoData buf;                                              \
        /* Internationalize the error format string */                   \
        if (!in_error_recursion_trouble())                               \
            fmt = dngettext(edata->domain, fmt_singular, fmt_plural, n); \
        else                                                             \
            fmt = (n == 1 ? fmt_singular : fmt_plural);                  \
        /* Expand %m in format string */                                 \
        fmtbuf = expand_fmt_string(fmt, edata);                          \
        initStringInfo(&buf);                                            \
        if ((appendval) && edata->targetfield) {                         \
            appendStringInfoString(&buf, edata->targetfield);            \
            appendStringInfoChar(&buf, '\n');                            \
        }                                                                \
        /* Generate actual output --- have to use appendStringInfoVA */  \
        for (; ; ) {                                                       \
            va_list args;                                                \
            bool success = false;                                        \
            va_start(args, n);                                           \
            success = appendStringInfoVA(&buf, fmtbuf, args);            \
            va_end(args);                                                \
            if (success)                                                 \
                break;                                                   \
            enlargeStringInfo(&buf, buf.maxlen);                         \
        }                                                                \
        /* Done with expanded fmt */                                     \
        pfree(fmtbuf);                                                   \
        /* Save the completed message into the stack item */             \
        if (edata->targetfield)                                          \
            pfree(edata->targetfield);                                   \
        edata->targetfield = pstrdup(buf.data);                          \
        pfree(buf.data);                                                 \
    }

/*
 * errmsg --- add a primary error message text to the current error
 *
 * In addition to the usual %-escapes recognized by printf, "%m" in
 * fmt is replaced by the error message for the caller's value of errno.
 *
 * Note: no newline is needed at the end of the fmt string, since
 * ereport will provide one for the output methods that need it.
 */
int errmsg(const char* fmt, ...)
{
    ErrorData* edata = &t_thrd.log_cxt.errordata[t_thrd.log_cxt.errordata_stack_depth];
    MemoryContext oldcontext;

    t_thrd.log_cxt.recursion_depth++;
    CHECK_STACK_DEPTH();
    oldcontext = MemoryContextSwitchTo(ErrorContext);

    EVALUATE_MESSAGE(message, false, true);

    MemoryContextSwitchTo(oldcontext);
    t_thrd.log_cxt.recursion_depth--;
    return 0; /* return value does not matter */
}

/*
 * errmsg_internal --- add a primary error message text to the current error
 *
 * This is exactly like errmsg() except that strings passed to errmsg_internal
 * are not translated, and are customarily left out of the
 * internationalization message dictionary.  This should be used for "can't
 * happen" cases that are probably not worth spending translation effort on.
 * We also use this for certain cases where we *must* not try to translate
 * the message because the translation would fail and result in infinite
 * error recursion.
 */
int errmsg_internal(const char* fmt, ...)
{
    ErrorData* edata = &t_thrd.log_cxt.errordata[t_thrd.log_cxt.errordata_stack_depth];
    MemoryContext oldcontext;

    t_thrd.log_cxt.recursion_depth++;
    CHECK_STACK_DEPTH();
    oldcontext = MemoryContextSwitchTo(ErrorContext);

    EVALUATE_MESSAGE(message, false, false);

    MemoryContextSwitchTo(oldcontext);
    t_thrd.log_cxt.recursion_depth--;
    return 0; /* return value does not matter */
}

/*
 * errmsg_plural --- add a primary error message text to the current error,
 * with support for pluralization of the message text
 */
int errmsg_plural(const char* fmt_singular, const char* fmt_plural, unsigned long n, ...)
{
    ErrorData* edata = &t_thrd.log_cxt.errordata[t_thrd.log_cxt.errordata_stack_depth];
    MemoryContext oldcontext;

    t_thrd.log_cxt.recursion_depth++;
    CHECK_STACK_DEPTH();
    oldcontext = MemoryContextSwitchTo(ErrorContext);

    EVALUATE_MESSAGE_PLURAL(message, false);

    MemoryContextSwitchTo(oldcontext);
    t_thrd.log_cxt.recursion_depth--;
    return 0; /* return value does not matter */
}

/*
 * errdetail --- add a detail error message text to the current error
 */
int errdetail(const char* fmt, ...)
{
    ErrorData* edata = &t_thrd.log_cxt.errordata[t_thrd.log_cxt.errordata_stack_depth];
    MemoryContext oldcontext;

    t_thrd.log_cxt.recursion_depth++;
    CHECK_STACK_DEPTH();
    oldcontext = MemoryContextSwitchTo(ErrorContext);

    EVALUATE_MESSAGE(detail, false, true);

    MemoryContextSwitchTo(oldcontext);
    t_thrd.log_cxt.recursion_depth--;
    return 0; /* return value does not matter */
}

/*
 * errdetail_internal --- add a detail error message text to the current error
 *
 * This is exactly like errdetail() except that strings passed to
 * errdetail_internal are not translated, and are customarily left out of the
 * internationalization message dictionary.  This should be used for detail
 * messages that seem not worth translating for one reason or another
 * (typically, that they don't seem to be useful to average users).
 */
int errdetail_internal(const char* fmt, ...)
{
    ErrorData* edata = &t_thrd.log_cxt.errordata[t_thrd.log_cxt.errordata_stack_depth];
    MemoryContext oldcontext;

    t_thrd.log_cxt.recursion_depth++;
    CHECK_STACK_DEPTH();
    oldcontext = MemoryContextSwitchTo(ErrorContext);

    EVALUATE_MESSAGE(detail, false, false);

    MemoryContextSwitchTo(oldcontext);
    t_thrd.log_cxt.recursion_depth--;
    return 0; /* return value does not matter */
}

/*
 * errdetail_log --- add a detail_log error message text to the current error
 */
int errdetail_log(const char* fmt, ...)
{
    ErrorData* edata = &t_thrd.log_cxt.errordata[t_thrd.log_cxt.errordata_stack_depth];
    MemoryContext oldcontext;

    t_thrd.log_cxt.recursion_depth++;
    CHECK_STACK_DEPTH();
    oldcontext = MemoryContextSwitchTo(ErrorContext);

    EVALUATE_MESSAGE(detail_log, false, true);

    MemoryContextSwitchTo(oldcontext);
    t_thrd.log_cxt.recursion_depth--;
    return 0; /* return value does not matter */
}

/*
 * errdetail_plural --- add a detail error message text to the current error,
 * with support for pluralization of the message text
 */
int errdetail_plural(const char* fmt_singular, const char* fmt_plural, unsigned long n, ...)
{
    ErrorData* edata = &t_thrd.log_cxt.errordata[t_thrd.log_cxt.errordata_stack_depth];
    MemoryContext oldcontext;

    t_thrd.log_cxt.recursion_depth++;
    CHECK_STACK_DEPTH();
    oldcontext = MemoryContextSwitchTo(ErrorContext);

    EVALUATE_MESSAGE_PLURAL(detail, false);

    MemoryContextSwitchTo(oldcontext);
    t_thrd.log_cxt.recursion_depth--;
    return 0; /* return value does not matter */
}

/*
 * errhint --- add a hint error message text to the current error
 */
int errhint(const char* fmt, ...)
{
    ErrorData* edata = &t_thrd.log_cxt.errordata[t_thrd.log_cxt.errordata_stack_depth];
    MemoryContext oldcontext;

    t_thrd.log_cxt.recursion_depth++;
    CHECK_STACK_DEPTH();
    oldcontext = MemoryContextSwitchTo(ErrorContext);

    EVALUATE_MESSAGE(hint, false, true);

    MemoryContextSwitchTo(oldcontext);
    t_thrd.log_cxt.recursion_depth--;
    return 0; /* return value does not matter */
}

/*
 * errquery --- add a query error message text to the current error
 */
int errquery(const char* fmt, ...)
{
    ErrorData* edata = &t_thrd.log_cxt.errordata[t_thrd.log_cxt.errordata_stack_depth];
    MemoryContext oldcontext;

    t_thrd.log_cxt.recursion_depth++;
    CHECK_STACK_DEPTH();
    oldcontext = MemoryContextSwitchTo(ErrorContext);

    EVALUATE_MESSAGE(internalquery, false, true);

    MemoryContextSwitchTo(oldcontext);
    t_thrd.log_cxt.recursion_depth--;
    return 0; /* return value does not matter */
}

/*
 * errcontext --- add a context error message text to the current error
 *
 * Unlike other cases, multiple calls are allowed to build up a stack of
 * context information.  We assume earlier calls represent more-closely-nested
 * states.
 */
int errcontext(const char* fmt, ...)
{
    ErrorData* edata = &t_thrd.log_cxt.errordata[t_thrd.log_cxt.errordata_stack_depth];
    MemoryContext oldcontext;

    t_thrd.log_cxt.recursion_depth++;
    CHECK_STACK_DEPTH();
    oldcontext = MemoryContextSwitchTo(ErrorContext);

    EVALUATE_MESSAGE(context, true, true);

    MemoryContextSwitchTo(oldcontext);
    t_thrd.log_cxt.recursion_depth--;
    return 0; /* return value does not matter */
}

/*
 * errhidestmt --- optionally suppress STATEMENT: field of log entry
 *
 * This should be called if the message text already includes the statement.
 */
int errhidestmt(bool hide_stmt)
{
    ErrorData* edata = &t_thrd.log_cxt.errordata[t_thrd.log_cxt.errordata_stack_depth];

    /* we don't bother incrementing t_thrd.log_cxt.recursion_depth */
    CHECK_STACK_DEPTH();

    edata->hide_stmt = hide_stmt;

    return 0; /* return value does not matter */
}

/*
 * errposition --- add cursor position to the current error
 */
int errposition(int cursorpos)
{
    ErrorData* edata = &t_thrd.log_cxt.errordata[t_thrd.log_cxt.errordata_stack_depth];

    /* we don't bother incrementing t_thrd.log_cxt.recursion_depth */
    CHECK_STACK_DEPTH();

    edata->cursorpos = cursorpos;

    return 0; /* return value does not matter */
}

/*
 * internalerrposition --- add internal cursor position to the current error
 */
int internalerrposition(int cursorpos)
{
    ErrorData* edata = &t_thrd.log_cxt.errordata[t_thrd.log_cxt.errordata_stack_depth];

    /* we don't bother incrementing t_thrd.log_cxt.recursion_depth */
    CHECK_STACK_DEPTH();

    edata->internalpos = cursorpos;

    return 0; /* return value does not matter */
}

/*
 * internalerrquery --- add internal query text to the current error
 *
 * Can also pass NULL to drop the internal query text entry.  This case
 * is intended for use in error callback subroutines that are editorializing
 * on the layout of the error report.
 */
int internalerrquery(const char* query)
{
    ErrorData* edata = &t_thrd.log_cxt.errordata[t_thrd.log_cxt.errordata_stack_depth];

    /* we don't bother incrementing t_thrd.log_cxt.recursion_depth */
    CHECK_STACK_DEPTH();

    if (edata->internalquery) {
        pfree(edata->internalquery);
        edata->internalquery = NULL;
    }

    if (query != NULL) {
        edata->internalquery = MemoryContextStrdup(ErrorContext, query);
    }

    return 0; /* return value does not matter */
}

/*
 * geterrcode --- return the currently set SQLSTATE error code
 *
 * This is only intended for use in error callback subroutines, since there
 * is no other place outside elog.c where the concept is meaningful.
 */
int geterrcode(void)
{
    ErrorData* edata = &t_thrd.log_cxt.errordata[t_thrd.log_cxt.errordata_stack_depth];

    /* we don't bother incrementing t_thrd.log_cxt.recursion_depth */
    CHECK_STACK_DEPTH();

    return edata->sqlerrcode;
}

/*
 * geterrposition --- return the currently set error position (0 if none)
 *
 * This is only intended for use in error callback subroutines, since there
 * is no other place outside elog.c where the concept is meaningful.
 */
int geterrposition(void)
{
    ErrorData* edata = &t_thrd.log_cxt.errordata[t_thrd.log_cxt.errordata_stack_depth];

    /* we don't bother incrementing t_thrd.log_cxt.recursion_depth */
    CHECK_STACK_DEPTH();

    return edata->cursorpos;
}

/*
 * getinternalerrposition --- same for internal error position
 *
 * This is only intended for use in error callback subroutines, since there
 * is no other place outside elog.c where the concept is meaningful.
 */
int getinternalerrposition(void)
{
    ErrorData* edata = &t_thrd.log_cxt.errordata[t_thrd.log_cxt.errordata_stack_depth];

    /* we don't bother incrementing t_thrd.log_cxt.recursion_depth */
    CHECK_STACK_DEPTH();

    return edata->internalpos;
}

/*
 * handle_in_client --- mark if the message should be sent and handled in client
 */
int handle_in_client(bool handle)
{
    ErrorData* edata = &t_thrd.log_cxt.errordata[t_thrd.log_cxt.errordata_stack_depth];

    /* we don't bother incrementing t_thrd.log_cxt.recursion_depth */
    CHECK_STACK_DEPTH();

    edata->handle_in_client = handle;

    return 0; /* return value does not matter */
}

/*
 * ignore_interrupt --- mark if should ignore interrupt when writing server log
 */
int ignore_interrupt(bool ignore)
{
    ErrorData* edata = &t_thrd.log_cxt.errordata[t_thrd.log_cxt.errordata_stack_depth];

    /* we don't bother incrementing t_thrd.log_cxt.recursion_depth */
    CHECK_STACK_DEPTH();

    edata->ignore_interrupt = ignore;

    return 0; /* return value does not matter */
}

/*
 * elog_start --- startup for old-style API
 *
 * All that we do here is stash the hidden filename/lineno/funcname
 * arguments into a stack entry.
 *
 * We need this to be separate from elog_finish because there's no other
 * portable way to deal with inserting extra arguments into the elog call.
 * (If macros with variable numbers of arguments were portable, it'd be
 * easy, but they aren't.)
 */
void elog_start(const char* filename, int lineno, const char* funcname)
{
    ErrorData* edata = NULL;

#ifdef ENABLE_UT
    if (t_thrd.log_cxt.disable_log_output)
        return;
#endif

    /* Make sure that memory context initialization has finished */
    if (ErrorContext == NULL) {
        /* Ooops, hard crash time; very little we can do safely here */
        write_stderr("error occurred at %s:%d before error message processing is available\n",
            filename ? filename : "(unknown file)",
            lineno);
        pg_usleep(1000);
        _exit(2);
    }

    if (++t_thrd.log_cxt.errordata_stack_depth >= ERRORDATA_STACK_SIZE) {
        /*
         * Wups, stack not big enough.	We treat this as a PANIC condition
         * because it suggests an infinite loop of errors during error
         * recovery.  Note that the message is intentionally not localized,
         * else failure to convert it to client encoding could cause further
         * recursion.
         */
        t_thrd.log_cxt.errordata_stack_depth = -1; /* make room on stack */
        ereport(PANIC, (errmsg_internal("ERRORDATA_STACK_SIZE exceeded")));
    }

    edata = &t_thrd.log_cxt.errordata[t_thrd.log_cxt.errordata_stack_depth];
    if (filename != NULL) {
        const char* slash = NULL;

        /* keep only base name, useful especially for vpath builds */
        slash = strrchr(filename, '/');
        if (slash != NULL) {
            filename = slash + 1;
        }
    }
    edata->filename = (char*)filename;
    edata->lineno = lineno;
    edata->funcname = (char*)funcname;
    /* errno is saved now so that error parameter eval can't change it */
    edata->saved_errno = errno;
    edata->backtrace_log = NULL;
}

/*
 * elog_finish --- finish up for old-style API
 */
void elog_finish(int elevel, const char* fmt, ...)
{
    ErrorData* edata = &t_thrd.log_cxt.errordata[t_thrd.log_cxt.errordata_stack_depth];
    MemoryContext oldcontext;

#ifdef ENABLE_UT
    if (t_thrd.log_cxt.disable_log_output)
        return;
#endif

    CHECK_STACK_DEPTH();

    /*
     * Do errstart() to see if we actually want to report the message.
     */
    t_thrd.log_cxt.errordata_stack_depth--;
    errno = edata->saved_errno;
    if (!errstart(elevel, edata->filename, edata->lineno, edata->funcname, NULL))
        return; /* nothing to do */

    /*
     * Format error message just like errmsg_internal().
     */
    t_thrd.log_cxt.recursion_depth++;
    oldcontext = MemoryContextSwitchTo(ErrorContext);

    EVALUATE_MESSAGE(message, false, false);

    MemoryContextSwitchTo(oldcontext);
    t_thrd.log_cxt.recursion_depth--;

    /*
     * And let errfinish() finish up.
     */
    errfinish(0);
}

/*
 * Functions to allow construction of error message strings separately from
 * the ereport() call itself.
 *
 * The expected calling convention is
 *
 *	pre_format_elog_string(errno, domain), var = format_elog_string(format,...)
 *
 * which can be hidden behind a macro such as GUC_check_errdetail().  We
 * assume that any functions called in the arguments of format_elog_string()
 * cannot result in re-entrant use of these functions --- otherwise the wrong
 * text domain might be used, or the wrong errno substituted for %m.  This is
 * okay for the current usage with GUC check hooks, but might need further
 * effort someday.
 *
 * The result of format_elog_string() is stored in ErrorContext, and will
 * therefore survive until FlushErrorState() is called.
 */
void pre_format_elog_string(int errnumber, const char* domain)
{
    /* Save errno before evaluation of argument functions can change it */
    t_thrd.log_cxt.save_format_errnumber = errnumber;
    /* Save caller's text domain */
    t_thrd.log_cxt.save_format_domain = domain;
}

char* format_elog_string(const char* fmt, ...)
{
    ErrorData errdata;
    ErrorData* edata = NULL;
    MemoryContext oldcontext;

    /* Initialize a mostly-dummy error frame */
    edata = &errdata;
    errno_t rc = memset_s(edata, sizeof(ErrorData), 0, sizeof(ErrorData));
    securec_check(rc, "", "");
    /* the default text domain is the backend's */
    edata->domain = t_thrd.log_cxt.save_format_domain ? t_thrd.log_cxt.save_format_domain : PG_TEXTDOMAIN("postgres");
    /* set the errno to be used to interpret %m */
    edata->saved_errno = t_thrd.log_cxt.save_format_errnumber;

    oldcontext = MemoryContextSwitchTo(ErrorContext);

    EVALUATE_MESSAGE(message, false, true);

    MemoryContextSwitchTo(oldcontext);

    return edata->message;
}

/*
 * Actual output of the top-of-stack error message
 *
 * In the ereport(ERROR) case this is called from PostgresMain (or not at all,
 * if the error is caught by somebody).  For all other severity levels this
 * is called by errfinish.
 */
void EmitErrorReport(void)
{
    ErrorData* edata = &t_thrd.log_cxt.errordata[t_thrd.log_cxt.errordata_stack_depth];
    MemoryContext oldcontext;

    t_thrd.log_cxt.recursion_depth++;
    CHECK_STACK_DEPTH();
    oldcontext = MemoryContextSwitchTo(ErrorContext);

    /* Send to server log, if enabled */
    if (edata->output_to_server && is_errmodule_enable(edata->elevel, edata->mod_id))
        send_message_to_server_log(edata);

    /* Send to client, if enabled */
    if (edata->output_to_client) {
        bool need_skip_by_retry = IsStmtRetryAvaliable(edata->elevel, edata->sqlerrcode);
        bool can_skip = (edata->elevel < FATAL);
        if (can_skip && need_skip_by_retry) {
            /* skip sending messsage to front, do noting for now */
        } else {
            send_message_to_frontend(edata);
        }
    }

    MemoryContextSwitchTo(oldcontext);
    t_thrd.log_cxt.recursion_depth--;
}

/*
 * CopyErrorData --- obtain a copy of the topmost error stack entry
 *
 * This is only for use in error handler code.	The data is copied into the
 * current memory context, so callers should always switch away from
 * ErrorContext first; otherwise it will be lost when FlushErrorState is done.
 */
ErrorData* CopyErrorData(void)
{
    ErrorData* edata = &t_thrd.log_cxt.errordata[t_thrd.log_cxt.errordata_stack_depth];
    ErrorData* newedata = NULL;
    /*
     * we don't increment t_thrd.log_cxt.recursion_depth because out-of-memory here does not
     * indicate a problem within the error subsystem.
     */
    CHECK_STACK_DEPTH();

    Assert(CurrentMemoryContext != ErrorContext);

    /* Copy the struct itself */
    newedata = (ErrorData*)palloc(sizeof(ErrorData));
    errno_t rc = memcpy_s(newedata, sizeof(ErrorData), edata, sizeof(ErrorData));
    securec_check(rc, "\0", "\0");
    /* Make copies of separately-allocated fields */
    if (newedata->message)
        newedata->message = pstrdup(newedata->message);
    if (newedata->detail)
        newedata->detail = pstrdup(newedata->detail);
    if (newedata->detail_log)
        newedata->detail_log = pstrdup(newedata->detail_log);
    if (newedata->hint)
        newedata->hint = pstrdup(newedata->hint);
    if (newedata->context)
        newedata->context = pstrdup(newedata->context);
    if (newedata->internalquery)
        newedata->internalquery = pstrdup(newedata->internalquery);
    if (newedata->filename)
        newedata->filename = pstrdup(newedata->filename);
    if (newedata->funcname)
        newedata->funcname = pstrdup(newedata->funcname);
    if (newedata->backtrace_log)
        newedata->backtrace_log = pstrdup(newedata->backtrace_log);

    return newedata;
}

/*
 * UpdateErrorData --- update current edata from newData.
 */
void UpdateErrorData(ErrorData* edata, ErrorData* newData)
{
    FREE_POINTER(edata->message);
    FREE_POINTER(edata->detail);
    FREE_POINTER(edata->detail_log);
    FREE_POINTER(edata->hint);
    FREE_POINTER(edata->context);
    FREE_POINTER(edata->internalquery);
    FREE_POINTER(edata->backtrace_log);

    MemoryContext oldcontext = MemoryContextSwitchTo(ErrorContext);

    edata->elevel = newData->elevel;
    edata->filename = pstrdup(newData->filename);
    edata->lineno = newData->lineno;
    edata->funcname = pstrdup(newData->funcname);
    edata->sqlerrcode = newData->sqlerrcode;
    edata->message = pstrdup(newData->message);
    edata->detail = pstrdup(newData->detail);
    edata->detail_log = pstrdup(newData->detail_log);
    edata->hint = pstrdup(newData->hint);
    edata->context = pstrdup(newData->context);
    edata->cursorpos = newData->cursorpos;
    edata->internalpos = newData->internalpos;
    edata->internalquery = pstrdup(newData->internalquery);
    edata->saved_errno = newData->saved_errno;
    edata->backtrace_log = pstrdup(newData->backtrace_log);
    edata->internalerrcode = newData->internalerrcode;

    MemoryContextSwitchTo(oldcontext);
}

/*
 * FreeErrorData --- free the structure returned by CopyErrorData.
 *
 * Error handlers should use this in preference to assuming they know all
 * the separately-allocated fields.
 */
void FreeErrorData(ErrorData* edata)
{
    if (edata->message)
        pfree(edata->message);
    if (edata->detail)
        pfree(edata->detail);
    if (edata->detail_log)
        pfree(edata->detail_log);
    if (edata->hint)
        pfree(edata->hint);
    if (edata->context)
        pfree(edata->context);
    if (edata->internalquery)
        pfree(edata->internalquery);
    if (edata->filename)
        pfree(edata->filename);
    if (edata->funcname)
        pfree(edata->funcname);
    if (edata->backtrace_log) {
        pfree(edata->backtrace_log);
        edata->backtrace_log = NULL;
    }

    pfree(edata);
}

/*
 * FlushErrorState --- flush the error state after error recovery
 *
 * This should be called by an error handler after it's done processing
 * the error; or as soon as it's done CopyErrorData, if it intends to
 * do stuff that is likely to provoke another error.  You are not "out" of
 * the error subsystem until you have done this.
 */
void FlushErrorState(void)
{
    /*
     * Reset stack to empty.  The only case where it would be more than one
     * deep is if we serviced an error that interrupted construction of
     * another message.  We assume control escaped out of that message
     * construction and won't ever go back.
     */
    t_thrd.log_cxt.errordata_stack_depth = -1;
    t_thrd.log_cxt.recursion_depth = 0;
    /* Delete all data in ErrorContext */
    MemoryContextResetAndDeleteChildren(ErrorContext);
}

void FlushErrorStateWithoutDeleteChildrenContext(void)
{
    t_thrd.log_cxt.errordata_stack_depth = -1;
    t_thrd.log_cxt.recursion_depth = 0;
    MemoryContextReset(ErrorContext);
}

/*
 * ReThrowError --- re-throw a previously copied error
 *
 * A handler can do CopyErrorData/FlushErrorState to get out of the error
 * subsystem, then do some processing, and finally ReThrowError to re-throw
 * the original error.	This is slower than just PG_RE_THROW() but should
 * be used if the "some processing" is likely to incur another error.
 */
void ReThrowError(ErrorData* edata)
{
    ErrorData* newedata = NULL;
    Assert(edata->elevel == ERROR);

    /* Push the data back into the error context */
    t_thrd.log_cxt.recursion_depth++;
    MemoryContextSwitchTo(ErrorContext);

    if (++t_thrd.log_cxt.errordata_stack_depth >= ERRORDATA_STACK_SIZE) {
        /*
         * Wups, stack not big enough.	We treat this as a PANIC condition
         * because it suggests an infinite loop of errors during error
         * recovery.
         */
        t_thrd.log_cxt.errordata_stack_depth = -1; /* make room on stack */
        ereport(PANIC, (errmsg_internal("ERRORDATA_STACK_SIZE exceeded")));
    }

    newedata = &t_thrd.log_cxt.errordata[t_thrd.log_cxt.errordata_stack_depth];
    errno_t rc = memcpy_s(newedata, sizeof(ErrorData), edata, sizeof(ErrorData));
    securec_check(rc, "\0", "\0");
    /* Make copies of separately-allocated fields */
    if (newedata->message)
        newedata->message = pstrdup(newedata->message);
    if (newedata->detail)
        newedata->detail = pstrdup(newedata->detail);
    if (newedata->detail_log)
        newedata->detail_log = pstrdup(newedata->detail_log);
    if (newedata->hint)
        newedata->hint = pstrdup(newedata->hint);
    if (newedata->context)
        newedata->context = pstrdup(newedata->context);
    if (newedata->internalquery)
        newedata->internalquery = pstrdup(newedata->internalquery);
    if (newedata->filename)
        newedata->filename = pstrdup(newedata->filename);
    if (newedata->funcname)
        newedata->funcname = pstrdup(newedata->funcname);
    if (newedata->backtrace_log)
        newedata->backtrace_log = pstrdup(newedata->backtrace_log);

    t_thrd.log_cxt.recursion_depth--;
    PG_RE_THROW();
}

/*
 * pg_re_throw --- out-of-line implementation of PG_RE_THROW() macro
 */
void pg_re_throw(void)
{
    /* If possible, throw the error to the next outer setjmp handler */
    if (t_thrd.log_cxt.PG_exception_stack != NULL)
        siglongjmp(*t_thrd.log_cxt.PG_exception_stack, 1);
    else {
        /*
         * If we get here, elog(ERROR) was thrown inside a PG_TRY block, which
         * we have now exited only to discover that there is no outer setjmp
         * handler to pass the error to.  Had the error been thrown outside
         * the block to begin with, we'd have promoted the error to FATAL, so
         * the correct behavior is to make it FATAL now; that is, emit it and
         * then call proc_exit.
         */
        ErrorData* edata = &t_thrd.log_cxt.errordata[t_thrd.log_cxt.errordata_stack_depth];

        Assert(t_thrd.log_cxt.errordata_stack_depth >= 0);
        Assert(edata->elevel == ERROR);
        edata->elevel = FATAL;

        /*
         * At least in principle, the increase in severity could have changed
         * where-to-output decisions, so recalculate.  This should stay in
         * sync with errstart(), which see for comments.
         */
        if (IsPostmasterEnvironment)
            edata->output_to_server = is_log_level_output(FATAL, log_min_messages);
        else
            edata->output_to_server = (FATAL >= log_min_messages);

        if (t_thrd.postgres_cxt.whereToSendOutput == DestRemote)
            edata->output_to_client = true;

        /*
         * We can use errfinish() for the rest, but we don't want it to call
         * any error context routines a second time.  Since we know we are
         * about to exit, it should be OK to just clear the context stack.
         */
        t_thrd.log_cxt.error_context_stack = NULL;

        errfinish(0);
    }

    /* Doesn't return ... */
    ExceptionalCondition("pg_re_throw tried to return", "FailedAssertion", __FILE__, __LINE__);
}

/*
 * Initialization of error output file
 */
void DebugFileOpen(void)
{
    int fd = 0;
    int istty = 0;

    if (t_thrd.proc_cxt.OutputFileName[0]) {
        /*
         * A debug-output file name was given.
         *
         * Make sure we can write the file, and find out if it's a tty.
         */
        if ((fd = open(t_thrd.proc_cxt.OutputFileName, O_CREAT | O_APPEND | O_WRONLY, 0600)) < 0) {
            ereport(FATAL,
                (errcode_for_file_access(), errmsg("could not open file \"%s\": %m", t_thrd.proc_cxt.OutputFileName)));
        }
        istty = isatty(fd);
        close(fd);

        /*
         * Redirect our stderr to the debug output file.
         */
        if (!freopen(t_thrd.proc_cxt.OutputFileName, "a", stderr)) {
            ereport(FATAL,
                (errcode_for_file_access(),
                    errmsg("could not reopen file \"%s\" as stderr: %m", t_thrd.proc_cxt.OutputFileName)));
        }

        /*
         * If the file is a tty and we're running under the postmaster, try to
         * send stdout there as well (if it isn't a tty then stderr will block
         * out stdout, so we may as well let stdout go wherever it was going
         * before).
         */
        if (istty && IsUnderPostmaster) {
            if (!freopen(t_thrd.proc_cxt.OutputFileName, "a", stdout)) {
                ereport(FATAL,
                    (errcode_for_file_access(),
                        errmsg("could not reopen file \"%s\" as stdout: %m", t_thrd.proc_cxt.OutputFileName)));
            }
        }
    }
}

#ifdef HAVE_SYSLOG

/*
 * Set or update the parameters for syslog logging
 */
void set_syslog_parameters(const char* ident, int facility)
{
    /*
     * guc.c is likely to call us repeatedly with same parameters, so don't
     * thrash the syslog connection unnecessarily.	Also, we do not re-open
     * the connection until needed, since this routine will get called whether
     * or not t_thrd.log_cxt.Log_destination actually mentions syslog.
     *
     * Note that we make our own copy of the ident string rather than relying
     * on guc.c's.  This may be overly paranoid, but it ensures that we cannot
     * accidentally free a string that syslog is still using.
     */
    if (u_sess->log_cxt.syslog_ident == NULL || strcmp(u_sess->log_cxt.syslog_ident, ident) != 0 ||
        u_sess->attr.attr_common.syslog_facility != facility) {
        if (t_thrd.log_cxt.openlog_done) {
            closelog();
            t_thrd.log_cxt.openlog_done = false;
        }
        if (u_sess->log_cxt.syslog_ident)
            pfree(u_sess->log_cxt.syslog_ident);
        u_sess->log_cxt.syslog_ident = MemoryContextStrdup(u_sess->top_mem_cxt, ident);
        /* if the strdup fails, we will cope in write_syslog() */
        u_sess->attr.attr_common.syslog_facility = facility;
    }
}

/*
 * Write a message line to syslog
 */
static void write_syslog(int level, const char* line)
{
    const char* nlpos = NULL;
    errno_t rc = EOK;

    /* Open syslog connection if not done yet */
    if (!t_thrd.log_cxt.openlog_done) {
        openlog(u_sess->log_cxt.syslog_ident ? u_sess->log_cxt.syslog_ident : "postgres",
            LOG_PID | LOG_NDELAY | LOG_NOWAIT,
            u_sess->attr.attr_common.syslog_facility);
        t_thrd.log_cxt.openlog_done = true;
    }

    /*
     * We add a sequence number to each log message to suppress "same"
     * messages.
     */
    t_thrd.log_cxt.syslog_seq++;

    /*
     * Our problem here is that many syslog implementations don't handle long
     * messages in an acceptable manner. While this function doesn't help that
     * fact, it does work around by splitting up messages into smaller pieces.
     *
     * We divide into multiple syslog() calls if message is too long or if the
     * message contains embedded newline(s).
     */
    int len = strlen(line);
    nlpos = strchr(line, '\n');
    if (len > PG_SYSLOG_LIMIT || nlpos != NULL) {
        int chunk_nr = 0;

        while (len > 0) {
            char buf[PG_SYSLOG_LIMIT + 1];
            int buflen;
            int i;

            /* if we start at a newline, move ahead one char */
            if (line[0] == '\n') {
                line++;
                len--;
                /* we need to recompute the next newline's position, too */
                nlpos = strchr(line, '\n');
                continue;
            }

            /* copy one line, or as much as will fit, to buf */
            if (nlpos != NULL)
                buflen = nlpos - line;
            else
                buflen = len;
            buflen = Min(buflen, PG_SYSLOG_LIMIT);
            rc = memcpy_s(buf, PG_SYSLOG_LIMIT + 1, line, buflen);
            securec_check(rc, "\0", "\0");
            buf[buflen] = '\0';

            /* trim to multibyte letter boundary */
            buflen = pg_mbcliplen(buf, buflen, buflen);
            if (buflen <= 0)
                return;
            buf[buflen] = '\0';

            /* already word boundary? */
            if (line[buflen] != '\0' && !isspace((unsigned char)line[buflen])) {
                /* try to divide at word boundary */
                i = buflen - 1;
                while (i > 0 && !isspace((unsigned char)buf[i]))
                    i--;

                /* else couldn't divide word boundary */
                if (i > 0) {
                    buflen = i;
                    buf[i] = '\0';
                }
            }

            chunk_nr++;

            syslog(level, "[%lu-%d] %s", t_thrd.log_cxt.syslog_seq, chunk_nr, buf);
            line += buflen;
            len -= buflen;
        }
    } else {
        /* message short enough */
        syslog(level, "[%lu] %s", t_thrd.log_cxt.syslog_seq, line);
    }
}
#endif /* HAVE_SYSLOG */

#ifdef WIN32
/*
 * Write a message line to the windows event log
 */
static void write_eventlog(int level, const char* line, int len)
{
    WCHAR* utf16 = NULL;
    int eventlevel = EVENTLOG_ERROR_TYPE;
    static HANDLE evtHandle = INVALID_HANDLE_VALUE;

    if (evtHandle == INVALID_HANDLE_VALUE) {
        evtHandle = RegisterEventSource(
            NULL, g_instance.attr.attr_common.event_source ? g_instance.attr.attr_common.event_source : "PostgreSQL");
        if (evtHandle == NULL) {
            evtHandle = INVALID_HANDLE_VALUE;
            return;
        }
    }

    switch (level) {
        case DEBUG5:
        case DEBUG4:
        case DEBUG3:
        case DEBUG2:
        case DEBUG1:
        case LOG:
        case COMMERROR:
        case INFO:
        case NOTICE:
            eventlevel = EVENTLOG_INFORMATION_TYPE;
            break;
        case WARNING:
            eventlevel = EVENTLOG_WARNING_TYPE;
            break;
        case ERROR:
        case FATAL:
        case PANIC:
        default:
            eventlevel = EVENTLOG_ERROR_TYPE;
            break;
    }

    /*
     * Convert message to UTF16 text and write it with ReportEventW, but
     * fall-back into ReportEventA if conversion failed.
     *
     * Also verify that we are not on our way into error recursion trouble due
     * to error messages thrown deep inside pgwin32_toUTF16().
     */
    if (GetDatabaseEncoding() != GetPlatformEncoding() && !in_error_recursion_trouble()) {
        utf16 = pgwin32_toUTF16(line, len, NULL);
        if (utf16 != NULL) {
            ReportEventW(evtHandle,
                eventlevel,
                0,
                0, /* All events are Id 0 */
                NULL,
                1,
                0,
                (LPCWSTR*)&utf16,
                NULL);

            pfree(utf16);
            return;
        }
    }
    ReportEventA(evtHandle,
        eventlevel,
        0,
        0, /* All events are Id 0 */
        NULL,
        1,
        0,
        &line,
        NULL);
}
#endif /* WIN32 */

static void write_console(const char* line, int len)
{
    int rc;

#ifdef WIN32

    /*
     * WriteConsoleW() will fail if stdout is redirected, so just fall through
     * to writing unconverted to the logfile in this case.
     *
     * Since we palloc the structure required for conversion, also fall
     * through to writing unconverted if we have not yet set up
     * CurrentMemoryContext.
     */
    if (GetDatabaseEncoding() != GetPlatformEncoding() && !in_error_recursion_trouble() &&
        !t_thrd.postmaster_cxt.redirection_done && CurrentMemoryContext != NULL) {
        WCHAR* utf16 = NULL;
        int utf16len;

        utf16 = pgwin32_toUTF16(line, len, &utf16len);
        if (utf16 != NULL) {
            HANDLE std_handle;
            DWORD written;

            std_handle = GetStdHandle(STD_ERROR_HANDLE);
            if (WriteConsoleW(std_handle, utf16, utf16len, &written, NULL)) {
                pfree(utf16);
                return;
            }

            /*
             * In case WriteConsoleW() failed, fall back to writing the
             * message unconverted.
             */
            pfree(utf16);
        }
    }
#else

    /*
     * Conversion on non-win32 platforms is not implemented yet. It requires
     * non-throw version of pg_do_encoding_conversion(), that converts
     * unconvertable characters to '?' without errors.
     */
#endif

    /*
     * We ignore any error from write() here.  We have no useful way to report
     * it ... certainly whining on stderr isn't likely to be productive.
     */
    rc = write(fileno(stderr), line, len);
    (void)rc;
}

/*
 * setup t_thrd.log_cxt.formatted_log_time, for consistent times between CSV and regular logs
 */
static void setup_formatted_log_time(void)
{
    struct timeval tv;
    pg_time_t stamp_time;
    char msbuf[8];

    gettimeofday(&tv, NULL);
    stamp_time = (pg_time_t)tv.tv_sec;

    /*
     * Note: we expect that guc.c will ensure that log_timezone is set up (at
     * least with a minimal GMT value) before u_sess->attr.attr_common.Log_line_prefix can become
     * nonempty or CSV mode can be selected.
     */
    pg_strftime(t_thrd.log_cxt.formatted_log_time,
        FORMATTED_TS_LEN,
        /* leave room for milliseconds... */
        "%Y-%m-%d %H:%M:%S     %Z",
        pg_localtime(&stamp_time, log_timezone));

    /* 'paste' milliseconds into place... */
    errno_t rc = sprintf_s(msbuf, sizeof(msbuf), ".%03d", (int)(tv.tv_usec / 1000));
    securec_check_ss(rc, "\0", "\0");
    rc = strncpy_s(t_thrd.log_cxt.formatted_log_time + 19, FORMATTED_TS_LEN - 19, msbuf, 4);
    securec_check(rc, "\0", "\0");
}

/*
 * setup t_thrd.log_cxt.formatted_start_time
 */
static void setup_formatted_start_time(void)
{
    pg_time_t stamp_time = (pg_time_t)t_thrd.proc_cxt.MyStartTime;

    /*
     * Note: we expect that guc.c will ensure that log_timezone is set up (at
     * least with a minimal GMT value) before u_sess->attr.attr_common.Log_line_prefix can become
     * nonempty or CSV mode can be selected.
     */
    pg_strftime(t_thrd.log_cxt.formatted_start_time,
        FORMATTED_TS_LEN,
        "%Y-%m-%d %H:%M:%S %Z",
        pg_localtime(&stamp_time, log_timezone));
}

/*
 * Format tag info for log lines; append to the provided buffer.
 */
static void log_line_prefix(StringInfo buf, ErrorData* edata)
{
    int format_len;
    int i;

    t_thrd.log_cxt.error_with_nodename = false;

    /*
     * This is one of the few places where we'd rather not inherit a static
     * variable's value from the postmaster.  But since we will, reset it when
     * t_thrd.proc_cxt.MyProcPid changes. t_thrd.proc_cxt.MyStartTime also changes when t_thrd.proc_cxt.MyProcPid does,
     * so reset the formatted start timestamp too.
     */
    if (t_thrd.log_cxt.log_my_pid != t_thrd.proc_cxt.MyProcPid) {
        t_thrd.log_cxt.log_line_number = 0;
        t_thrd.log_cxt.log_my_pid = t_thrd.proc_cxt.MyProcPid;
        t_thrd.log_cxt.formatted_start_time[0] = '\0';
    }
    t_thrd.log_cxt.log_line_number++;

    if (u_sess->attr.attr_common.Log_line_prefix == NULL) {
        /* for --single, do not append query id */
        if (IsPostmasterEnvironment) {
            appendStringInfo(buf, "%lu ", u_sess->debug_query_id);
        }
        return; /* in case guc hasn't run yet */
    }

    format_len = strlen(u_sess->attr.attr_common.Log_line_prefix);

    for (i = 0; i < format_len; i++) {
        if (u_sess->attr.attr_common.Log_line_prefix[i] != '%') {
            /* literal char, just copy */
            appendStringInfoChar(buf, u_sess->attr.attr_common.Log_line_prefix[i]);
            continue;
        }
        /* go to char after '%' */
        i++;
        if (i >= format_len)
            break; /* format error - ignore it */

        /* process the option */
        switch (u_sess->attr.attr_common.Log_line_prefix[i]) {
            case 'a':
                if (u_sess->proc_cxt.MyProcPort && u_sess->attr.attr_common.application_name &&
                    u_sess->attr.attr_common.application_name[0] != '\0') {
                    appendStringInfoString(buf, u_sess->attr.attr_common.application_name);
                } else {
                    appendStringInfoString(buf, "[unknown]");
                }
                break;
            case 'u':
                if (u_sess->proc_cxt.MyProcPort && u_sess->proc_cxt.MyProcPort->user_name &&
                    u_sess->proc_cxt.MyProcPort->user_name[0] != '\0') {
                    appendStringInfoString(buf, u_sess->proc_cxt.MyProcPort->user_name);
                } else {
                    appendStringInfoString(buf, "[unknown]");
                }
                break;
            case 'd':
                if (u_sess->proc_cxt.MyProcPort && u_sess->proc_cxt.MyProcPort->database_name &&
                    u_sess->proc_cxt.MyProcPort->database_name[0] != '\0') {
                    appendStringInfoString(buf, u_sess->proc_cxt.MyProcPort->database_name);
                } else {
                    appendStringInfoString(buf, "[unknown]");
                }
                break;
            case 'c':
                appendStringInfo(buf, "%lx.%d", (long)(t_thrd.proc_cxt.MyStartTime), t_thrd.myLogicTid);
                break;
            case 'p':
                appendStringInfo(buf, "%lu", t_thrd.proc_cxt.MyProcPid);
                break;
            case 'l':
                appendStringInfo(buf, "%ld", t_thrd.log_cxt.log_line_number);
                break;
            case 'm':
                setup_formatted_log_time();
                appendStringInfoString(buf, t_thrd.log_cxt.formatted_log_time);
                break;
            case 't': {
                pg_time_t stamp_time = (pg_time_t)time(NULL);
                char strfbuf[128];

                pg_strftime(strfbuf, sizeof(strfbuf), "%Y-%m-%d %H:%M:%S %Z", pg_localtime(&stamp_time, log_timezone));
                appendStringInfoString(buf, strfbuf);
            } break;
            case 's':
                if (t_thrd.log_cxt.formatted_start_time[0] == '\0')
                    setup_formatted_start_time();
                appendStringInfoString(buf, t_thrd.log_cxt.formatted_start_time);
                break;
            case 'i':
                if (u_sess->proc_cxt.MyProcPort) {
                    const char* psdisp = NULL;
                    int displen;

                    psdisp = get_ps_display(&displen);
                    appendBinaryStringInfo(buf, psdisp, displen);
                } else {
                    appendStringInfoString(buf, "[unknown]");
                }
                break;
            case 'r':
                if (u_sess->proc_cxt.MyProcPort && u_sess->proc_cxt.MyProcPort->remote_host) {
                    appendStringInfoString(buf, u_sess->proc_cxt.MyProcPort->remote_host);
                    if (u_sess->proc_cxt.MyProcPort->remote_port && u_sess->proc_cxt.MyProcPort->remote_port[0] != '\0')
                        appendStringInfo(buf, "(%s)", u_sess->proc_cxt.MyProcPort->remote_port);
                } else {
                    appendStringInfoString(buf, "localhost");
                }
                break;
            case 'h':
                if (u_sess->proc_cxt.MyProcPort && u_sess->proc_cxt.MyProcPort->remote_host)
                    appendStringInfoString(buf, u_sess->proc_cxt.MyProcPort->remote_host);
                else
                    appendStringInfoString(buf, "localhost");
                break;
            case 'q':
                /* in postmaster and friends, stop if %q is seen */
                /* in a backend, just ignore */
                if (u_sess->proc_cxt.MyProcPort == NULL)
                    i = format_len;
                break;
            case 'v':
                /* keep VXID format in sync with lockfuncs.c */
                if (t_thrd.proc != NULL && t_thrd.proc->backendId != InvalidBackendId)
                    appendStringInfo(buf, "%d/" XID_FMT, t_thrd.proc->backendId, t_thrd.proc->lxid);
                else {
                    appendStringInfo(buf, "0/0");
                }
                break;
            case 'x':
                appendStringInfo(buf, XID_FMT, GetTopTransactionIdIfAny());
                break;
            case 'e':
                appendStringInfoString(buf, unpack_sql_state(edata->sqlerrcode));
                break;
            case 'n':
                appendStringInfoString(buf, g_instance.attr.attr_common.PGXCNodeName);
                t_thrd.log_cxt.error_with_nodename = true;
                break;
            case 'S':
                appendStringInfo(buf, "%lu", u_sess->session_id);
                break;
            case '%':
                appendStringInfoChar(buf, '%');
                break;
            default:
                /* format error - ignore it */
                break;
        }
    }

    /* for --single, do not append query id */
    if (IsPostmasterEnvironment)
        appendStringInfo(buf, "%lu ", u_sess->debug_query_id);

    /* module name information */
    appendStringInfo(buf, "[%s] ", get_valid_module_name(edata->mod_id));
}

/*
 * append a CSV'd version of a string to a StringInfo
 * We use the PostgreSQL defaults for CSV, i.e. quote = escape = '"'
 * If it's NULL, append nothing.
 */
static inline void appendCSVLiteral(StringInfo buf, const char* data)
{
    const char* p = data;
    char c;

    /* avoid confusing an empty string with NULL */
    if (p == NULL)
        return;

    appendStringInfoCharMacro(buf, '"');
    while ((c = *p++) != '\0') {
        if (c == '"')
            appendStringInfoCharMacro(buf, '"');
        appendStringInfoCharMacro(buf, c);
    }
    appendStringInfoCharMacro(buf, '"');
}

/*
 * Constructs the error message, depending on the Errordata it gets, in a CSV
 * format which is described in doc/src/sgml/config.sgml.
 */
static void write_csvlog(ErrorData* edata)
{
    StringInfoData buf;
    bool print_stmt = false;

    /*
     * This is one of the few places where we'd rather not inherit a static
     * variable's value from the postmaster.  But since we will, reset it when
     * t_thrd.proc_cxt.MyProcPid changes.
     */
    if (t_thrd.log_cxt.csv_log_my_pid != t_thrd.proc_cxt.MyProcPid) {
        t_thrd.log_cxt.csv_log_line_number = 0;
        t_thrd.log_cxt.csv_log_my_pid = t_thrd.proc_cxt.MyProcPid;
        t_thrd.log_cxt.formatted_start_time[0] = '\0';
    }
    t_thrd.log_cxt.csv_log_line_number++;

    initStringInfo(&buf);

    /*
     * timestamp with milliseconds
     *
     * Check if the timestamp is already calculated for the syslog message,
     * and use it if so.  Otherwise, get the current timestamp.  This is done
     * to put same timestamp in both syslog and csvlog messages.
     */
    if (t_thrd.log_cxt.formatted_log_time[0] == '\0') {
        setup_formatted_log_time();
    }

    /* @CSV_SCHMA@ log_time timestamp with time zone, @ */
    appendStringInfoString(&buf, t_thrd.log_cxt.formatted_log_time);
    appendStringInfoChar(&buf, ',');

    /* @CSV_SCHMA@ node_name text, @ */
    appendCSVLiteral(&buf, g_instance.attr.attr_common.PGXCNodeName);
    appendStringInfoChar(&buf, ',');

    /* username */
    /* @CSV_SCHMA@ user_name text, @ */
    if (u_sess->proc_cxt.MyProcPort) {
        appendCSVLiteral(&buf, u_sess->proc_cxt.MyProcPort->user_name);
    }
    appendStringInfoChar(&buf, ',');

    /* database name */
    /* @CSV_SCHMA@ dbname text, @ */
    if (u_sess->proc_cxt.MyProcPort) {
        appendCSVLiteral(&buf, u_sess->proc_cxt.MyProcPort->database_name);
    }
    appendStringInfoChar(&buf, ',');

    /* Process id  */
    /* @CSV_SCHMA@ thread_id bigint, @ */
    if (t_thrd.proc_cxt.MyProcPid != 0)
        appendStringInfo(&buf, "%lu", t_thrd.proc_cxt.MyProcPid);
    appendStringInfoChar(&buf, ',');

    /* Remote host and port */
    /* @CSV_SCHMA@ remote_host text, @ */
    if (u_sess->proc_cxt.MyProcPort && u_sess->proc_cxt.MyProcPort->remote_host) {
        appendStringInfoChar(&buf, '"');
        appendStringInfoString(&buf, u_sess->proc_cxt.MyProcPort->remote_host);
        if (u_sess->proc_cxt.MyProcPort->remote_port != NULL && u_sess->proc_cxt.MyProcPort->remote_port[0] != '\0') {
            appendStringInfoChar(&buf, ':');
            appendStringInfoString(&buf, u_sess->proc_cxt.MyProcPort->remote_port);
        }
        appendStringInfoChar(&buf, '"');
    }
    appendStringInfoChar(&buf, ',');

    /* session id */
    /* OLAP: keep the same value with %c in log_line_prefix, so
     *       replace t_thrd.proc_cxt.MyProcPid with myLogicTid.
     */
    /* @CSV_SCHMA@ session_id text, @ */
    appendStringInfo(&buf, "%lx.%d", (long)t_thrd.proc_cxt.MyStartTime, t_thrd.myLogicTid);
    appendStringInfoChar(&buf, ',');

    /* Line number */
    /* @CSV_SCHMA@ lineno bigint, @ */
    appendStringInfo(&buf, "%ld", t_thrd.log_cxt.csv_log_line_number);
    appendStringInfoChar(&buf, ',');

    /* PS display */
    /* @CSV_SCHMA@ psdisp text, @ */
    if (u_sess->proc_cxt.MyProcPort) {
        StringInfoData msgbuf;
        const char* psdisp = NULL;
        int displen = 0;

        initStringInfo(&msgbuf);

        psdisp = get_ps_display(&displen);
        appendBinaryStringInfo(&msgbuf, psdisp, displen);
        appendCSVLiteral(&buf, msgbuf.data);

        pfree(msgbuf.data);
    }
    appendStringInfoChar(&buf, ',');

    /* session start timestamp */
    /* @CSV_SCHMA@ session_start_tm timestamp with time zone , @ */
    if (t_thrd.log_cxt.formatted_start_time[0] == '\0') {
        setup_formatted_start_time();
    }
    appendStringInfoString(&buf, t_thrd.log_cxt.formatted_start_time);
    appendStringInfoChar(&buf, ',');

    /* Virtual transaction id */
    /* keep VXID format in sync with lockfuncs.c */
    /* @CSV_SCHMA@ vxid text , @ */
    if (t_thrd.proc != NULL && t_thrd.proc->backendId != InvalidBackendId) {
        appendStringInfo(&buf, "%d/" XID_FMT, t_thrd.proc->backendId, t_thrd.proc->lxid);
    }
    appendStringInfoChar(&buf, ',');

    /* Transaction id */
    /* @CSV_SCHMA@ xid bigint , @ */
    appendStringInfo(&buf, XID_FMT, GetTopTransactionIdIfAny());
    appendStringInfoChar(&buf, ',');

    /* OLAP: debug query id */
    /* @CSV_SCHMA@ query_id bigint , @ */
    if (IsPostmasterEnvironment) {
        appendStringInfo(&buf, "%lu", u_sess->debug_query_id);
    }
    appendStringInfoChar(&buf, ',');

    /* OLAP: Module/Feature ID */
    /* @CSV_SCHMA@ module text , @ */
    appendStringInfoChar(&buf, '"');
    appendStringInfo(&buf, "%s", get_valid_module_name(edata->mod_id));
    appendStringInfoChar(&buf, '"');
    appendStringInfoChar(&buf, ',');

    /* Error severity */
    /* @CSV_SCHMA@ log_level text, @ */
    appendStringInfoString(&buf, error_severity(edata->elevel));
    appendStringInfoChar(&buf, ',');

    /* SQL state code */
    /* @CSV_SCHMA@ sql_state text, @ */
    appendStringInfoString(&buf, unpack_sql_state(edata->sqlerrcode));
    appendStringInfoChar(&buf, ',');

    /* errmessage */
    /* @CSV_SCHMA@ msg text, @ */
    appendCSVLiteral(&buf, edata->message);
    appendStringInfoChar(&buf, ',');

    /* errdetail or errdetail_log */
    /* @CSV_SCHMA@ detail text, @ */
    if (edata->detail_log)
        appendCSVLiteral(&buf, edata->detail_log);
    else
        appendCSVLiteral(&buf, edata->detail);
    appendStringInfoChar(&buf, ',');

    /* errhint */
    /* @CSV_SCHMA@ hint text, @ */
    appendCSVLiteral(&buf, edata->hint);
    appendStringInfoChar(&buf, ',');

    /* internal query */
    /* @CSV_SCHMA@ internal_query text, @ */
    if (edata->internalquery) {
        char* mask_string = NULL;

        /* mask the query whenever including sensitive information. */
        mask_string = maskPassword(edata->internalquery);

        /* with nothing to mask, just use the source query. */
        if (mask_string == NULL)
            mask_string = edata->internalquery;

        appendCSVLiteral(&buf, mask_string);
        appendStringInfoChar(&buf, ',');

        /* free the memory malloced for mask_string. */
        if (mask_string != edata->internalquery)
            pfree(mask_string);
    } else {
        appendCSVLiteral(&buf, edata->internalquery);
        appendStringInfoChar(&buf, ',');
    }

    /* if printed internal query, print internal pos too */
    /* @CSV_SCHMA@ internal_pos int, @ */
    if (edata->internalpos > 0 && edata->internalquery != NULL)
        appendStringInfo(&buf, "%d", edata->internalpos);
    appendStringInfoChar(&buf, ',');

    /* errcontext */
    /* @CSV_SCHMA@ errcontext text, @ */
    appendCSVLiteral(&buf, edata->context);
    appendStringInfoChar(&buf, ',');

    /* user query --- only reported if not disabled by the caller */
    if (is_log_level_output(edata->elevel, u_sess->attr.attr_common.log_min_error_statement) &&
        t_thrd.postgres_cxt.debug_query_string != NULL && !edata->hide_stmt)
        print_stmt = true;
    /* @CSV_SCHMA@ user_query text, @ */
    if (print_stmt) {
        char* mask_string = maskPassword(t_thrd.postgres_cxt.debug_query_string);

        if (mask_string == NULL) {
            mask_string = (char*)t_thrd.postgres_cxt.debug_query_string;
        }
        appendCSVLiteral(&buf, mask_string);
        if (mask_string != t_thrd.postgres_cxt.debug_query_string) {
            pfree(mask_string);
        }
    }
    appendStringInfoChar(&buf, ',');

    /* @CSV_SCHMA@ user_query_pos int, @ */
    if (print_stmt && edata->cursorpos > 0) {
        appendStringInfo(&buf, "%d", edata->cursorpos);
    }
    appendStringInfoChar(&buf, ',');

    /* file error location */
    /* @CSV_SCHMA@ fun_name text, @ */
    /* @CSV_SCHMA@ file_location text, @ */
    if (u_sess->attr.attr_common.Log_error_verbosity >= PGERROR_VERBOSE) {
        StringInfoData msgbuf;

        initStringInfo(&msgbuf);

        if (edata->funcname && edata->filename) {
            appendStringInfo(&msgbuf, "%s,%s:%d", edata->funcname, edata->filename, edata->lineno);
        } else if (edata->filename) {
            /* make filename field null */
            appendStringInfo(&msgbuf, ",%s:%d", edata->filename, edata->lineno);
        }
        appendCSVLiteral(&buf, msgbuf.data);
        pfree(msgbuf.data);
    }
    appendStringInfoChar(&buf, ',');

    /* application name */
    /* @CSV_SCHMA@ appname text @ */
    if (u_sess->attr.attr_common.application_name) {
        appendCSVLiteral(&buf, u_sess->attr.attr_common.application_name);
    }

    /* append line end char */
    appendStringInfoChar(&buf, '\n');

    /* If in the syslogger process, try to write messages direct to file */
    if (t_thrd.role == SYSLOGGER) {
        write_syslogger_file(buf.data, buf.len, LOG_DESTINATION_CSVLOG);
    } else {
        write_pipe_chunks(buf.data, buf.len, LOG_DESTINATION_CSVLOG);
    }

    pfree(buf.data);
}

/*
 * Unpack MAKE_SQLSTATE code. Note that this returns a pointer to a
 * static THR_LOCAL buffer.
 */
char* unpack_sql_state(int sql_state)
{
    char* buf = t_thrd.buf_cxt.unpack_sql_state_buf;
    int i;

    for (i = 0; i < 5; i++) {
        buf[i] = PGUNSIXBIT(sql_state);
        sql_state >>= 6;
    }

    buf[i] = '\0';
    return buf;
}

static int output_backtrace_to_log(StringInfoData* pOutBuf)
{
    const int max_buffer_size = 128;
    void* buffer[max_buffer_size];

    char title[max_buffer_size];

    AutoMutexLock btLock(&bt_lock);

    btLock.lock();

    if (t_thrd.log_cxt.thd_bt_symbol) {
        free(t_thrd.log_cxt.thd_bt_symbol);
        t_thrd.log_cxt.thd_bt_symbol = NULL;
    }

    int len_symbols = backtrace(buffer, max_buffer_size);
    t_thrd.log_cxt.thd_bt_symbol = backtrace_symbols(buffer, len_symbols);

    int ret = snprintf_s(title, sizeof(title), sizeof(title) - 1, "tid[%d]'s backtrace:\n", gettid());
    securec_check_ss_c(ret, "\0", "\0");
    appendStringInfoString(pOutBuf, title);

    if (t_thrd.log_cxt.thd_bt_symbol == NULL) {
        appendStringInfoString(pOutBuf, "Failed to get backtrace symbols.\n");
        btLock.unLock();
        return -1;
    }

    for (int i = 0; i < len_symbols; i++) {
        appendStringInfoString(pOutBuf, t_thrd.log_cxt.thd_bt_symbol[i]);
        appendStringInfoString(pOutBuf, "\n");
    }
    appendStringInfoString(pOutBuf, "Use addr2line to get pretty function name and line\n");

    /*
     * If above code longjmp, we should free this pointer when call this function again.
     * for normal case, free it when exit from function.
     */
    free(t_thrd.log_cxt.thd_bt_symbol);
    t_thrd.log_cxt.thd_bt_symbol = NULL;

    btLock.unLock();

    return 0;
}

/*
 * Write error report to server's log
 */
static void send_message_to_server_log(ErrorData* edata)
{
    StringInfoData buf;

    initStringInfo(&buf);

    t_thrd.log_cxt.formatted_log_time[0] = '\0';

    log_line_prefix(&buf, edata);
    appendStringInfo(&buf, "%s:  ", error_severity(edata->elevel));

    if (u_sess->attr.attr_common.Log_error_verbosity >= PGERROR_VERBOSE) {
        appendStringInfo(&buf, "%s: ", unpack_sql_state(edata->sqlerrcode));
    }

    if (edata->message) {
        append_with_tabs(&buf, edata->message);
    } else {
        append_with_tabs(&buf, _("missing error text"));
    }

    if (edata->cursorpos > 0) {
        appendStringInfo(&buf, _(" at character %d"), edata->cursorpos);
    } else if (edata->internalpos > 0) {
        appendStringInfo(&buf, _(" at character %d"), edata->internalpos);
    }

    appendStringInfoChar(&buf, '\n');

    if (u_sess->attr.attr_common.Log_error_verbosity >= PGERROR_DEFAULT) {
        if (edata->detail_log) {
            log_line_prefix(&buf, edata);
            appendStringInfoString(&buf, _("DETAIL:  "));
            append_with_tabs(&buf, edata->detail_log);
            appendStringInfoChar(&buf, '\n');
        } else if (edata->detail) {
            log_line_prefix(&buf, edata);
            appendStringInfoString(&buf, _("DETAIL:  "));
            append_with_tabs(&buf, edata->detail);
            appendStringInfoChar(&buf, '\n');
        }
        if (edata->hint) {
            log_line_prefix(&buf, edata);
            appendStringInfoString(&buf, _("HINT:  "));
            append_with_tabs(&buf, edata->hint);
            appendStringInfoChar(&buf, '\n');
        }
        if (edata->internalquery) {
            char* mask_string = NULL;
            log_line_prefix(&buf, edata);
            appendStringInfoString(&buf, _("QUERY:  "));

            mask_string = maskPassword(edata->internalquery);
            if (mask_string == NULL) {
                mask_string = (char*)edata->internalquery;
            }
            append_with_tabs(&buf, mask_string);
            if (mask_string != edata->internalquery) {
                pfree(mask_string);
            }

            appendStringInfoChar(&buf, '\n');
        }
        if (edata->context) {
            log_line_prefix(&buf, edata);
            appendStringInfoString(&buf, _("CONTEXT:  "));
            append_with_tabs(&buf, edata->context);
            appendStringInfoChar(&buf, '\n');
        }
        if (u_sess->attr.attr_common.Log_error_verbosity >= PGERROR_VERBOSE) {
            /* assume no newlines in funcname or filename... */
            if (edata->funcname && edata->filename) {
                log_line_prefix(&buf, edata);
                appendStringInfo(&buf, _("LOCATION:  %s, %s:%d\n"), edata->funcname, edata->filename, edata->lineno);
            } else if (edata->filename) {
                log_line_prefix(&buf, edata);
                appendStringInfo(&buf, _("LOCATION:  %s:%d\n"), edata->filename, edata->lineno);
            }
        }
    }

    /* Omit the query part for non-error messages in Datanode. */
    if (IS_PGXC_DATANODE && edata->elevel < ERROR)
        edata->hide_stmt = true;

    /*
     * If the user wants the query that generated this error logged, do it.
     */
    if (is_log_level_output(edata->elevel, u_sess->attr.attr_common.log_min_error_statement) &&
        t_thrd.postgres_cxt.debug_query_string != NULL && !edata->hide_stmt) {
        char* mask_string = maskPassword(t_thrd.postgres_cxt.debug_query_string);
        if (mask_string == NULL) {
            mask_string = (char*)t_thrd.postgres_cxt.debug_query_string;
        }

        log_line_prefix(&buf, edata);
        appendStringInfoString(&buf, _("STATEMENT:  "));

        /*
         * In log injection attack scene, syntax error and espaced characters are dangerous,
         * we need mask the espaced characters here.
         */
        if (edata->sqlerrcode == ERRCODE_SYNTAX_ERROR) {
            mask_espaced_character(mask_string);
        }

        append_with_tabs(&buf, mask_string);
        appendStringInfoChar(&buf, '\n');

        /* show random plan seed if u_sess->attr.attr_sql.plan_mode_seed is not OPTIMIZE_PLAN */
        char* random_plan_info = get_random_plan_string();
        if (random_plan_info != NULL) {
            appendStringInfoString(&buf, random_plan_info);
            appendStringInfoChar(&buf, '\n');
            pfree(random_plan_info);
        }

        if (mask_string != t_thrd.postgres_cxt.debug_query_string) {
            pfree(mask_string);
        }
    }

    if (edata->backtrace_log) {
        log_line_prefix(&buf, edata);
        appendStringInfoString(&buf, _("BACKTRACELOG:  "));
        append_with_tabs(&buf, edata->backtrace_log);
        appendStringInfoChar(&buf, '\n');
    }

#ifdef HAVE_SYSLOG
    /* Write to syslog, if enabled */
    if (t_thrd.log_cxt.Log_destination & LOG_DESTINATION_SYSLOG) {
        int syslog_level;

        switch (edata->elevel) {
            case DEBUG5:
            case DEBUG4:
            case DEBUG3:
            case DEBUG2:
            case DEBUG1:
                syslog_level = LOG_DEBUG;
                break;
            case LOG:
            case COMMERROR:
            case INFO:
                syslog_level = LOG_INFO;
                break;
            case NOTICE:
            case WARNING:
                syslog_level = LOG_NOTICE;
                break;
            case ERROR:
                syslog_level = LOG_WARNING;
                break;
            case FATAL:
                syslog_level = LOG_ERR;
                break;
            case PANIC:
            default:
                syslog_level = LOG_CRIT;
                break;
        }

        write_syslog(syslog_level, buf.data);
    }
#endif /* HAVE_SYSLOG */

#ifdef WIN32
    /* Write to eventlog, if enabled */
    if (t_thrd.log_cxt.Log_destination & LOG_DESTINATION_EVENTLOG) {
        write_eventlog(edata->elevel, buf.data, buf.len);
    }
#endif /* WIN32 */

    /* Write to stderr, if enabled */
    if ((t_thrd.log_cxt.Log_destination & LOG_DESTINATION_STDERR) ||
        t_thrd.postgres_cxt.whereToSendOutput == DestDebug) {
        /*
         * Use the chunking protocol if we know the syslogger should be
         * catching stderr output, and we are not ourselves the syslogger.
         * Otherwise, just do a vanilla write to stderr.
         */
        if (t_thrd.postmaster_cxt.redirection_done && t_thrd.role != SYSLOGGER) {
            write_pipe_chunks(buf.data, buf.len, LOG_DESTINATION_STDERR);
        }
#ifdef WIN32

        /*
         * In a win32 service environment, there is no usable stderr. Capture
         * anything going there and write it to the eventlog instead.
         *
         * If stderr redirection is active, it was OK to write to stderr above
         * because that's really a pipe to the syslogger process.
         */
        else if (pgwin32_is_service()) {
            write_eventlog(edata->elevel, buf.data, buf.len);
        }
#endif
        else if (t_thrd.role != SYSLOGGER) {
            write_console(buf.data, buf.len);
        }
    }

    /* If in the syslogger process, try to write messages direct to file */
    FILE* logfile = LOG_DESTINATION_CSVLOG ? t_thrd.logger.csvlogFile : t_thrd.logger.syslogFile;
    if (t_thrd.role == SYSLOGGER && logfile != NULL) {
        write_syslogger_file(buf.data, buf.len, LOG_DESTINATION_STDERR);
    }

    /* Write to CSV log if enabled */
    if (t_thrd.log_cxt.Log_destination & LOG_DESTINATION_CSVLOG) {
        if (t_thrd.postmaster_cxt.redirection_done || t_thrd.role == SYSLOGGER) {
            /*
             * send CSV data if it's safe to do so (syslogger doesn't need the
             * pipe). First get back the space in the message buffer.
             */
            pfree(buf.data);
            write_csvlog(edata);
        } else {
            /*
             * syslogger not up (yet), so just dump the message to stderr,
             * unless we already did so above.
             */
            if (!(t_thrd.log_cxt.Log_destination & LOG_DESTINATION_STDERR) &&
                t_thrd.postgres_cxt.whereToSendOutput != DestDebug)
                write_console(buf.data, buf.len);
            pfree(buf.data);
        }
    } else {
        pfree(buf.data);
    }
}

/* Write error report to server's log in a simple way without errstack */
void SimpleLogToServer(int elevel, bool silent, const char* fmt, ...)
{
    if (silent == true || !is_log_level_output(elevel, log_min_messages))
        return;

    ErrorData errdata;
    ErrorData* edata = NULL;
    MemoryContext oldcontext;

    /* Initialize a mostly-dummy error frame */
    edata = &errdata;
    errno_t rc = memset_s(edata, sizeof(ErrorData), 0, sizeof(ErrorData));
    securec_check(rc, "", "");
    /* the default text domain is the backend's */
    edata->domain = t_thrd.log_cxt.save_format_domain ? t_thrd.log_cxt.save_format_domain : PG_TEXTDOMAIN("postgres");
    /* set the errno to be used to interpret %m */
    edata->saved_errno = t_thrd.log_cxt.save_format_errnumber;

    edata->elevel = elevel;
    edata->mod_id = MOD_CN_RETRY;

    oldcontext = MemoryContextSwitchTo(ErrorContext);

    EVALUATE_MESSAGE(message, false, true);

    MemoryContextSwitchTo(oldcontext);

    send_message_to_server_log(edata);
}
/*
 * @Description: Write error report to server's log for stream thread
 *
 * @param: void
 * @return: void
 */
void stream_send_message_to_server_log(void)
{
    ErrorData* edata = &t_thrd.log_cxt.errordata[t_thrd.log_cxt.errordata_stack_depth];
    MemoryContext oldcontext;

    /*
     * Since cancel is always driven by Coordinator, internal-cancel message
     * of stream thread can be ignored to avoid message misorder.
     */
    if (edata->sqlerrcode == ERRCODE_QUERY_INTERNAL_CANCEL) {
        return;
    }

    t_thrd.log_cxt.recursion_depth++;
    CHECK_STACK_DEPTH();
    oldcontext = MemoryContextSwitchTo(ErrorContext);

    /* Send to server log, if enabled */
    if (edata->output_to_server && is_errmodule_enable(edata->elevel, edata->mod_id)) {
        send_message_to_server_log(edata);
    }

    MemoryContextSwitchTo(oldcontext);
    t_thrd.log_cxt.recursion_depth--;
}

/*
 * @Description: Write error report to client for stream thread
 *
 * @param: void
 * @return: void
 */
void stream_send_message_to_consumer(void)
{
    ErrorData* edata = &t_thrd.log_cxt.errordata[t_thrd.log_cxt.errordata_stack_depth];
    MemoryContext oldcontext;

    /*
     * Since cancel is always driven by Coordinator, internal-cancel message
     * of stream thread can be ignored to avoid message misorder.
     */
    t_thrd.log_cxt.recursion_depth++;
    CHECK_STACK_DEPTH();
    oldcontext = MemoryContextSwitchTo(ErrorContext);

    send_message_to_frontend(edata);

    MemoryContextSwitchTo(oldcontext);
    t_thrd.log_cxt.recursion_depth--;
}

/*
 * Send data to the syslogger using the chunked protocol
 *
 * Note: when there are multiple backends writing into the syslogger pipe,
 * it's critical that each write go into the pipe indivisibly, and not
 * get interleaved with data from other processes.	Fortunately, the POSIX
 * spec requires that writes to pipes be atomic so long as they are not
 * more than PIPE_BUF bytes long.  So we divide long messages into chunks
 * that are no more than that length, and send one chunk per write() call.
 * The collector process knows how to reassemble the chunks.
 *
 * Because of the atomic write requirement, there are only two possible
 * results from write() here: -1 for failure, or the requested number of
 * bytes.  There is not really anything we can do about a failure; retry would
 * probably be an infinite loop, and we can't even report the error usefully.
 * (There is noplace else we could send it!)  So we might as well just ignore
 * the result from write().  However, on some platforms you get a compiler
 * warning from ignoring write()'s result, so do a little dance with casting
 * rc to void to shut up the compiler.
 */
static void write_pipe_chunks(char* data, int len, int dest)
{
    LogPipeProtoChunk p;
    int fd = fileno(stderr);
    int rc;

    Assert(len > 0);

    p.proto.nuls[0] = p.proto.nuls[1] = '\0';
    p.proto.pid = t_thrd.proc_cxt.MyProcPid;
    p.proto.logtype = LOG_TYPE_ELOG;
    p.proto.magic = PROTO_HEADER_MAGICNUM;
    /* write all but the last chunk */
    while (len > LOGPIPE_MAX_PAYLOAD) {
        p.proto.is_last = (dest == LOG_DESTINATION_CSVLOG ? 'F' : 'f');
        p.proto.len = LOGPIPE_MAX_PAYLOAD;
        rc = memcpy_s(p.proto.data, LOGPIPE_MAX_PAYLOAD, data, LOGPIPE_MAX_PAYLOAD);
        securec_check(rc, "\0", "\0");
        rc = write(fd, &p, LOGPIPE_HEADER_SIZE + LOGPIPE_MAX_PAYLOAD);
        (void)rc;
        data += LOGPIPE_MAX_PAYLOAD;
        len -= LOGPIPE_MAX_PAYLOAD;
    }

    /* write the last chunk */
    p.proto.is_last = (dest == LOG_DESTINATION_CSVLOG ? 'T' : 't');
    p.proto.len = len;
    rc = memcpy_s(p.proto.data, len, data, len);
    securec_check(rc, "\0", "\0");
    rc = write(fd, &p, LOGPIPE_HEADER_SIZE + len);
    (void)rc;
}

/*
 * Append a text string to the error report being built for the client.
 *
 * This is ordinarily identical to pq_sendstring(), but if we are in
 * error recursion trouble we skip encoding conversion, because of the
 * possibility that the problem is a failure in the encoding conversion
 * subsystem itself.  Code elsewhere should ensure that the passed-in
 * strings will be plain 7-bit ASCII, and thus not in need of conversion,
 * in such cases.  (In particular, we disable localization of error messages
 * to help ensure that's true.)
 */
static void err_sendstring(StringInfo buf, const char* str)
{
    if (in_error_recursion_trouble()) {
        pq_send_ascii_string(buf, str);
    } else {
        pq_sendstring(buf, str);
    }
}

/* Get internal error code by the location(filename and lineno) of error message arised */
static int pg_geterrcode_byerrmsg(ErrorData* edata)
{
    unsigned int i = 0;
    unsigned int j = 0;
    const char* ext_name = NULL;

    if (edata == NULL) {
        return 0;
    }

    for (i = 0; i < lengthof(g_mppdb_errors); i++) {
        for (j = 0; j < lengthof(g_mppdb_errors[i].astErrLocate); j++) {
            if ((0 == strcmp(g_mppdb_errors[i].astErrLocate[j].szFileName, edata->filename)) &&
                (g_mppdb_errors[i].astErrLocate[j].ulLineno == (unsigned int)edata->lineno)) {
                return g_mppdb_errors[i].ulSqlErrcode;
            } else if (0 == strcmp(g_mppdb_errors[i].astErrLocate[j].szFileName, edata->filename)) {
                /* file name is valid or not */
                ext_name = strrchr(edata->filename, '.');
                if (ext_name == NULL) {
                    return 0;
                }

                /* *.l file */
                if ((*(ext_name + 1) == 'l') &&
                    ((g_mppdb_errors[i].astErrLocate[j].ulLineno + 1) == (unsigned int)edata->lineno)) {
                    return g_mppdb_errors[i].ulSqlErrcode;
                }
            }
        }
    }

    return 0;
}

/**
 * @Description: cn add all error info from dn
 * @in/out pErrData - remote error data from dn
 * @return - 0 is ok
 */
int combiner_errdata(RemoteErrorData* pErrData)
{
    ErrorData* edata = &t_thrd.log_cxt.errordata[t_thrd.log_cxt.errordata_stack_depth];

    /* we don't bother incrementing t_thrd.log_cxt.recursion_depth */
    CHECK_STACK_DEPTH();

    edata->internalerrcode = pErrData->internalerrcode;
    edata->filename = pErrData->filename;
    edata->funcname = pErrData->errorfuncname;
    edata->lineno = pErrData->lineno;
    edata->mod_id = pErrData->mod_id;

    return 0; /* return value does not matter */
}

/*
 * Write error report to client
 */
static void send_message_to_frontend(ErrorData* edata)
{
    StringInfoData msgbuf;

#ifndef USE_ASSERT_CHECKING
    /* Send too much detail to client is not allowed, stored them in system log is enough. */
    if (IS_PGXC_COORDINATOR && IsConnFromApp() && edata->elevel <= LOG) {
        return;
    }
#endif

    /*
     * Since cancel is always driven by Coordinator, internal-cancel message
     * of datanode postgres thread can be ignored to avoid libcomm waitting quota in here.
     * If a single node, always send message to front.
     *
     * Since the ('N') message is ignored in old PGXC handle_response, we
     * can simply ignore the message here if not marked by handle_in_client
     * when invoking ereport.
     * If u_sess->utils_cxt.qunit_case_number != 0, it(CN/DN) serves as a QUNIT backend thread, and it(CN/DN)
     * needs to send all ERROR messages to the client(gsql).
     */
    if ((IsConnFromCoord() || StreamThreadAmI()) && edata->elevel < ERROR && !edata->handle_in_client

#ifdef ENABLE_QUNIT
        && u_sess->utils_cxt.qunit_case_number == 0
#endif

    )
        return;

    /* 'N' (Notice) is for nonfatal conditions, 'E' is for errors */
    pq_beginmessage(&msgbuf, (edata->elevel < ERROR) ? 'N' : 'E');

    if (PG_PROTOCOL_MAJOR(FrontendProtocol) >= 3) {
        /* New style with separate fields */
        char tbuf[12] = {0};
        int ssval;
        int i;
        char vbuf[256] = {0};
        errno_t rc = 0;

        pq_sendbyte(&msgbuf, PG_DIAG_SEVERITY);
        err_sendstring(&msgbuf, error_severity(edata->elevel));

        /* get mpp internal errcode */
        if (edata->elevel >= ERROR) {
            if (edata->internalerrcode == 0 && edata->filename && edata->lineno > 0) {
                /*
                 * In case of error from MM module we skip getting the internal error code,
                 * since fdw classes are not scanned by scanEreport.cpp.
                 */
                if (IsMMEngineUsed()) {
                    edata->internalerrcode = ERRCODE_SUCCESSFUL_COMPLETION;
                } else {
                    edata->internalerrcode = pg_geterrcode_byerrmsg(edata);
                }
            }
        } else {
            edata->internalerrcode = ERRCODE_SUCCESSFUL_COMPLETION;
        }

        rc = snprintf_s(tbuf, sizeof(tbuf), sizeof(tbuf) - 1, "%d", edata->internalerrcode);
        securec_check_ss(rc, "\0", "\0");
        pq_sendbyte(&msgbuf, PG_DIAG_INTERNEL_ERRCODE);
        err_sendstring(&msgbuf, tbuf);

        /* M field is required per protocol, so always send something */
        pq_sendbyte(&msgbuf, PG_DIAG_MESSAGE_PRIMARY);

        /* Add node_name before error message */
        if (COORDINATOR_NOT_SINGLE && t_thrd.log_cxt.error_with_nodename) {
            appendStringInfoString(&msgbuf, g_instance.attr.attr_common.PGXCNodeName);
            appendStringInfoString(&msgbuf, ": ");
        }

        if (edata->message) {
            if (edata->verbose) {
                rc = snprintf_s(vbuf,
                    sizeof(vbuf),
                    sizeof(vbuf) - 1,
                    "(%s pid=%d)",
                    g_instance.attr.attr_common.PGXCNodeName,
                    getpid());
                securec_check_ss(rc, "\0", "\0");
            }

            /*
             * We treat FATAL as ERROR when reporting error message to consumer/Coordinator.
             * So add keyword '[FATAL]' before error message.
             */
            if (IS_PGXC_DATANODE && !IsConnFromApp() && edata->elevel == FATAL) {
                appendStringInfoString(&msgbuf, _("[FATAL] "));
            }

            appendStringInfoString(&msgbuf, edata->message);
            err_sendstring(&msgbuf, vbuf);
        } else
            err_sendstring(&msgbuf, _("missing error text"));

        /* unpack MAKE_SQLSTATE code */
        ssval = edata->sqlerrcode;
        for (i = 0; i < 5; i++) {
            tbuf[i] = PGUNSIXBIT(ssval);
            ssval >>= 6;
        }
        tbuf[i] = '\0';

        pq_sendbyte(&msgbuf, PG_DIAG_SQLSTATE);
        err_sendstring(&msgbuf, tbuf);

        if (edata->mod_id) {
            pq_sendbyte(&msgbuf, PG_DIAG_MODULE_ID);
            err_sendstring(&msgbuf, get_valid_module_name(edata->mod_id));
        }

        if (edata->detail) {
            pq_sendbyte(&msgbuf, PG_DIAG_MESSAGE_DETAIL);
            err_sendstring(&msgbuf, edata->detail);
        }

        /* detail_log is intentionally not used here */
        if (edata->hint) {
            pq_sendbyte(&msgbuf, PG_DIAG_MESSAGE_HINT);
            err_sendstring(&msgbuf, edata->hint);
        }

        if (edata->context) {
            pq_sendbyte(&msgbuf, PG_DIAG_CONTEXT);
            err_sendstring(&msgbuf, edata->context);
        }

        if (edata->cursorpos > 0) {
            rc = snprintf_s(tbuf, sizeof(tbuf), sizeof(tbuf) - 1, "%d", edata->cursorpos);
            securec_check_ss_c(rc, "\0", "\0");
            pq_sendbyte(&msgbuf, PG_DIAG_STATEMENT_POSITION);
            err_sendstring(&msgbuf, tbuf);
        }

        if (edata->internalpos > 0) {
            rc = snprintf_s(tbuf, sizeof(tbuf), sizeof(tbuf) - 1, "%d", edata->internalpos);
            securec_check_ss_c(rc, "\0", "\0");
            pq_sendbyte(&msgbuf, PG_DIAG_INTERNAL_POSITION);
            err_sendstring(&msgbuf, tbuf);
        }

        if (edata->internalquery) {
            char* mask_string = NULL;

            /* mask the query whenever including sensitive information. */
            mask_string = maskPassword(edata->internalquery);
            /* with nothing to mask, just use the source querystring. */
            if (mask_string == NULL)
                mask_string = edata->internalquery;

            pq_sendbyte(&msgbuf, PG_DIAG_INTERNAL_QUERY);
            err_sendstring(&msgbuf, mask_string);

            /* free the memory malloced for mask_string. */
            if (mask_string != edata->internalquery)
                pfree(mask_string);
        }

#if defined(USE_ASSERT_CHECKING) || defined(FASTCHECK)
        /* Send filename lineno and funcname to client is not allowed. */
        if (edata->filename) {
            pq_sendbyte(&msgbuf, PG_DIAG_SOURCE_FILE);
            err_sendstring(&msgbuf, edata->filename);
        }

        if (edata->lineno > 0) {
            rc = snprintf_s(tbuf, sizeof(tbuf), sizeof(tbuf) - 1, "%d", edata->lineno);
            securec_check_ss_c(rc, "\0", "\0");
            pq_sendbyte(&msgbuf, PG_DIAG_SOURCE_LINE);
            err_sendstring(&msgbuf, tbuf);
        }

        if (edata->funcname) {
            pq_sendbyte(&msgbuf, PG_DIAG_SOURCE_FUNCTION);
            err_sendstring(&msgbuf, edata->funcname);
        }

#endif

        pq_sendbyte(&msgbuf, '\0'); /* terminator */
    } else {
        /* Old style --- gin up a backwards-compatible message */
        StringInfoData buf;

        initStringInfo(&buf);

        appendStringInfo(&buf, "%s:  ", error_severity(edata->elevel));

        if (edata->show_funcname && edata->funcname)
            appendStringInfo(&buf, "%s: ", edata->funcname);

        if (edata->message)
            appendStringInfoString(&buf, edata->message);
        else
            appendStringInfoString(&buf, _("missing error text"));

        if (edata->cursorpos > 0)
            appendStringInfo(&buf, _(" at character %d"), edata->cursorpos);
        else if (edata->internalpos > 0)
            appendStringInfo(&buf, _(" at character %d"), edata->internalpos);

        appendStringInfoChar(&buf, '\n');

        err_sendstring(&msgbuf, buf.data);

        pfree(buf.data);
    }

    if (u_sess->stream_cxt.producer_obj &&
        STREAM_IS_LOCAL_NODE(u_sess->stream_cxt.producer_obj->getParallelDesc().distriType)) {
        gs_message_by_memory(
            &msgbuf, u_sess->stream_cxt.producer_obj->getSharedContext(), u_sess->stream_cxt.producer_obj->getNth());
    } else {
        pq_endmessage(&msgbuf);

        /*
         * This flush is normally not necessary, since postgres.c will flush out
         * waiting data when control returns to the main loop. But it seems best
         * to leave it here, so that the client has some clue what happened if the
         * backend dies before getting back to the main loop ... error/notice
         * messages should not be a performance-critical path anyway, so an extra
         * flush won't hurt much ...
         * if CN retry is enabled, we need to avoid flushing data to client before ReadyForQuery is called
         */
        if (STMT_RETRY_ENABLED && edata->elevel < ERROR && IS_PGXC_COORDINATOR &&
            !t_thrd.log_cxt.flush_message_immediately)
            return;

        pq_flush();

        if (edata->elevel == FATAL)
            t_thrd.log_cxt.flush_message_immediately = true;
    }
}

/*
 * Support routines for formatting error messages.
 *
 * expand_fmt_string --- process special format codes in a format string
 *
 * We must replace %m with the appropriate strerror string, since vsnprintf
 * won't know what to do with it.
 *
 * The result is a palloc'd string.
 */
static char* expand_fmt_string(const char* fmt, ErrorData* edata)
{
    StringInfoData buf;
    const char* cp = NULL;

    initStringInfo(&buf);

    for (cp = fmt; *cp; cp++) {
        if (cp[0] == '%' && cp[1] != '\0') {
            cp++;
            if (*cp == 'm') {
                /*
                 * Replace %m by system error string.  If there are any %'s in
                 * the string, we'd better double them so that vsnprintf won't
                 * misinterpret.
                 */
                const char* cp2 = NULL;

                cp2 = useful_strerror(edata->saved_errno);
                for (; *cp2; cp2++) {
                    if (*cp2 == '%')
                        appendStringInfoCharMacro(&buf, '%');
                    appendStringInfoCharMacro(&buf, *cp2);
                }
            } else {
                /* copy % and next char --- this avoids trouble with %%m */
                appendStringInfoCharMacro(&buf, '%');
                appendStringInfoCharMacro(&buf, *cp);
            }
        } else {
            appendStringInfoCharMacro(&buf, *cp);
        }
    }

    return buf.data;
}

/*
 * A slightly cleaned-up version of strerror()
 */
static const char* useful_strerror(int errnum)
{
    /* this buffer is only used if errno has a bogus value */
    char* errorstr_buf = t_thrd.buf_cxt.errorstr_buf;
    int size = sizeof(t_thrd.buf_cxt.errorstr_buf);
    const char* str = NULL;
    int infolen = 0;
    errno_t rc = EOK;

#ifdef WIN32
    /* Winsock error code range, per WinError.h */
    if (errnum >= 10000 && errnum <= 11999)
        return pgwin32_socket_strerror(errnum);
#endif
    str = gs_strerror(errnum);
    /*
     * Some strerror()s return an empty string for out-of-range errno. This is
     * ANSI C spec compliant, but not exactly useful.
     */
    if (str == NULL || *str == '\0') {
        infolen = strlen("operating system error ") + sizeof(int) + 1;
        rc = snprintf_s(errorstr_buf,
            size,
            infolen,
            /* ------
              translator: This string will be truncated at 47
              characters expanded. */
            _("operating system error %d"),
            errnum);
        securec_check_ss_c(rc, "\0", "\0");
        str = errorstr_buf;
    }

    return str;
}

/*
 * error_severity --- get localized string representing elevel
 */
static const char* error_severity(int elevel)
{
    const char* prefix = NULL;

    switch (elevel) {
        case DEBUG1:
        case DEBUG2:
        case DEBUG3:
        case DEBUG4:
        case DEBUG5:
            prefix = _("DEBUG");
            break;
        case LOG:
        case COMMERROR:
            prefix = _("LOG");
            break;
        case INFO:
            prefix = _("INFO");
            break;
        case NOTICE:
            prefix = _("NOTICE");
            break;
        case WARNING:
            prefix = _("WARNING");
            break;
        case ERROR:
            prefix = _("ERROR");
            break;
        case FATAL:
            prefix = _("FATAL");
            break;
        case PANIC:
            prefix = _("PANIC");
            break;
        default:
            prefix = "\?\?\?";
            break;
    }

    return prefix;
}

/*
 *	append_with_tabs
 *
 *	Append the string to the StringInfo buffer, inserting a tab after any
 *	newline.
 */
static void append_with_tabs(StringInfo buf, const char* str)
{
    char ch = 0;

    while ((ch = *str++) != '\0') {
        appendStringInfoCharMacro(buf, ch);
        if (ch == '\n') {
            appendStringInfoCharMacro(buf, '\t');
        }
    }
}

/*
 * Write errors to stderr (or by equal means when stderr is
 * not available). Used before ereport/elog can be used
 * safely (memory context, GUC load etc)
 */
void write_stderr(const char* fmt, ...)
{
    va_list ap;

#ifdef WIN32
    char errbuf[2048]; /* Arbitrary size? */
#endif

    fmt = _(fmt);

    va_start(ap, fmt);
#ifndef WIN32
    /* On Unix, we just fprintf to stderr */
    vfprintf(stderr, fmt, ap);
    fflush(stderr);
#else
    errno_t rc = vsnprintf_s(errbuf, sizeof(errbuf), sizeof(errbuf) - 1, fmt, ap);
    securec_check_ss_c(rc, "\0", "\0");
    /*
     * On Win32, we print to stderr if running on a console, or write to
     * eventlog if running as a service
     */
    if (pgwin32_is_service()) {
        /* Running as a service */
        write_eventlog(ERROR, errbuf, strlen(errbuf));
    } else {
        /* Not running as service, write to stderr */
        write_console(errbuf, strlen(errbuf));
        fflush(stderr);
    }
#endif
    va_end(ap);
}

/*
 * is_log_level_output -- is elevel logically >= log_min_level?
 *
 * We use this for tests that should consider LOG to sort out-of-order,
 * between ERROR and FATAL.  Generally this is the right thing for testing
 * whether a message should go to the postmaster log, whereas a simple >=
 * test is correct for testing whether the message should go to the client.
 */
static bool is_log_level_output(int elevel, int log_min_level)
{
    if (elevel == LOG || elevel == COMMERROR) {
        if (log_min_level == LOG || log_min_level <= ERROR) {
            return true;
        }
    } else if (elevel >= log_min_level) {
        /* Neither is log */
        return true;
    }

    return false;
}

/*
 * Adjust the level of a recovery-related message per u_sess->attr.attr_common.trace_recovery_messages.
 *
 * The argument is the default log level of the message, eg, DEBUG2.  (This
 * should only be applied to DEBUGn log messages, otherwise it's a no-op.)
 * If the level is >= u_sess->attr.attr_common.trace_recovery_messages, we return LOG, causing the
 * message to be logged unconditionally (for most settings of
 * log_min_messages).  Otherwise, we return the argument unchanged.
 * The message will then be shown based on the setting of log_min_messages.
 *
 * Intention is to keep this for at least the whole of the 9.0 production
 * release, so we can more easily diagnose production problems in the field.
 * It should go away eventually, though, because it's an ugly and
 * hard-to-explain kluge.
 */
int trace_recovery(int trace_level)
{
    if (trace_level < LOG && trace_level >= u_sess->attr.attr_common.trace_recovery_messages) {
        return LOG;
    }

    return trace_level;
}

void getElevelAndSqlstate(int* eLevel, int* sqlState)
{
    if (eLevel == NULL || sqlState == NULL) {
        return;
    }
    *eLevel = t_thrd.log_cxt.errordata[t_thrd.log_cxt.errordata_stack_depth].elevel;
    *sqlState = t_thrd.log_cxt.errordata[t_thrd.log_cxt.errordata_stack_depth].sqlerrcode;
}

char* maskPassword(const char* query_string)
{
    char* mask_string = NULL;

    if (t_thrd.log_cxt.on_mask_password)
        return NULL;

    t_thrd.log_cxt.on_mask_password = true;

    MemoryContext oldCxt = MemoryContextSwitchTo(t_thrd.mem_cxt.mask_password_mem_cxt);
    mask_string = mask_Password_internal(query_string);
    (void)MemoryContextSwitchTo(oldCxt);
    MemoryContextReset(t_thrd.mem_cxt.mask_password_mem_cxt);

    t_thrd.log_cxt.on_mask_password = false;

    return mask_string;
}

/*
 * Mask the password in statment CREATE ROLE, CREATE USER, ALTER ROLE, ALTER USER, CREATE GROUP
 * SET ROLE, CREATE DATABASE LINK, and some function
 */
static char* mask_Password_internal(const char* query_string)
{
    int i = 0;
    core_yyscan_t yyscanner;
    core_yy_extra_type yyextra;
    core_YYSTYPE yylval;
    YYLTYPE yylloc;
    int curr_token = 59; /* initialize prev_token as ';' */
    bool is_password = false;
    char* mask_string = NULL;
    const char* funcs[] = {"dblink_connect"}; /* the function list need mask */
    int funcNum = sizeof(funcs) / sizeof(funcs[0]);
    int position[16] = {0};
    int length[16] = {0};
    int idx = 0;
    bool is_create_func = false;
    bool is_child_stmt = false;
    errno_t rc = EOK;
    int truncateLen = 0; /* accumulate total length for each truncate */

    /* the functions need to mask all contents */
    const char* fun_crypt[] = {"gs_encrypt_aes128", "gs_decrypt_aes128"};
    int fun_crypt_num = sizeof(fun_crypt) / sizeof(fun_crypt[0]);
    bool is_crypt_func = false;
    int length_crypt = 0;
    int count_crypt = 0;
    int position_crypt = 0;

    /* functions whose second paramter will be masked as a child stmt. */
    const char* funcs2[] = {"exec_on_extension", "exec_hadoop_sql"};
    int funcNum2 = sizeof(funcs2) / sizeof(funcs2[0]);

    /* stmt type:
     * 0 - unknown type
     * 1 - create role
     * 2 - create user
     * 3 - alter role
     * 4 - alter user
     * 5 - create group
     * 6 - set role/session
     * 7 - create database link
     * 8 - exec function
     * 9 - create function or procedure
     * 10 - create/alter server; create/alter foreign table;
     * 11 - create/alter data source
     * 12 - for funcs2
     */
    int cur_stmt_type = 0;
    int prev_token[5] = {0};

    sigjmp_buf compile_sigjmp_buf;
    sigjmp_buf* save_exception_stack = t_thrd.log_cxt.PG_exception_stack;
    ErrorContextCallback* save_context_stack = t_thrd.log_cxt.error_context_stack;
    int save_stack_depth = t_thrd.log_cxt.errordata_stack_depth;
    int save_recursion_depth = t_thrd.log_cxt.recursion_depth;
    int save_interrupt_holdoff_count = t_thrd.int_cxt.InterruptHoldoffCount;
    bool save_escape_string_warning = u_sess->attr.attr_sql.escape_string_warning;
    bool need_clear_yylval = false;
    /* initialize the flex scanner */
    yyscanner = scanner_init(query_string, &yyextra, ScanKeywords, NumScanKeywords);
    yyextra.warnOnTruncateIdent = false;
    u_sess->attr.attr_sql.escape_string_warning = false;

    /* set t_thrd.log_cxt.recursion_depth to 0 for avoiding MemoryContextReset called */
    t_thrd.log_cxt.recursion_depth = 0;
    /* set t_thrd.log_cxt.error_context_stack to NULL for avoiding context callback called */
    t_thrd.log_cxt.error_context_stack = NULL;
    /* replace globe JUMP point, ensurance return here if syntex error */
    t_thrd.log_cxt.PG_exception_stack = &compile_sigjmp_buf;

    PG_TRY();
    {
        while (1) {
            prev_token[0] = curr_token;
            curr_token = core_yylex(&yylval, &yylloc, yyscanner);
            /*
             * curr_token = 0 means there are no token any more mainly for non-semicolon condition.
             * Just break here is enough as the query need masked have been masked, here need
             * comprehensive test validation in properly time.
             */
            if (curr_token == 0) {
                break;
            }

            /* For function procedure and anonymous blocks condition. */
            if (is_child_stmt) {
                is_child_stmt = false;
                if (curr_token == SCONST && (yylval.str != NULL) && (yylval.str[0] != '\0')) {
                    /*
                     * Actually erase single quotes which was originally expected
                     * to do on IMMEDIATE branch.
                     */
                    if (prev_token[0] == IMMEDIATE) {
                        erase_single_quotes(yylval.str);
                    }

                    char* childStmt = mask_Password_internal(yylval.str);
                    if (childStmt != NULL) {
                        if (mask_string == NULL) {
                            mask_string = MemoryContextStrdup(u_sess->top_mem_cxt, query_string);
                        }
                        if (unlikely(yyextra.literallen != (int)strlen(childStmt))) {
                            ereport(ERROR,
                                (errcode(ERRCODE_SYNTAX_ERROR), errmsg("parse error on statement %s.", childStmt)));
                        }
                        rc = memcpy_s(mask_string + yylloc + 1, yyextra.literallen, childStmt, yyextra.literallen);
                        securec_check(rc, "\0", "\0");
                        rc = memset_s(childStmt, yyextra.literallen, 0, yyextra.literallen);
                        securec_check(rc, "", "");
                        pfree(childStmt);
                    }
                    continue;
                }
            }

            /*
             * Password and function parameters is always SCONST or IDENT.
             * The token have been assigned consistent numbers according to
             * the order in gram.y(e.g. IDENT = 258 and SCONST = 260).
             */
            if (cur_stmt_type > 0 && cur_stmt_type != 12 && (curr_token == SCONST || curr_token == IDENT)) {
                if (unlikely(yylloc >= (int)strlen(query_string))) {
                    ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR), errmsg("parse error on query %s.", query_string)));
                }
                char ch = query_string[yylloc];
                position[idx] = yylloc;

                if (ch == '\'' || ch == '\"') {
                    ++position[idx];
                }
                length[idx] = strlen(yylval.str);
                ++idx;
                /*
                 *  use a fixed length of masked password.
                 *  For a matched token, position[idx] is query_string's position, but mask_string is truncated,
                 *  real position of mask_string be located at (position[idx] - truncateLen).
                 */
                if (idx == 16 || is_password) {
                    if (mask_string == NULL) {
                        mask_string = MemoryContextStrdup(u_sess->top_mem_cxt, query_string);
                    }
                    int maskLen = u_sess->attr.attr_security.Password_min_length;
                    for (i = 0; i < idx; ++i) {
                        /* while masking password, if password is't quoted by \' or \", 
                         * the len of password may be shorter than actual, 
                         * we need to find the start position of password word by looking forward.
                         */ 
                        char wordHead = position[i] > 0 ? query_string[position[i] - 1] : '\0';
                        if (is_password && wordHead != '\0' && wordHead != '\'' && wordHead != '\"') {
                            while (position[i] > 0 && !isspace(wordHead) && wordHead != '\'' && wordHead != '\"') {
                                position[i]--;
                                wordHead = query_string[position[i] - 1];
                            }
                            length[i] = strlen(query_string + position[i]);
                            /* if the last char is ';', we should keep it */
                            if (query_string[position[i] + length[i] - 1] == ';') {
                                length[i]--;
                            }
                        }
                        if (length[i] < maskLen) {
                            /* need more space. */
                            int plen = strlen(mask_string) + maskLen - length[i] + 1;
                            char* maskStrNew = (char*)selfpalloc0(plen);
                            rc = memcpy_s(maskStrNew, plen, mask_string, strlen(mask_string));
                            securec_check(rc, "\0", "\0");
                            selfpfree(mask_string);
                            mask_string = maskStrNew;
                        }

                        char* maskBegin = mask_string + position[i] - truncateLen;
                        int copySize = strlen(mask_string) - (position[i] - truncateLen) - length[i] + 1;
                        rc = memmove_s(maskBegin + maskLen, copySize, maskBegin + length[i], copySize);
                        securec_check(rc, "", "");
                        if (length[i] > maskLen) {
                            truncateLen += (length[i] - maskLen);
                        }
                        rc = memset_s(maskBegin, maskLen, '*', maskLen);
                        securec_check(rc, "", "");

                        need_clear_yylval = true;
                    }
                    if (need_clear_yylval) {
                        rc = memset_s(yylval.str, strlen(yylval.str), 0, strlen(yylval.str));
                        securec_check(rc, "\0", "\0");
                        rc = memset_s((char*)yylval.keyword, strlen(yylval.keyword), 0, strlen(yylval.keyword));
                        securec_check(rc, "\0", "\0");
                        need_clear_yylval = false;
                    }
                    idx = 0;
                    is_password = false;
                    if (cur_stmt_type == 10 || cur_stmt_type == 11) {
                        cur_stmt_type = 0;
                    }
                }
            }

            switch (curr_token) {
                case CREATE:
                case ALTER:
                case SET:
                    break;
                case ROLE:
                case SESSION:
                    if (cur_stmt_type > 0) {
                        break;
                    }

                    if (prev_token[0] == CREATE) {
                        cur_stmt_type = 1;
                    } else if (prev_token[0] == ALTER) {
                        cur_stmt_type = 3;
                    } else if (prev_token[0] == SET) {
                        cur_stmt_type = 6;
                    } else if (prev_token[1] == SET && (prev_token[0] == LOCAL || prev_token[0] == SESSION)) {
                        cur_stmt_type = 6;
                        prev_token[1] = 0;
                    }

                    break;
                case USER:
                    if (cur_stmt_type > 0) {
                        break;
                    }
                    if (prev_token[0] == CREATE) {
                        cur_stmt_type = 2;
                    }
                    else if (prev_token[0] == ALTER) {
                        cur_stmt_type = 4;
                    }
                    break;
                case LOCAL:  // set local role
                    if (prev_token[0] == SET) {
                        prev_token[1] = SET;
                    }
                    break;
                case GROUP_P:
                    if (cur_stmt_type > 0) {
                        break;
                    }
                    if (prev_token[0] == CREATE) {
                        cur_stmt_type = 5;
                    }
                    break;
                case DATABASE:
                    if (prev_token[0] == CREATE) {
                        prev_token[1] = CREATE;
                    }
                    break;
                case PASSWORD:
                    if (prev_token[1] == SERVER && prev_token[2] == OPTIONS) {
                        cur_stmt_type = 10;
                        curr_token = IDENT;
                    } else if (prev_token[1] == DATA_P && prev_token[2] == SOURCE_P && prev_token[3] == OPTIONS) {
                        /* For create/alter data source: sensitive opt is 'password' */
                        cur_stmt_type = 11;
                        curr_token = IDENT;
                    }
                    is_password = true;
                    idx = 0;
                    break;
                case BY:
                    is_password = (cur_stmt_type > 0 && prev_token[0] == IDENTIFIED);
                    if (is_password) {
                        idx = 0;
                    }
                    break;
                case REPLACE:
                    is_password = (cur_stmt_type == 3 || cur_stmt_type == 4);
                    if (is_password) {
                        idx = 0;
                    }
                    break;
                case FUNCTION:
                case PROCEDURE:
                    if (cur_stmt_type > 0) {
                        break;
                    }
                    if (prev_token[0] == CREATE || prev_token[0] == REPLACE) {
                        is_create_func = true;
                    }
                    break;
                case DO:
                    is_create_func = true;
                    if (is_create_func) {
                        is_create_func = false;
                        is_child_stmt = true;
                    }
                    break;
                case AS:
                case IS:
                    if (is_create_func) {
                        is_create_func = false;
                        is_child_stmt = true;
                    }
                    break;
                case IMMEDIATE:
                    if (cur_stmt_type > 0) {
                        break;
                    }
                    if (prev_token[0] == EXECUTE) {
                        is_child_stmt = true;
                        erase_single_quotes(yyextra.scanbuf + yylloc);
                    }
                    break;
                case 40: /* character '(' */
                    if (is_crypt_func) {
                        count_crypt++;
                    }
                    if (prev_token[0] == IDENT) {
                        /* first, check funcs[] */
                        for (i = 0; i < funcNum; ++i) {
                            if (pg_strcasecmp(yylval.str, funcs[i]) == 0) {
                                cur_stmt_type = 8;
                                break;
                            }
                        }
                        /* if found, just break; */
                        if (i < funcNum) {
                            break;
                        }

                        /* otherwise, check if it is in funcs2[] */
                        for (i = 0; i < funcNum2; i++) {
                            if (pg_strcasecmp(yylval.str, funcs2[i]) == 0) {
                                /* for funcs2, we will mask its second parameter as child stmt. */
                                is_child_stmt = false;
                                prev_token[1] = 40;
                                cur_stmt_type = 12;
                                break;
                            }
                        }

                        /* if found, just break; */
                        if (i < funcNum2) {
                            break;
                        }

                        /* otherwise, check if it is in fun_crypt[] */
                        for (i = 0; i < fun_crypt_num; i++) {
                            if (pg_strcasecmp(yylval.str, fun_crypt[i]) == 0) {
                                /* for fun_crypt, we will mask all contents in (). */
                                is_crypt_func = true;
                                cur_stmt_type = 8;
                                if (0 == count_crypt) {
                                    count_crypt++;
                                    position_crypt = yylloc + 1;
                                }
                                break;
                            }
                        }
                    }
                    break;
                case 41: /* character ')' */
                    if (is_crypt_func) {
                        count_crypt--;
                        if (count_crypt == 0) {
                            if (mask_string == NULL) {
                                mask_string = MemoryContextStrdup(u_sess->top_mem_cxt, query_string);
                            }
                            if (yylloc > position_crypt) {
                                int len_mask_string = strlen(mask_string);
                                rc = memset_s(mask_string + position_crypt,
                                    len_mask_string - position_crypt,
                                    '*',
                                    yylloc - position_crypt);
                                securec_check(rc, "\0", "\0");
                            }
                            is_crypt_func = false;
                            length_crypt = 0;
                            position_crypt = 0;
                        }
                    }
                    if (cur_stmt_type == 8) {
                        if (mask_string == NULL) {
                            mask_string = MemoryContextStrdup(u_sess->top_mem_cxt, query_string);
                        }
                        for (i = 0; i < idx; ++i) {
                            rc = memset_s(mask_string + position[i], length[i], '*', length[i]);
                            securec_check(rc, "", "");
                        }
                        idx = 0;
                        cur_stmt_type = 0;
                    }
                    /* for funcs2: exec_on_extension, exec_hadoop_sql */
                    if (cur_stmt_type == 12) {
                        cur_stmt_type = 0;
                        prev_token[1] = 0;
                    }
                    break;
                case 44: /* character ',' */
                    /* for mask funcs2 */
                    if (cur_stmt_type == 12) {
                        if (prev_token[1] == 40) {
                            /* only mask its second parameter as a child stmt. */
                            is_child_stmt = true;
                            break;
                        }
                    }
                    break;
                case 59: /* character ';' */
                    /*
                     * Since the sensitive data always follow 'password', 'identified by',
                     * and 'replace' syntax, and we do mask before. We can just finish
                     * the masking task and reset all the parameters when meet the end.
                     */
                    cur_stmt_type = 0;
                    is_password = false;
                    idx = 0;
                    break;
                case FOREIGN:
                    if (prev_token[0] == CREATE || prev_token[0] == ALTER) {
                        prev_token[1] = FOREIGN;
                    }
                    break;
                case TABLE:
                    if (prev_token[1] == FOREIGN) {
                        prev_token[2] = TABLE;
                    }
                    break;
                case SERVER:
                    if (prev_token[0] == CREATE || prev_token[0] == ALTER) {
                        prev_token[1] = SERVER;
                    }
                    break;
                case OPTIONS:
                    if (prev_token[1] == SERVER) {
                        prev_token[2] = OPTIONS;
                    } else if (prev_token[1] == FOREIGN && prev_token[2] == TABLE) {
                        prev_token[3] = OPTIONS;
                    } else if (prev_token[1] == DATA_P && prev_token[2] == SOURCE_P) {
                        prev_token[3] = OPTIONS;
                    }
                    break;
                /* For create/alter data source */
                case DATA_P:
                    if (prev_token[0] == CREATE || prev_token[0] == ALTER) {
                        prev_token[1] = DATA_P;
                    }
                    break;
                case SOURCE_P:
                    if (prev_token[1] == DATA_P) {
                        prev_token[2] = SOURCE_P;
                    }
                    break;
                case IDENT:
                    if ((prev_token[1] == SERVER && prev_token[2] == OPTIONS) ||
                        (prev_token[1] == FOREIGN && prev_token[2] == TABLE && prev_token[3] == OPTIONS)) {
                        if (pg_strcasecmp(yylval.str, "secret_access_key") == 0) {
                            /* create/alter server  */
                            cur_stmt_type = 10;
                        } else {
                            cur_stmt_type = 0;
                        }
                        idx = 0;
                    } else if (prev_token[1] == DATA_P && prev_token[2] == SOURCE_P && prev_token[3] == OPTIONS) {
                        /*
                         * For create/alter data source: sensitive opts are 'username' and 'password'.
                         * 'username' is marked here, while 'password' is marked as a standard Token, not here.
                         */
                        if (pg_strcasecmp(yylval.str, "username") == 0) {
                            cur_stmt_type = 11;
                        } else {
                            cur_stmt_type = 0;
                        }
                        idx = 0;
                    }
                    break;
                case SCONST:
                    /* create/alter server || create/alter data source: masked here */
                    if (cur_stmt_type == 10 || cur_stmt_type == 11) {
                        if (prev_token[0] == IDENT) {
                            if (mask_string == NULL) {
                                mask_string = MemoryContextStrdup(u_sess->top_mem_cxt, query_string);
                            }

                            rc = memset_s(mask_string + position[0], length[0], '*', length[0]);
                            securec_check(rc, "", "");
                            idx = 0;
                            cur_stmt_type = 0;
                        }
                    }
                    break;
                default:
                    break;
            }
        }
    }
    PG_CATCH();
    {
        for (i = save_stack_depth + 1; i <= t_thrd.log_cxt.errordata_stack_depth; ++i) {
            ErrorData* edata = &t_thrd.log_cxt.errordata[t_thrd.log_cxt.errordata_stack_depth];
            /* Now free up subsidiary data attached to stack entry, and release it */
            if (edata->message != NULL) {
                pfree_ext(edata->message);
            }
            if (edata->detail != NULL) {
                pfree_ext(edata->detail);
            }
            if (edata->detail_log != NULL) {
                pfree_ext(edata->detail_log);
            }
            if (edata->hint != NULL) {
                pfree_ext(edata->hint);
            }
            if (edata->context != NULL) {
                pfree_ext(edata->context);
            }
            if (edata->internalquery != NULL) {
                pfree_ext(edata->internalquery);
            }
            t_thrd.log_cxt.errordata_stack_depth--;
        }
    }
    PG_END_TRY();

    /* restore the globe jump, globe jump if encounter errors in compile */
    t_thrd.log_cxt.PG_exception_stack = save_exception_stack;
    t_thrd.log_cxt.error_context_stack = save_context_stack;
    t_thrd.log_cxt.recursion_depth = save_recursion_depth;
    t_thrd.int_cxt.InterruptHoldoffCount = save_interrupt_holdoff_count;
    u_sess->attr.attr_sql.escape_string_warning = save_escape_string_warning;

    if (yyextra.scanbuflen > 0) {
        rc = memset_s(yyextra.scanbuf, yyextra.scanbuflen, 0, yyextra.scanbuflen);
        securec_check(rc, "\0", "\0");
        pfree_ext(yyextra.scanbuf);
    }
    if (yyextra.literalalloc > 0) {
        rc = memset_s(yyextra.literalbuf, yyextra.literallen, 0, yyextra.literallen);
        securec_check(rc, "\0", "\0");
        pfree_ext(yyextra.literalbuf);
    }

    return mask_string;
}

static void erase_single_quotes(char* query_string)
{
    char* curr = NULL;
    int count = 0;
    bool inDoubleQuotes = false;

    curr = query_string;
    while (curr != NULL && curr[0] != '\0') {
        // 0x27 is symbol '\''
        if (*curr == 0x27 && !inDoubleQuotes) {
            *curr = ' ';
            ++count;
        } else if (*curr == ';') {
            if (count % 2 == 0)
                break;
        } else if (*curr == 0x22) {
            inDoubleQuotes = !inDoubleQuotes;
        }

        if (*curr == '|' && *(curr + 1) == '|') {
            *curr = ' ';
            *(curr + 1) = ' ';
        }

        curr++;
    }
}

/*
 * Report error according to the return value.
 * At the same time, we should free the space alloced by developers.
 */
void freeSecurityFuncSpace(char* charList, ...)
{
    va_list argptr;

    /* if the first parameter is not empty */
    if (strcmp(charList, "\0") != 0) {
        /* free the first charList */
        pfree_ext(charList);

        /* if have move charList */
        va_start(argptr, charList);
        while (true) {
            char* szBuf = va_arg(argptr, char*);
            if (strcmp(szBuf, "\0") == 0) /* empty string */
                break;
            pfree_ext(szBuf);
        }
        va_end(argptr);
    }

    return;
}

/*
 * @Description: mask part of espaced characters which may cause log injection attack.
 * @in source_str : the messages which need mask before write to syslog.
 * @return : non-return
 */
static void mask_espaced_character(char* source_str)
{
    /*
     * Our syslog is splited by new line, so we just mask the "\n" here temporarily.
     * Replace all the "\n" with "*" in the string.
     */
    char* match_pos = strstr(source_str, "\n");
    while (match_pos != NULL) {
        *match_pos = '*';
        match_pos = strstr(match_pos, "\n");
    }
    return;
}
