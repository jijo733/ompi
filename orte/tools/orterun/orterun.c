/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2008 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2006-2015 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2007-2009 Sun Microsystems, Inc. All rights reserved.
 * Copyright (c) 2007-2016 Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2013-2016 Intel, Inc. All rights reserved.
 * Copyright (c) 2015      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "orte_config.h"
#include "orte/constants.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef HAVE_STRINGS_H
#include <strings.h>
#endif  /* HAVE_STRINGS_H */
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_SYS_PARAM_H
#include <sys/param.h>
#endif
#include <errno.h>
#include <signal.h>
#include <ctype.h>
#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif  /* HAVE_SYS_TYPES_H */
#ifdef HAVE_SYS_WAIT_H
#include <sys/wait.h>
#endif  /* HAVE_SYS_WAIT_H */
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif  /* HAVE_SYS_TIME_H */
#include <fcntl.h>
#ifdef HAVE_SYS_STAT_H
#include <sys/stat.h>
#endif

#include "opal/mca/event/event.h"
#include "opal/mca/installdirs/installdirs.h"
#include "opal/mca/hwloc/base/base.h"
#include "opal/mca/base/base.h"
#include "opal/util/argv.h"
#include "opal/util/output.h"
#include "opal/util/basename.h"
#include "opal/util/cmd_line.h"
#include "opal/util/opal_environ.h"
#include "opal/util/opal_getcwd.h"
#include "opal/util/show_help.h"
#include "opal/util/fd.h"
#include "opal/sys/atomic.h"
#if OPAL_ENABLE_FT_CR == 1
#include "opal/runtime/opal_cr.h"
#endif

#include "opal/version.h"
#include "opal/runtime/opal.h"
#include "opal/runtime/opal_info_support.h"
#include "opal/util/os_path.h"
#include "opal/util/path.h"
#include "opal/class/opal_pointer_array.h"
#include "opal/dss/dss.h"

#include "orte/mca/dfs/dfs.h"
#include "orte/mca/odls/odls.h"
#include "orte/mca/rml/rml.h"
#include "orte/mca/state/state.h"
#include "orte/util/proc_info.h"
#include "orte/util/show_help.h"

#include "orte/runtime/runtime.h"
#include "orte/runtime/orte_globals.h"
#include "orte/runtime/orte_wait.h"
#include "orte/runtime/orte_locks.h"
#include "orte/runtime/orte_quit.h"

/* ensure I can behave like a daemon */
#include "orte/orted/orted.h"
#include "orte/orted/orted_submit.h"
#include "orterun.h"

/* local type */
 typedef struct {
     int status;
     volatile bool active;
     orte_job_t *jdata;
 } orte_submit_status_t;


/* local data */
static opal_list_t job_stack;

static void spawn_next_job(opal_buffer_t *bptr, void *cbdata)
{
    orte_job_t *jdata = (orte_job_t*)cbdata;

    /* add the data to the job's file map */
    orte_set_attribute(&jdata->attributes, ORTE_JOB_FILE_MAPS, ORTE_ATTR_GLOBAL, &bptr, OPAL_BUFFER);

    /* spawn the next job */
    orte_plm.spawn(jdata);
}
static void run_next_job(int fd, short args, void *cbdata)
{
    orte_state_caddy_t *caddy = (orte_state_caddy_t*)cbdata;
    orte_job_t *jdata;
    orte_process_name_t name;

    /* get next job on stack */
    jdata = (orte_job_t*)opal_list_remove_first(&job_stack);

    if (NULL == jdata) {
        /* all done - trip the termination sequence */
        orte_event_base_active = false;
        OBJ_DESTRUCT(&job_stack);
        OBJ_RELEASE(caddy);
        return;
    }

    if (NULL != orte_dfs.get_file_map) {
        /* collect any file maps and spawn the next job */
        name.jobid = caddy->jdata->jobid;
        name.vpid = ORTE_VPID_WILDCARD;

        orte_dfs.get_file_map(&name, spawn_next_job, jdata);
    } else {
        /* just spawn the job */
        orte_plm.spawn(jdata);
    }

    OBJ_RELEASE(caddy);
}

static void launched(int index, orte_job_t *jdata, int ret, void *cbdata)
{
    orte_submit_status_t *launchst = (orte_submit_status_t*)cbdata;
    launchst->status = ret;
    ORTE_UPDATE_EXIT_STATUS(ret);
    OBJ_RETAIN(jdata);
    launchst->jdata = jdata;
    launchst->active = false;
}
static void completed(int index, orte_job_t *jdata, int ret, void *cbdata)
{
    orte_submit_status_t *completest = (orte_submit_status_t*)cbdata;
    completest->status = ret;
    ORTE_UPDATE_EXIT_STATUS(ret);
    OBJ_RETAIN(jdata);
    completest->jdata = jdata;
    completest->active = false;
}

