/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2021 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2006-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2007-2009 Sun Microsystems, Inc. All rights reserved.
 * Copyright (c) 2007-2017 Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2020      Geoffroy Vallee. All rights reserved.
 * Copyright (c) 2020      IBM Corporation.  All rights reserved.
 * Copyright (c) 2021-2022 Nanook Consulting.  All rights reserved.
 * Copyright (c) 2021      Amazon.com, Inc. or its affiliates.  All Rights
 *                         reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "src/include/constants.h"
#include "src/include/version.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef HAVE_STRINGS_H
#    include <strings.h>
#endif /* HAVE_STRINGS_H */
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif
#ifdef HAVE_SYS_PARAM_H
#    include <sys/param.h>
#endif
#include <ctype.h>
#include <errno.h>
#include <signal.h>
#ifdef HAVE_SYS_TYPES_H
#    include <sys/types.h>
#endif /* HAVE_SYS_TYPES_H */
#ifdef HAVE_SYS_WAIT_H
#    include <sys/wait.h>
#endif /* HAVE_SYS_WAIT_H */
#ifdef HAVE_SYS_TIME_H
#    include <sys/time.h>
#endif /* HAVE_SYS_TIME_H */
#include <fcntl.h>
#ifdef HAVE_SYS_STAT_H
#    include <sys/stat.h>
#endif
#ifdef HAVE_POLL_H
#    include <poll.h>
#endif

#include "src/event/event-internal.h"
#include "src/mca/base/base.h"
#include "src/mca/prteinstalldirs/prteinstalldirs.h"
#include "src/pmix/pmix-internal.h"
#include "src/threads/pmix_mutex.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_basename.h"
#include "src/util/prte_cmd_line.h"
#include "src/util/pmix_fd.h"
#include "src/util/pmix_os_path.h"
#include "src/util/output.h"
#include "src/util/pmix_path.h"
#include "src/util/pmix_printf.h"
#include "src/util/pmix_environ.h"
#include "src/util/pmix_getcwd.h"
#include "src/util/show_help.h"

#include "src/class/pmix_pointer_array.h"
#include "src/runtime/prte_progress_threads.h"

#include "prun.h"
#include "src/mca/errmgr/errmgr.h"
#include "src/mca/ess/base/base.h"
#include "src/mca/schizo/base/base.h"
#include "src/mca/state/state.h"
#include "src/prted/prted.h"
#include "src/runtime/prte_globals.h"
#include "src/runtime/runtime.h"

typedef struct {
    prte_pmix_lock_t lock;
    pmix_info_t *info;
    size_t ninfo;
} mylock_t;

static pmix_nspace_t spawnednspace;

static size_t evid = INT_MAX;
static pmix_proc_t myproc;
static bool forcibly_die = false;
static prte_event_t term_handler;
static int term_pipe[2];
static pmix_mutex_t prun_abort_inprogress_lock = PMIX_MUTEX_STATIC_INIT;
static bool verbose = false;
static pmix_list_t forwarded_signals;

static void abort_signal_callback(int signal);
static void clean_abort(int fd, short flags, void *arg);
static void signal_forward_callback(int signal);
static void epipe_signal_callback(int signal);

static void regcbfunc(pmix_status_t status, size_t ref, void *cbdata)
{
    prte_pmix_lock_t *lock = (prte_pmix_lock_t *) cbdata;
    PMIX_ACQUIRE_OBJECT(lock);
    evid = ref;
    PRTE_PMIX_WAKEUP_THREAD(lock);
}

static void opcbfunc(pmix_status_t status, void *cbdata)
{
    prte_pmix_lock_t *lock = (prte_pmix_lock_t *) cbdata;
    PMIX_ACQUIRE_OBJECT(lock);
    PRTE_PMIX_WAKEUP_THREAD(lock);
}

static void defhandler(size_t evhdlr_registration_id, pmix_status_t status,
                       const pmix_proc_t *source, pmix_info_t info[], size_t ninfo,
                       pmix_info_t *results, size_t nresults,
                       pmix_event_notification_cbfunc_fn_t cbfunc, void *cbdata)
{
    prte_pmix_lock_t *lock = NULL;
    size_t n;
    pmix_status_t rc;

    if (verbose) {
        prte_output(0, "PRUN: DEFHANDLER WITH STATUS %s(%d)", PMIx_Error_string(status), status);
    }

    if (PMIX_ERR_IOF_FAILURE == status) {
        pmix_proc_t target;
        pmix_info_t directive;

        /* tell PRTE to terminate our job */
        PMIX_LOAD_PROCID(&target, prte_process_info.myproc.nspace, PMIX_RANK_WILDCARD);
        PMIX_INFO_LOAD(&directive, PMIX_JOB_CTRL_KILL, NULL, PMIX_BOOL);
        rc = PMIx_Job_control_nb(&target, 1, &directive, 1, NULL, NULL);
        if (PMIX_SUCCESS != rc && PMIX_OPERATION_SUCCEEDED != rc) {
            PMIx_tool_finalize();
            /* exit with a non-zero status */
            exit(1);
        }
        goto progress;
    }

    if (PMIX_ERR_UNREACH == status || PMIX_ERR_LOST_CONNECTION == status) {
        /* we should always have info returned to us - if not, there is
         * nothing we can do */
        if (NULL != info) {
            for (n = 0; n < ninfo; n++) {
                if (PMIX_CHECK_KEY(&info[n], PMIX_EVENT_RETURN_OBJECT)) {
                    lock = (prte_pmix_lock_t *) info[n].value.data.ptr;
                }
            }
        }

        if (NULL == lock) {
            exit(1);
        }
        /* save the status */
        lock->status = status;
        /* release the lock */
        PRTE_PMIX_WAKEUP_THREAD(lock);
    }
progress:
    /* we _always_ have to execute the evhandler callback or
     * else the event progress engine will hang */
    if (NULL != cbfunc) {
        cbfunc(PMIX_EVENT_ACTION_COMPLETE, NULL, 0, NULL, NULL, cbdata);
    }
}

static void evhandler(size_t evhdlr_registration_id, pmix_status_t status,
                      const pmix_proc_t *source, pmix_info_t info[], size_t ninfo,
                      pmix_info_t *results, size_t nresults,
                      pmix_event_notification_cbfunc_fn_t cbfunc, void *cbdata)
{
    prte_pmix_lock_t *lock = NULL;
    int jobstatus = 0;
    pmix_nspace_t jobid = {0};
    size_t n;
    char *msg = NULL;

    if (verbose) {
        prte_output(0, "PRUN: EVHANDLER WITH STATUS %s(%d)", PMIx_Error_string(status), status);
    }

    /* we should always have info returned to us - if not, there is
     * nothing we can do */
    if (NULL != info) {
        for (n = 0; n < ninfo; n++) {
            if (0 == strncmp(info[n].key, PMIX_JOB_TERM_STATUS, PMIX_MAX_KEYLEN)) {
                jobstatus = prte_pmix_convert_status(info[n].value.data.status);
            } else if (0 == strncmp(info[n].key, PMIX_EVENT_AFFECTED_PROC, PMIX_MAX_KEYLEN)) {
                PMIX_LOAD_NSPACE(jobid, info[n].value.data.proc->nspace);
            } else if (0 == strncmp(info[n].key, PMIX_EVENT_RETURN_OBJECT, PMIX_MAX_KEYLEN)) {
                lock = (prte_pmix_lock_t *) info[n].value.data.ptr;
            } else if (0 == strncmp(info[n].key, PMIX_EVENT_TEXT_MESSAGE, PMIX_MAX_KEYLEN)) {
                msg = info[n].value.data.string;
            }
        }
        if (verbose && PMIX_CHECK_NSPACE(jobid, spawnednspace)) {
            prte_output(0, "JOB %s COMPLETED WITH STATUS %d", PRTE_JOBID_PRINT(jobid), jobstatus);
        }
    }
    if (NULL != lock) {
        /* save the status */
        lock->status = jobstatus;
        if (NULL != msg) {
            lock->msg = strdup(msg);
        }
        /* release the lock */
        PRTE_PMIX_WAKEUP_THREAD(lock);
    }

    /* we _always_ have to execute the evhandler callback or
     * else the event progress engine will hang */
    if (NULL != cbfunc) {
        cbfunc(PMIX_EVENT_ACTION_COMPLETE, NULL, 0, NULL, NULL, cbdata);
    }
}

static void debug_cbfunc(size_t evhdlr_registration_id, pmix_status_t status,
                         const pmix_proc_t *source, pmix_info_t info[], size_t ninfo,
                         pmix_info_t *results, size_t nresults,
                         pmix_event_notification_cbfunc_fn_t cbfunc, void *cbdata)
{
    /* we _always_ have to execute the evhandler callback or
     * else the event progress engine will hang */
    if (NULL != cbfunc) {
        cbfunc(PMIX_EVENT_ACTION_COMPLETE, NULL, 0, NULL, NULL, cbdata);
    }
}

static void setupcbfunc(pmix_status_t status, pmix_info_t info[], size_t ninfo,
                        void *provided_cbdata, pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    mylock_t *mylock = (mylock_t *) provided_cbdata;
    size_t n;

    if (NULL != info) {
        mylock->ninfo = ninfo;
        PMIX_INFO_CREATE(mylock->info, mylock->ninfo);
        /* cycle across the provided info */
        for (n = 0; n < ninfo; n++) {
            PMIX_INFO_XFER(&mylock->info[n], &info[n]);
        }
    } else {
        mylock->info = NULL;
        mylock->ninfo = 0;
    }

    /* release the caller */
    if (NULL != cbfunc) {
        cbfunc(PMIX_SUCCESS, cbdata);
    }

    PRTE_PMIX_WAKEUP_THREAD(&mylock->lock);
}