int orterun(int argc, char *argv[])
{
    orte_submit_status_t launchst, completest;

    if (ORTE_SUCCESS != orte_submit_init(argc, argv, NULL)) {
        exit(1);
    }

    /* check if we are running as root - if we are, then only allow
     * us to proceed if the allow-run-as-root flag was given. Otherwise,
     * exit with a giant warning flag
     */
    if (0 == geteuid() && !orte_cmd_options.run_as_root) {
        fprintf(stderr, "--------------------------------------------------------------------------\n");
        if (orte_cmd_options.help) {
            fprintf(stderr, "%s cannot provide the help message when run as root.\n", orte_basename);
        } else {
            /* show_help is not yet available, so print an error manually */
            fprintf(stderr, "%s has detected an attempt to run as root.\n", orte_basename);
        }
        fprintf(stderr, "Running at root is *strongly* discouraged as any mistake (e.g., in\n");
        fprintf(stderr, "defining TMPDIR) or bug can result in catastrophic damage to the OS\n");
        fprintf(stderr, "file system, leaving your system in an unusable state.\n\n");
        fprintf(stderr, "You can override this protection by adding the --allow-run-as-root\n");
        fprintf(stderr, "option to your cmd line. However, we reiterate our strong advice\n");
        fprintf(stderr, "against doing so - please do so at your own risk.\n");
        fprintf(stderr, "--------------------------------------------------------------------------\n");
        exit(1);
    }

    /* setup to listen for commands sent specifically to me, even though I would probably
     * be the one sending them! Unfortunately, since I am a participating daemon,
     * there are times I need to send a command to "all daemons", and that means *I* have
     * to receive it too
     */
    orte_rml.recv_buffer_nb(ORTE_NAME_WILDCARD, ORTE_RML_TAG_DAEMON,
                            ORTE_RML_PERSISTENT, orte_daemon_recv, NULL);

    /* if the user just wants us to terminate a DVM, then do so */
    if (orte_cmd_options.terminate_dvm) {
        if (ORTE_ERR_OP_IN_PROGRESS != orte_submit_halt()) {
            ORTE_UPDATE_EXIT_STATUS(1);
            goto DONE;
        }
        while (1) {
           opal_event_loop(orte_event_base, OPAL_EVLOOP_ONCE);
        }
    } else {
        /* spawn the job and its daemons */
        memset(&launchst, 0, sizeof(launchst));
        memset(&completest, 0, sizeof(completest));
        launchst.active = true;
        completest.active = true;
        if (ORTE_SUCCESS != orte_submit_job(argv, NULL,
                                            launched, &launchst,
                                            completed, &completest)) {
            ORTE_UPDATE_EXIT_STATUS(1);
            goto DONE;
        }
    }

      // wait for response and unpack the status, jobid
      while (orte_event_base_active && launchst.active) {
          opal_event_loop(orte_event_base, OPAL_EVLOOP_ONCE);
      }
      if (orte_debug_flag) {
          opal_output(0, "Job %s has launched",
                      (NULL == launchst.jdata) ? "UNKNOWN" : ORTE_JOBID_PRINT(launchst.jdata->jobid));
      }
      if (!orte_event_base_active || ORTE_SUCCESS != launchst.status) {
          goto DONE;
      }

      while (orte_event_base_active && completest.active) {
          opal_event_loop(orte_event_base, OPAL_EVLOOP_ONCE);
      }

#if 0
    if (orte_staged_execution) {
        /* staged execution is requested - each app_context
         * is treated as a separate job and executed in
         * sequence
         */
        int i;
        jdata->num_procs = 0;
        OBJ_CONSTRUCT(&job_stack, opal_list_t);
        for (i=1; i < jdata->apps->size; i++) {
            if (NULL == (app = (orte_app_context_t*)opal_pointer_array_get_item(jdata->apps, i))) {
                continue;
            }
            jptr = OBJ_NEW(orte_job_t);
            opal_list_append(&job_stack, &jptr->super);
            /* transfer the app */
            opal_pointer_array_set_item(jdata->apps, i, NULL);
            --jdata->num_apps;
            /* reset the app_idx */
            app->idx = 0;
            opal_pointer_array_set_item(jptr->apps, 0, app);
            ++jptr->num_apps;
        }
        /* define a state machine position
         * that is fired when each job completes so we can then start
         * the next job in our stack
         */
        if (ORTE_SUCCESS != (rc = orte_state.set_job_state_callback(ORTE_JOB_STATE_NOTIFY_COMPLETED, run_next_job))) {
            ORTE_ERROR_LOG(rc);
            ORTE_UPDATE_EXIT_STATUS(rc);
            goto DONE;
        }
    }
#endif

    if (ORTE_PROC_IS_HNP) {
        /* ensure all local procs are dead */
        orte_odls.kill_local_procs(NULL);
    }

 DONE:
    /* cleanup and leave */
    orte_submit_finalize();
    orte_finalize();

    if (orte_debug_flag) {
        fprintf(stderr, "exiting with status %d\n", orte_exit_status);
    }
    exit(orte_exit_status);
}