int prun(int argc, char *argv[])
{
    int rc = 1, i;
    char *param, *ptr, *cptr, **options;
    prte_pmix_lock_t lock, rellock;
    pmix_list_t apps;
    prte_pmix_app_t *app;
    void *tinfo, *jinfo;
    pmix_info_t info, *iptr;
    pmix_proc_t pname;
    pmix_status_t ret;
    bool flag;
    size_t n, ninfo;
    pmix_app_t *papps = NULL;
    size_t napps;
    mylock_t mylock;
    uint32_t ui32;
    pid_t pid;
    char **pargv, **targv;
    int pargc;
    prte_ess_base_signal_t *sig;
    prte_event_list_item_t *evitm;
    pmix_value_t *val;
    pmix_data_array_t darray;
    prte_schizo_base_module_t *schizo;
    char hostname[PRTE_PATH_MAX];
    pmix_rank_t rank;
    pmix_status_t code;
    char *outdir = NULL;
    char *outfile = NULL;
    char *personality;
    pmix_proc_t parent;
    pmix_cli_result_t results;
    pmix_cli_item_t *opt;

    /* init the globals */
    PMIX_CONSTRUCT(&apps, pmix_list_t);
    PMIX_CONSTRUCT(&forwarded_signals, pmix_list_t);
    prte_tool_basename = pmix_basename(argv[0]);
    prte_tool_actual = "prun";
    pargc = argc;
    pargv = pmix_argv_copy_strip(argv);  // strip any quoted arguments
    gethostname(hostname, sizeof(hostname));

    /* because we have to use the schizo framework and init our hostname
     * prior to parsing the incoming argv for cmd line options, do a hacky
     * search to support passing of impacted options (e.g., verbosity for schizo) */
    rc = prte_schizo_base_parse_prte(pargc, 0, pargv, NULL);
    if (PRTE_SUCCESS != rc) {
        return rc;
    }
    rc = prte_schizo_base_parse_pmix(pargc, 0, pargv, NULL);
    if (PRTE_SUCCESS != rc) {
        return rc;
    }

    /* init the tiny part of PRTE we use */
    prte_init_util(PRTE_PROC_TYPE_NONE);

    /** setup callbacks for abort signals - from this point
     * forward, we need to abort in a manner that allows us
     * to cleanup. However, we cannot directly use libevent
     * to trap these signals as otherwise we cannot respond
     * to them if we are stuck in an event! So instead use
     * the basic POSIX trap functions to handle the signal,
     * and then let that signal handler do some magic to
     * avoid the hang
     *
     * NOTE: posix traps don't allow us to do anything major
     * in them, so use a pipe tied to a libevent event to
     * reach a "safe" place where the termination event can
     * be created
     */
    if (0 != (rc = pipe(term_pipe))) {
        fprintf(stderr, "Failed to create pipe\n");
        exit(1);
    }
    /* setup an event to attempt normal termination on signal */
    rc = prte_event_base_open();
    if (PRTE_SUCCESS != rc) {
        fprintf(stderr, "Unable to initialize event library\n");
        exit(1);
    }
    prte_event_set(prte_event_base, &term_handler, term_pipe[0], PRTE_EV_READ, clean_abort, NULL);
    prte_event_add(&term_handler, NULL);

    /* Set both ends of this pipe to be close-on-exec so that no
       children inherit it */
    if (pmix_fd_set_cloexec(term_pipe[0]) != PRTE_SUCCESS
        || pmix_fd_set_cloexec(term_pipe[1]) != PRTE_SUCCESS) {
        fprintf(stderr, "unable to set the pipe to CLOEXEC\n");
        prte_progress_thread_finalize(NULL);
        exit(1);
    }

    /* point the signal trap to a function that will activate that event */
    signal(SIGTERM, abort_signal_callback);
    signal(SIGINT, abort_signal_callback);
    signal(SIGHUP, abort_signal_callback);

    /* setup callback for SIGPIPE */
    signal(SIGPIPE, epipe_signal_callback);

    /* open the SCHIZO framework */
    rc = prte_mca_base_framework_open(&prte_schizo_base_framework,
                                      PRTE_MCA_BASE_OPEN_DEFAULT);
    if (PRTE_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
        return rc;
    }

    if (PRTE_SUCCESS != (rc = prte_schizo_base_select())) {
        PRTE_ERROR_LOG(rc);
        return rc;
    }

    /* look for any personality specification */
    personality = NULL;
    for (i = 0; NULL != argv[i]; i++) {
        if (0 == strcmp(argv[i], "--personality")) {
            personality = argv[i + 1];
            break;
        }
    }

    /* detect if we are running as a proxy and select the active
     * schizo module for this tool */
    schizo = prte_schizo_base_detect_proxy(personality);
    if (NULL == schizo) {
        prte_show_help("help-schizo-base.txt", "no-proxy", true, prte_tool_basename, personality);
        return 1;
    }
    if (NULL == personality) {
        personality = schizo->name;
    }

    /* parse the input argv to get values, including everyone's MCA params */
    PMIX_CONSTRUCT(&results, pmix_cli_result_t);
    rc = schizo->parse_cli(pargv, &results, PMIX_CLI_WARN);
    if (PRTE_SUCCESS != rc) {
        PMIX_DESTRUCT(&results);
        if (PRTE_ERR_SILENT != rc) {
            fprintf(stderr, "%s: command line error (%s)\n", prte_tool_basename, prte_strerror(rc));
        } else {
            rc = PRTE_SUCCESS;
        }
        return rc;
    }

    /* check if we are running as root - if we are, then only allow
     * us to proceed if the allow-run-as-root flag was given. Otherwise,
     * exit with a giant warning message
     */
    if (0 == geteuid()) {
        schizo->allow_run_as_root(&results); // will exit us if not allowed
    }

    /** setup callbacks for signals we should forward */
    PMIX_CONSTRUCT(&prte_ess_base_signals, pmix_list_t);
    opt = pmix_cmd_line_get_param(&results, "forward-signals");
    if (NULL != opt) {
        param = opt->values[0];
    } else {
        param = NULL;
    }
    if (PRTE_SUCCESS != (rc = prte_ess_base_setup_signals(param))) {
        return rc;
    }
    PMIX_LIST_FOREACH(sig, &prte_ess_base_signals, prte_ess_base_signal_t)
    {
        signal(sig->signal, signal_forward_callback);
    }

    /* setup the job data global table */
    prte_job_data = PMIX_NEW(pmix_pointer_array_t);
    ret = pmix_pointer_array_init(prte_job_data, PRTE_GLOBAL_ARRAY_BLOCK_SIZE,
                                          PRTE_GLOBAL_ARRAY_MAX_SIZE,
                                  PRTE_GLOBAL_ARRAY_BLOCK_SIZE);
    if (PRTE_SUCCESS != ret) {
        PRTE_ERROR_LOG(ret);
        return rc;
    }

    /* setup options */
    PMIX_INFO_LIST_START(tinfo);

    /* tell PMIx what our name should be */
    if (NULL != (param = getenv("PMIX_NAMESPACE"))) {
        PMIX_INFO_LIST_ADD(ret, tinfo, PMIX_TOOL_NSPACE, param, PMIX_STRING);
    } else {
        pmix_asprintf(&param, "%s.%s.%lu", prte_tool_basename, hostname, (unsigned long)getpid());
        PMIX_INFO_LIST_ADD(ret, tinfo, PMIX_TOOL_NSPACE, param, PMIX_STRING);
        free(param);
    }
    if (NULL != (param = getenv("PMIX_RANK"))) {
        rank = strtoul(param, NULL, 10);
    } else {
        rank = 0;
    }
    PMIX_INFO_LIST_ADD(ret, tinfo, PMIX_TOOL_RANK, &rank, PMIX_PROC_RANK);

    if (pmix_cmd_line_is_taken(&results, "do-not-connect")) {
        PMIX_INFO_LIST_ADD(ret, tinfo, PMIX_TOOL_DO_NOT_CONNECT, NULL, PMIX_BOOL);
    } else if (pmix_cmd_line_is_taken(&results, "system-server-first")) {
        PMIX_INFO_LIST_ADD(ret, tinfo, PMIX_CONNECT_SYSTEM_FIRST, NULL, PMIX_BOOL);
    } else if (pmix_cmd_line_is_taken(&results, "system-server-only")) {
        PMIX_INFO_LIST_ADD(ret, tinfo, PMIX_CONNECT_TO_SYSTEM, NULL, PMIX_BOOL);
    }

    opt = pmix_cmd_line_get_param(&results, "wait-to-connect");
    if (NULL != opt) {
        ui32 = strtol(opt->values[0], NULL, 10);
        PMIX_INFO_LIST_ADD(ret, tinfo, PMIX_CONNECT_RETRY_DELAY, &ui32, PMIX_UINT32);
    }

    opt = pmix_cmd_line_get_param(&results, "num-connect-retries");
    if (NULL != opt) {
        ui32 = strtol(opt->values[0], NULL, 10);
        PMIX_INFO_LIST_ADD(ret, tinfo, PMIX_CONNECT_MAX_RETRIES, &ui32, PMIX_UINT32);
    }

    opt = pmix_cmd_line_get_param(&results, "pid");
    if (NULL != opt) {
        /* see if it is an integer value */
        char *leftover;
        leftover = NULL;
        pid = strtol(opt->values[0], &leftover, 10);
        if (NULL == leftover || 0 == strlen(leftover)) {
            /* it is an integer */
            PMIX_INFO_LIST_ADD(ret, tinfo, PMIX_SERVER_PIDINFO, &pid, PMIX_PID);
        } else if (0 == strncasecmp(opt->values[0], "file", 4)) {
            FILE *fp;
            /* step over the file: prefix */
            param = strchr(opt->values[0], ':');
            if (NULL == param) {
                /* malformed input */
                prte_show_help("help-prun.txt", "bad-option-input", true, prte_tool_basename,
                               "--pid", opt->values[0], "file:path");
                return PRTE_ERR_BAD_PARAM;
            }
            ++param;
            fp = fopen(param, "r");
            if (NULL == fp) {
                prte_show_help("help-prun.txt", "file-open-error", true, prte_tool_basename,
                               "--pid", opt->values[0], param);
                return PRTE_ERR_BAD_PARAM;
            }
            rc = fscanf(fp, "%lu", (unsigned long *) &pid);
            if (1 != rc) {
                /* if we were unable to obtain the single conversion we
                 * require, then error out */
                prte_show_help("help-prun.txt", "bad-file", true, prte_tool_basename,
                               "--pid", opt->values[0], param);
                fclose(fp);
                return PRTE_ERR_BAD_PARAM;
            }
            fclose(fp);
            PMIX_INFO_LIST_ADD(ret, tinfo, PMIX_SERVER_PIDINFO, &pid, PMIX_PID);
        } else { /* a string that's neither an integer nor starts with 'file:' */
                prte_show_help("help-prun.txt", "bad-option-input", true,
                               prte_tool_basename, "--pid",
                               opt->values[0], "file:path");
                return PRTE_ERR_BAD_PARAM;
        }
    }
    opt = pmix_cmd_line_get_param(&results, "namespace");
    if (NULL != opt) {
        PMIX_INFO_LIST_ADD(ret, tinfo, PMIX_SERVER_NSPACE, opt->values[0], PMIX_STRING);
    }

    /* set our session directory to something hopefully unique so
     * our rendezvous files don't conflict with other prun/prte
     * instances */
    pmix_asprintf(&ptr, "%s/%s.session.%s.%lu.%lu", pmix_tmp_directory(), prte_tool_basename,
                  prte_process_info.nodename, (unsigned long) geteuid(), (unsigned long) getpid());
    PMIX_INFO_LIST_ADD(ret, tinfo, PMIX_SERVER_TMPDIR, ptr, PMIX_STRING);
    free(ptr);

    /* we are also a launcher, so pass that down so PMIx knows
     * to setup rendezvous points */
    PMIX_INFO_LIST_ADD(ret, tinfo, PMIX_LAUNCHER, NULL, PMIX_BOOL);

    /* we always support tool rendezvous */
    PMIX_INFO_LIST_ADD(ret, tinfo, PMIX_SERVER_TOOL_SUPPORT, NULL, PMIX_BOOL);

    /* if they specified the URI, then pass it along */
    opt = pmix_cmd_line_get_param(&results, "dvm-uri");
    if (NULL != opt) {
        PMIX_INFO_LIST_ADD(ret, tinfo, PMIX_SERVER_URI, opt->values[0], PMIX_STRING);
    }

    /* output all IOF */
    PMIX_INFO_LIST_ADD(ret, tinfo, PMIX_IOF_LOCAL_OUTPUT, NULL, PMIX_BOOL);

    /* convert to array of info */
    PMIX_INFO_LIST_CONVERT(ret, tinfo, &darray);
    iptr = (pmix_info_t *) darray.array;
    ninfo = darray.size;
    PMIX_INFO_LIST_RELEASE(tinfo);

    /* now initialize PMIx */
    if (PMIX_SUCCESS != (ret = PMIx_tool_init(&myproc, iptr, ninfo))) {
        fprintf(stderr, "%s failed to initialize, likely due to no DVM being available\n",
                prte_tool_basename);
        exit(1);
    }
    PMIX_INFO_FREE(iptr, ninfo);

    /* register a default event handler and pass it our release lock
     * so we can cleanly exit if the server goes away */
    PRTE_PMIX_CONSTRUCT_LOCK(&rellock);
    PMIX_INFO_CREATE(iptr, 2);
    PMIX_INFO_LOAD(&iptr[1], PMIX_EVENT_RETURN_OBJECT, &rellock, PMIX_POINTER);
    PMIX_INFO_LOAD(&iptr[0], PMIX_EVENT_HDLR_NAME, "DEFAULT", PMIX_STRING);
    PRTE_PMIX_CONSTRUCT_LOCK(&lock);
    PMIx_Register_event_handler(NULL, 0, iptr, 2, defhandler, regcbfunc, &lock);
    PRTE_PMIX_WAIT_THREAD(&lock);
    PRTE_PMIX_DESTRUCT_LOCK(&lock);
    PMIX_INFO_FREE(iptr, 2);

    /***** CONSTRUCT THE APP'S JOB-INFO ****/
    PMIX_INFO_LIST_START(jinfo);
    PMIX_LOAD_PROCID(&parent, prte_process_info.myproc.nspace, prte_process_info.myproc.rank);

    /***** CHECK FOR LAUNCH DIRECTIVES - ADD THEM TO JOB INFO IF FOUND ****/
    PMIX_LOAD_PROCID(&pname, myproc.nspace, PMIX_RANK_WILDCARD);
    PMIX_INFO_LOAD(&info, PMIX_OPTIONAL, NULL, PMIX_BOOL);
    ret = PMIx_Get(&pname, PMIX_LAUNCH_DIRECTIVES, &info, 1, &val);
    PMIX_INFO_DESTRUCT(&info);
    if (PMIX_SUCCESS == ret) {
        iptr = (pmix_info_t *) val->data.darray->array;
        ninfo = val->data.darray->size;
        for (n = 0; n < ninfo; n++) {
            PMIX_INFO_LIST_XFER(ret, jinfo, &iptr[n]);
        }
        PMIX_VALUE_RELEASE(val);
    }

    /* we want to be notified upon job completion */
    PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_NOTIFY_COMPLETION, &flag, PMIX_BOOL);

    /* pass the personality */
    PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_PERSONALITY, personality, PMIX_STRING);

    /* get display options */
    opt = pmix_cmd_line_get_param(&results, "display");
    if (NULL != opt) {
        for (n=0; NULL != opt->values[n]; n++) {
            targv = pmix_argv_split(opt->values[n], ',');

            for (int idx = 0; idx < pmix_argv_count(targv); idx++) {
                if (0 == strncasecmp(targv[idx], "allocation", strlen(targv[idx]))) {
                    PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_MAPBY, ":DISPLAYALLOC", PMIX_STRING);
                }
                if (0 == strcasecmp(targv[idx], "map")) {
                    PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_MAPBY, ":DISPLAY", PMIX_STRING);
                }
                if (0 == strncasecmp(targv[idx], "bind", strlen(targv[idx]))) {
                    PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_BINDTO, ":REPORT", PMIX_STRING);
                }
                if (0 == strcasecmp(targv[idx], "map-devel")) {
                    PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_MAPBY, ":DISPLAYDEVEL", PMIX_STRING);
                }
                if (0 == strncasecmp(targv[idx], "topo", strlen(targv[idx]))) {
                    PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_MAPBY, ":DISPLAYTOPO", PMIX_STRING);
                }
            }
            pmix_argv_free(targv);
        }
    }

    /* cannot have both files and directory set for output */
    outdir = NULL;
    outfile = NULL;
    opt = pmix_cmd_line_get_param(&results, "output");
    if (NULL != opt) {
        for (n=0; NULL != opt->values[n]; n++) {
            targv = pmix_argv_split(opt->values[n], ',');

            for (int idx = 0; NULL != targv[idx]; idx++) {
                /* check for qualifiers */
                cptr = strchr(targv[idx], ':');
                if (NULL != cptr) {
                    *cptr = '\0';
                    ++cptr;
                    /* could be multiple qualifiers, so separate them */
                    options = pmix_argv_split(cptr, ',');
                    for (int m=0; NULL != options[m]; m++) {
                        if (0 == strcasecmp(options[m], "nocopy")) {
                            PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_IOF_FILE_ONLY, NULL, PMIX_BOOL);
                        } else if (0 == strcasecmp(options[m], "pattern")) {
                            PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_IOF_FILE_PATTERN, NULL, PMIX_BOOL);
    #ifdef PMIX_IOF_OUTPUT_RAW
                        } else if (0 == strcasecmp(options[m], "raw")) {
                            PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_IOF_OUTPUT_RAW, NULL, PMIX_BOOL);
    #endif
                        }
                    }
                    pmix_argv_free(options);
                }
                if (0 == strlen(targv[idx])) {
                    // only qualifiers were given
                    continue;
                }
                /* remove any '=' sign in the directive */
                if (NULL != (ptr = strchr(targv[idx], '='))) {
                    *ptr = '\0';
                    ++ptr;
                }
                if (0 == strncasecmp(targv[idx], "tag", strlen(targv[idx]))) {
                    PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_IOF_TAG_OUTPUT, &flag, PMIX_BOOL);
                }
                if (0 == strncasecmp(targv[idx], "timestamp", strlen(targv[idx]))) {
                    PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_IOF_TIMESTAMP_OUTPUT, &flag, PMIX_BOOL);
                }
                if (0 == strncasecmp(targv[idx], "xml", strlen(targv[idx]))) {
                    PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_IOF_XML_OUTPUT, NULL, PMIX_BOOL);
                }
                if (0 == strncasecmp(targv[idx], "merge-stderr-to-stdout", strlen(targv[idx]))) {
                    PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_IOF_MERGE_STDERR_STDOUT, &flag, PMIX_BOOL);
                }
                if (0 == strncasecmp(targv[idx], "directory", strlen(targv[idx]))) {
                    if (NULL != outfile) {
                        prte_show_help("help-prted.txt", "both-file-and-dir-set", true, outfile, outdir);
                        return PRTE_ERR_FATAL;
                    }
                    if (NULL == ptr) {
                        prte_show_help("help-prte-rmaps-base.txt",
                                       "missing-qualifier", true,
                                       "output", "directory", "directory");
                        return PRTE_ERR_FATAL;
                    }
                    /* If the given filename isn't an absolute path, then
                     * convert it to one so the name will be relative to
                     * the directory where prun was given as that is what
                     * the user will have seen */
                    if (!pmix_path_is_absolute(ptr)) {
                        char cwd[PRTE_PATH_MAX];
                        if (NULL == getcwd(cwd, sizeof(cwd))) {
                            PRTE_UPDATE_EXIT_STATUS(PRTE_ERR_FATAL);
                            goto DONE;
                        }
                        outdir = pmix_os_path(false, cwd, ptr, NULL);
                    } else {
                        outdir = strdup(ptr);
                    }
                    PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_IOF_OUTPUT_TO_DIRECTORY, outdir, PMIX_STRING);
                }
                if (0 == strncasecmp(targv[idx], "file", strlen(targv[idx]))) {
                    if (NULL != outdir) {
                        prte_show_help("help-prted.txt", "both-file-and-dir-set", true, outfile, outdir);
                        return PRTE_ERR_FATAL;
                    }
                    if (NULL == ptr) {
                        prte_show_help("help-prte-rmaps-base.txt",
                                       "missing-qualifier", true,
                                       "output", "filename", "filename");
                        return PRTE_ERR_FATAL;
                    }
                    /* If the given filename isn't an absolute path, then
                     * convert it to one so the name will be relative to
                     * the directory where prun was given as that is what
                     * the user will have seen */
                    if (!pmix_path_is_absolute(ptr)) {
                        char cwd[PRTE_PATH_MAX];
                        if (NULL == getcwd(cwd, sizeof(cwd))) {
                            PRTE_UPDATE_EXIT_STATUS(PRTE_ERR_FATAL);
                            goto DONE;
                        }
                        outfile = pmix_os_path(false, cwd, ptr, NULL);
                    } else {
                        outfile = strdup(ptr);
                    }
                    PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_IOF_OUTPUT_TO_FILE, outfile, PMIX_STRING);
                }
            }
            pmix_argv_free(targv);
        }
    }
    if (NULL != outdir) {
        free(outdir);
    }
    if (NULL != outfile) {
        free(outfile);
    }

    /* check what user wants us to do with stdin */
    opt = pmix_cmd_line_get_param(&results, PRTE_CLI_STDIN);
    if (NULL != opt) {
        PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_STDIN_TGT, opt->values[0], PMIX_STRING);
    }

    opt = pmix_cmd_line_get_param(&results, PRTE_CLI_MAPBY);
    if (NULL != opt) {
        PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_MAPBY, opt->values[0], PMIX_STRING);
        if (NULL != strcasestr(opt->values[0], "DONOTLAUNCH")) {
            PMIX_INFO_LIST_ADD(ret, jinfo, "PRTE_JOB_DO_NOT_LAUNCH", NULL, PMIX_BOOL);
        }
    }

    /* if the user specified a ranking policy, then set it */
    opt = pmix_cmd_line_get_param(&results, PRTE_CLI_RANKBY);
    if (NULL != opt) {
        PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_RANKBY, opt->values[0], PMIX_STRING);
    }

    /* if the user specified a binding policy, then set it */
    opt = pmix_cmd_line_get_param(&results, PRTE_CLI_BINDTO);
    if (NULL != opt) {
        PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_BINDTO, opt->values[0], PMIX_STRING);
    }

    /* check for an exec agent */
    opt = pmix_cmd_line_get_param(&results, PRTE_CLI_EXEC_AGENT);
    if (NULL != opt) {
        PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_EXEC_AGENT, opt->values[0], PMIX_STRING);
    }

    /* mark if recovery was enabled on the cmd line */
    if (pmix_cmd_line_is_taken(&results, PRTE_CLI_ENABLE_RECOVERY)) {
        PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_JOB_RECOVERABLE, &flag, PMIX_BOOL);
    }
    /* record the max restarts */
    opt = pmix_cmd_line_get_param(&results, PRTE_CLI_MAX_RESTARTS);
    if (NULL != opt) {
        ui32 = strtol(opt->values[0], NULL, 10);
        PMIX_LIST_FOREACH(app, &apps, prte_pmix_app_t)
        {
            PMIX_INFO_LIST_ADD(ret, app->info, PMIX_MAX_RESTARTS, &ui32, PMIX_UINT32);
        }
    }
    /* if continuous operation was specified */
    if (pmix_cmd_line_is_taken(&results, "continuous")) {
        /* mark this job as continuously operating */
        PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_JOB_CONTINUOUS, &flag, PMIX_BOOL);
    }

    /* if stop-on-exec was specified */
    if (pmix_cmd_line_is_taken(&results, "stop-on-exec")) {
        PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_DEBUG_STOP_ON_EXEC, &flag, PMIX_BOOL);
    }

    /* check for a job timeout specification, to be provided in seconds
     * as that is what MPICH used
     */
    i = 0;
    opt = pmix_cmd_line_get_param(&results, "timeout");
    if (NULL != opt) {
        i = strtol(opt->values[0], NULL, 10);
    } else if (NULL != (param = getenv("MPIEXEC_TIMEOUT"))) {
        i = strtol(param, NULL, 10);
    }
    if (0 != i) {
#ifdef PMIX_JOB_TIMEOUT
        PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_JOB_TIMEOUT, &i, PMIX_INT);
#else
        PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_TIMEOUT, &i, PMIX_INT);
#endif
    }

    if (pmix_cmd_line_is_taken(&results, "get-stack-traces")) {
        PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_TIMEOUT_STACKTRACES, &flag, PMIX_BOOL);
    }
    if (pmix_cmd_line_is_taken(&results, "report-state-on-timeout")) {
        PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_TIMEOUT_REPORT_STATE, &flag, PMIX_BOOL);
    }
#ifdef PMIX_SPAWN_TIMEOUT
    opt = pmix_cmd_line_get_param(&results, "spawn-timeout");
    if (NULL != opt) {
        i = strtol(opt->values[0], NULL, 10);
        PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_SPAWN_TIMEOUT, &i, PMIX_INT);
    }
#endif

    /* give the schizo components a chance to add to the job info */
    schizo->job_info(&results, jinfo);

    /* pickup any relevant envars */
    ninfo = 4;
    PMIX_INFO_CREATE(iptr, ninfo);
    flag = true;
    PMIX_INFO_LOAD(&iptr[0], PMIX_SETUP_APP_ENVARS, &flag, PMIX_BOOL);
    ui32 = geteuid();
    PMIX_INFO_LOAD(&iptr[1], PMIX_USERID, &ui32, PMIX_UINT32);
    ui32 = getegid();
    PMIX_INFO_LOAD(&iptr[2], PMIX_GRPID, &ui32, PMIX_UINT32);
    PMIX_INFO_LOAD(&iptr[3], PMIX_PERSONALITY, personality, PMIX_STRING);

    PRTE_PMIX_CONSTRUCT_LOCK(&mylock.lock);
    ret = PMIx_server_setup_application(prte_process_info.myproc.nspace, iptr, ninfo, setupcbfunc,
                                        &mylock);
    if (PMIX_SUCCESS != ret) {
        PMIX_ERROR_LOG(ret);
        PRTE_PMIX_DESTRUCT_LOCK(&mylock.lock);
        PRTE_UPDATE_EXIT_STATUS(ret);
        goto DONE;
    }
    PRTE_PMIX_WAIT_THREAD(&mylock.lock);
    PMIX_INFO_FREE(iptr, ninfo);
    PRTE_PMIX_DESTRUCT_LOCK(&mylock.lock);
    /* transfer any returned ENVARS to the job_info */
    if (NULL != mylock.info) {
        for (n = 0; n < mylock.ninfo; n++) {
            if (0 == strncmp(mylock.info[n].key, PMIX_SET_ENVAR, PMIX_MAX_KEYLEN)
                || 0 == strncmp(mylock.info[n].key, PMIX_ADD_ENVAR, PMIX_MAX_KEYLEN)
                || 0 == strncmp(mylock.info[n].key, PMIX_UNSET_ENVAR, PMIX_MAX_KEYLEN)
                || 0 == strncmp(mylock.info[n].key, PMIX_PREPEND_ENVAR, PMIX_MAX_KEYLEN)
                || 0 == strncmp(mylock.info[n].key, PMIX_APPEND_ENVAR, PMIX_MAX_KEYLEN)) {
                PMIX_INFO_LIST_XFER(ret, jinfo, &mylock.info[n]);
            }
        }
        PMIX_INFO_FREE(mylock.info, mylock.ninfo);
    }
    /* mark that we harvested envars so prte knows not to do it again */
    PMIX_INFO_LIST_ADD(ret, jinfo, PMIX_ENVARS_HARVESTED, NULL, PMIX_BOOL);


    /* they want to run an application, so let's parse
     * the cmd line to get it */

    if (PRTE_SUCCESS != (rc = prte_parse_locals(schizo, &apps, pargv, NULL, NULL))) {
        PRTE_ERROR_LOG(rc);
        PMIX_LIST_DESTRUCT(&apps);
        goto DONE;
    }

    /* bozo check */
    if (0 == pmix_list_get_size(&apps)) {
        prte_output(0, "No application specified!");
        goto DONE;
    }

    /* convert the job info into an array */
    PMIX_INFO_LIST_CONVERT(ret, jinfo, &darray);
    iptr = (pmix_info_t *) darray.array;
    ninfo = darray.size;
    PMIX_INFO_LIST_RELEASE(jinfo);

    /* convert the apps to an array */
    napps = pmix_list_get_size(&apps);
    PMIX_APP_CREATE(papps, napps);
    n = 0;
    PMIX_LIST_FOREACH(app, &apps, prte_pmix_app_t)
    {
        papps[n].cmd = strdup(app->app.cmd);
        papps[n].argv = pmix_argv_copy(app->app.argv);
        papps[n].env = pmix_argv_copy(app->app.env);
        papps[n].cwd = strdup(app->app.cwd);
        papps[n].maxprocs = app->app.maxprocs;
        PMIX_INFO_LIST_CONVERT(ret, app->info, &darray);
        papps[n].info = (pmix_info_t *) darray.array;
        papps[n].ninfo = darray.size;
        ++n;
    }
    PMIX_LIST_DESTRUCT(&apps);

    if (verbose) {
        prte_output(0, "Calling PMIx_Spawn");
    }

    ret = PMIx_Spawn(iptr, ninfo, papps, napps, spawnednspace);
    if (PRTE_SUCCESS != ret) {
        prte_output(0, "PMIx_Spawn failed (%d): %s", ret, PMIx_Error_string(ret));
        rc = ret;
        goto DONE;
    }

    /* register to receive the ready-for-debug event - the internal
     * event library can relay it to any tool connected to us */
    PRTE_PMIX_CONSTRUCT_LOCK(&lock);
    code = PMIX_READY_FOR_DEBUG;
    n = 0;
    PMIX_INFO_CREATE(iptr, 2);
    PMIX_INFO_LOAD(&iptr[n], PMIX_EVENT_HDLR_NAME, "READY-FOR-DEBUG", PMIX_STRING);
    ++n;
    PMIX_LOAD_PROCID(&pname, spawnednspace, PMIX_RANK_WILDCARD);
    PMIX_INFO_LOAD(&iptr[n], PMIX_EVENT_AFFECTED_PROC, &pname, PMIX_PROC);
    PMIx_Register_event_handler(&code, 1, iptr, 2, debug_cbfunc, regcbfunc,
                                (void *) &lock);
    PRTE_PMIX_WAIT_THREAD(&lock);
    PRTE_PMIX_DESTRUCT_LOCK(&lock);
    PMIX_INFO_FREE(iptr, 2);

    /* check what user wants us to do with stdin */
    PMIX_LOAD_NSPACE(pname.nspace, spawnednspace);
    opt = pmix_cmd_line_get_param(&results, PRTE_CLI_STDIN);
    if (NULL != opt) {
        if (0 == strcmp(opt->values[0], "all")) {
            pname.rank = PMIX_RANK_WILDCARD;
        } else if (0 == strcmp(opt->values[0], "none")) {
            pname.rank = PMIX_RANK_INVALID;
        } else {
            pname.rank = 0;
        }
    } else {
        pname.rank = 0;
    }
    if (PMIX_RANK_INVALID != pname.rank) {
        PMIX_INFO_CREATE(iptr, 1);
        PMIX_INFO_LOAD(&iptr[0], PMIX_IOF_PUSH_STDIN, NULL, PMIX_BOOL);
        PRTE_PMIX_CONSTRUCT_LOCK(&lock);
        ret = PMIx_IOF_push(&pname, 1, NULL, iptr, 1, opcbfunc, &lock);
        if (PMIX_SUCCESS != ret && PMIX_OPERATION_SUCCEEDED != ret) {
            prte_output(0, "IOF push of stdin failed: %s", PMIx_Error_string(ret));
        } else if (PMIX_SUCCESS == ret) {
            PRTE_PMIX_WAIT_THREAD(&lock);
        }
        PRTE_PMIX_DESTRUCT_LOCK(&lock);
        PMIX_INFO_FREE(iptr, 1);
    }

    /* register to be notified when
     * our job completes */
    ret = PMIX_EVENT_JOB_END;
    /* setup the info */
    ninfo = 3;
    PMIX_INFO_CREATE(iptr, ninfo);
    /* give the handler a name */
    PMIX_INFO_LOAD(&iptr[0], PMIX_EVENT_HDLR_NAME, "JOB_TERMINATION_EVENT", PMIX_STRING);
    /* specify we only want to be notified when our
     * job terminates */
    PMIX_LOAD_PROCID(&pname, spawnednspace, PMIX_RANK_WILDCARD);
    PMIX_INFO_LOAD(&iptr[1], PMIX_EVENT_AFFECTED_PROC, &pname, PMIX_PROC);
    /* request that they return our lock object */
    PMIX_INFO_LOAD(&iptr[2], PMIX_EVENT_RETURN_OBJECT, &rellock, PMIX_POINTER);
    /* do the registration */
    PRTE_PMIX_CONSTRUCT_LOCK(&lock);
    PMIx_Register_event_handler(&ret, 1, iptr, ninfo, evhandler, regcbfunc, &lock);
    PRTE_PMIX_WAIT_THREAD(&lock);
    PRTE_PMIX_DESTRUCT_LOCK(&lock);

    if (verbose) {
        prte_output(0, "JOB %s EXECUTING", PRTE_JOBID_PRINT(spawnednspace));
    }
    PRTE_PMIX_WAIT_THREAD(&rellock);
    /* save the status */
    rc = rellock.status;
    /* output any message */
    if (NULL != rellock.msg) {
        fprintf(stderr, "%s\n", rellock.msg);
    }

    /* if we lost connection to the server, then we are done */
    if (PMIX_ERR_LOST_CONNECTION == rc || PMIX_ERR_UNREACH == rc) {
        goto DONE;
    }

    /* deregister our event handler */
    PRTE_PMIX_CONSTRUCT_LOCK(&lock);
    PMIx_Deregister_event_handler(evid, opcbfunc, &lock);
    PRTE_PMIX_WAIT_THREAD(&lock);
    PRTE_PMIX_DESTRUCT_LOCK(&lock);
    PRTE_PMIX_DESTRUCT_LOCK(&rellock);

    /* close the push of our stdin */
    PMIX_INFO_LOAD(&info, PMIX_IOF_COMPLETE, NULL, PMIX_BOOL);
    PRTE_PMIX_CONSTRUCT_LOCK(&lock);
    ret = PMIx_IOF_push(NULL, 0, NULL, &info, 1, opcbfunc, &lock);
    if (PMIX_SUCCESS != ret && PMIX_OPERATION_SUCCEEDED != ret) {
        prte_output(0, "IOF close of stdin failed: %s", PMIx_Error_string(ret));
    } else if (PMIX_SUCCESS == ret) {
        PRTE_PMIX_WAIT_THREAD(&lock);
    }
    PRTE_PMIX_DESTRUCT_LOCK(&lock);
    PMIX_INFO_DESTRUCT(&info);

DONE:
    PMIX_LIST_FOREACH(evitm, &forwarded_signals, prte_event_list_item_t)
    {
        prte_event_signal_del(&evitm->ev);
    }
    PMIX_LIST_DESTRUCT(&forwarded_signals);
    if (NULL != papps) {
        PMIX_APP_FREE(papps, napps);
    }
    /* cleanup and leave */
    ret = PMIx_tool_finalize();
    if (PMIX_SUCCESS != ret) {
        // Since the user job has probably exited by
        // now, let's preserve its return code and print
        // a warning here, if prte logging is on.
        prte_output(0, "PMIx_tool_finalize() failed. Status = %d", ret);
    }
    return rc;
}

static void clean_abort(int fd, short flags, void *arg)
{
    pmix_proc_t target;
    pmix_info_t directive;
    pmix_status_t rc;

    /* if we have already ordered this once, don't keep
     * doing it to avoid race conditions
     */
    if (pmix_mutex_trylock(&prun_abort_inprogress_lock)) { /* returns 1 if already locked */
        if (forcibly_die) {
            PMIx_tool_finalize();
            /* exit with a non-zero status */
            exit(1);
        }
        fprintf(stderr,
                "prun: abort is already in progress...hit ctrl-c again to forcibly terminate\n\n");
        forcibly_die = true;
        /* reset the event */
        prte_event_add(&term_handler, NULL);
        return;
    }

    /* tell PRTE to terminate our job */
    PMIX_LOAD_PROCID(&target, spawnednspace, PMIX_RANK_WILDCARD);
    PMIX_INFO_LOAD(&directive, PMIX_JOB_CTRL_KILL, NULL, PMIX_BOOL);
    rc = PMIx_Job_control_nb(&target, 1, &directive, 1, NULL, NULL);
    if (PMIX_SUCCESS != rc && PMIX_OPERATION_SUCCEEDED != rc) {
        PMIx_tool_finalize();
        /* exit with a non-zero status */
        exit(1);
    }
}

static struct timeval current, last = {0, 0};
static bool first = true;

/*
 * Attempt to terminate the job and wait for callback indicating
 * the job has been aborted.
 */
static void abort_signal_callback(int fd)
{
    uint8_t foo = 1;
    char *msg
        = "Abort is in progress...hit ctrl-c again within 5 seconds to forcibly terminate\n\n";

    /* if this is the first time thru, just get
     * the current time
     */
    if (first) {
        first = false;
        gettimeofday(&current, NULL);
    } else {
        /* get the current time */
        gettimeofday(&current, NULL);
        /* if this is within 5 seconds of the
         * last time we were called, then just
         * exit - we are probably stuck
         */
        if ((current.tv_sec - last.tv_sec) < 5) {
            exit(1);
        }
        if (-1 == write(1, (void *) msg, strlen(msg))) {
            exit(1);
        }
    }
    /* save the time */
    last.tv_sec = current.tv_sec;
    /* tell the event lib to attempt to abnormally terminate */
    if (-1 == write(term_pipe[1], &foo, 1)) {
        exit(1);
    }
}

static void signal_forward_callback(int signum)
{
    pmix_status_t rc;
    pmix_proc_t proc;
    pmix_info_t info;

    if (verbose) {
        fprintf(stderr, "%s: Forwarding signal %d to job\n", prte_tool_basename, signum);
    }

    /* send the signal out to the processes */
    PMIX_LOAD_PROCID(&proc, spawnednspace, PMIX_RANK_WILDCARD);
    PMIX_INFO_LOAD(&info, PMIX_JOB_CTRL_SIGNAL, &signum, PMIX_INT);
    rc = PMIx_Job_control(&proc, 1, &info, 1, NULL, NULL);
    if (PMIX_SUCCESS != rc && PMIX_OPERATION_SUCCEEDED != rc) {
        fprintf(stderr, "Signal %d could not be sent to job %s (returned %s)", signum,
                spawnednspace, PMIx_Error_string(rc));
    }
}

/**
 * Deal with sigpipe errors
 */
static int sigpipe_error_count = 0;
static void epipe_signal_callback(int signal)
{
    sigpipe_error_count++;

    if (10 < sigpipe_error_count) {
        /* time to abort */
        prte_output(0, "%s: SIGPIPE detected - aborting", prte_tool_basename);
        clean_abort(0, 0, NULL);
    }

    return;
}
