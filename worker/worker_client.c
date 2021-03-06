/******************************************************************************
 *
 * mod_gearman - distribute checks with gearman
 *
 * Copyright (c) 2010 Sven Nierlein - sven.nierlein@consol.de
 *
 * This file is part of mod_gearman.
 *
 *  mod_gearman is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  mod_gearman is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with mod_gearman.  If not, see <http://www.gnu.org/licenses/>.
 *
 *****************************************************************************/

/* include header */
#include "worker.h"
#include "common.h"
#include "worker_client.h"
#include "utils.h"
#include "gearman.h"

char temp_buffer1[GM_BUFFERSIZE];
char temp_buffer2[GM_BUFFERSIZE];
char hostname[GM_BUFFERSIZE];

gearman_worker_st worker;
gearman_client_st client;
gearman_client_st client_dup;

gm_job_t * current_job;
pid_t current_pid;
gm_job_t * exec_job;

int jobs_done = 0;
int sleep_time_after_error = 1;
int worker_run_mode;
int shm_index = 0;
volatile sig_atomic_t shmid;

/* callback for task completed */
void worker_client(int worker_mode, int indx, int shid) {

    gm_log( GM_LOG_TRACE, "%s worker client started\n", (worker_mode == GM_WORKER_STATUS ? "status" : "job" ));

    /* set signal handlers for a clean exit */
    signal(SIGINT, clean_worker_exit);
    signal(SIGTERM,clean_worker_exit);

    worker_run_mode = worker_mode;
    shm_index       = indx;
    shmid           = shid;
    current_pid     = getpid();

    gethostname(hostname, GM_BUFFERSIZE-1);

    /* create worker */
    if(set_worker(&worker) != GM_OK) {
        gm_log( GM_LOG_ERROR, "cannot start worker\n" );
        clean_worker_exit(0);
        exit( EXIT_FAILURE );
    }

    /* create client */
    if ( create_client( mod_gm_opt->server_list, &client ) != GM_OK ) {
        gm_log( GM_LOG_ERROR, "cannot start client\n" );
        clean_worker_exit(0);
        exit( EXIT_FAILURE );
    }

    /* create duplicate client */
    if ( create_client_dup( mod_gm_opt->dupserver_list, &client_dup ) != GM_OK ) {
        gm_log( GM_LOG_ERROR, "cannot start client for duplicate server\n" );
        exit( EXIT_FAILURE );
    }

    worker_loop();

    return;
}


/* main loop of jobs */
void worker_loop() {

    while ( 1 ) {
        gearman_return_t ret;

        /* wait for a job, otherwise exit when hit the idle timeout */
        if(mod_gm_opt->idle_timeout > 0 && worker_run_mode == GM_WORKER_MULTI) {
            signal(SIGALRM, idle_sighandler);
            alarm(mod_gm_opt->idle_timeout);
        }

        signal(SIGPIPE, SIG_IGN);
        ret = gearman_worker_work( &worker );
        if ( ret != GEARMAN_SUCCESS ) {
            gm_log( GM_LOG_ERROR, "worker error: %s\n", gearman_worker_error( &worker ) );
            gearman_job_free_all( &worker );
            gearman_worker_free( &worker );
            gearman_client_free( &client );
            if( mod_gm_opt->dupserver_num )
                gearman_client_free( &client_dup );

            /* sleep on error to avoid cpu intensive infinite loops */
            sleep(sleep_time_after_error);
            sleep_time_after_error += 3;
            if(sleep_time_after_error > 60)
                sleep_time_after_error = 60;

            /* create new connections */
            set_worker( &worker );
            create_client( mod_gm_opt->server_list, &client );
            if( mod_gm_opt->dupserver_num )
                create_client_dup( mod_gm_opt->dupserver_list, &client_dup );
        }
    }

    return;
}


/* get a job */
void *get_job( gearman_job_st *job, void *context, size_t *result_size, gearman_return_t *ret_ptr ) {
    sigset_t block_mask;
    sigset_t old_mask;
    int wsize, valid_lines;
    char workload[GM_BUFFERSIZE];
    char * decrypted_data;
    char * decrypted_data_c;
    char * decrypted_orig;
    char *ptr;

    /* reset timeout for now, will be set befor execution again */
    alarm(0);
    signal(SIGALRM, SIG_IGN);

    jobs_done++;

    /* send start signal to parent */
    set_state(GM_JOB_START);

    gm_log( GM_LOG_TRACE, "get_job()\n" );

    /* contect is unused */
    context = context;

    /* set size of result */
    *result_size = 0;

    /* reset sleep time */
    sleep_time_after_error = 1;

    /* ignore sigterms while running job */
    sigemptyset(&block_mask);
    sigaddset(&block_mask, SIGTERM);
    sigprocmask(SIG_BLOCK, &block_mask, &old_mask);

    /* get the data */
    wsize = gearman_job_workload_size(job);
    strncpy(workload, (const char*)gearman_job_workload(job), wsize);
    workload[wsize] = '\0';
    gm_log( GM_LOG_TRACE, "got new job %s\n", gearman_job_handle( job ) );
    gm_log( GM_LOG_TRACE, "%d +++>\n%s\n<+++\n", strlen(workload), workload );

    /* decrypt data */
    decrypted_data = malloc(GM_BUFFERSIZE);
    decrypted_data_c = decrypted_data;
    mod_gm_decrypt(&decrypted_data, workload, mod_gm_opt->transportmode);
    decrypted_orig = strdup(decrypted_data);

    if(decrypted_data == NULL) {
        *ret_ptr = GEARMAN_WORK_FAIL;
        return NULL;
    }
    gm_log( GM_LOG_TRACE, "%d --->\n%s\n<---\n", strlen(decrypted_data), decrypted_data );

    /* set result pointer to success */
    *ret_ptr= GEARMAN_SUCCESS;

    exec_job = ( gm_job_t * )malloc( sizeof *exec_job );
    set_default_job(exec_job, mod_gm_opt);

    valid_lines = 0;
    while ( (ptr = strsep(&decrypted_data, "\n" )) != NULL ) {
        char *key   = strsep( &ptr, "=" );
        char *value = strsep( &ptr, "\x0" );

        if ( key == NULL )
            continue;

        if ( value == NULL || !strcmp( value, "") )
            break;

        if ( !strcmp( key, "host_name" ) ) {
            exec_job->host_name = strdup(value);
            valid_lines++;
        } else if ( !strcmp( key, "service_description" ) ) {
            exec_job->service_description = strdup(value);
            valid_lines++;
        } else if ( !strcmp( key, "type" ) ) {
            exec_job->type = strdup(value);
            valid_lines++;
        } else if ( !strcmp( key, "result_queue" ) ) {
            exec_job->result_queue = strdup(value);
            valid_lines++;
        } else if ( !strcmp( key, "check_options" ) ) {
            exec_job->check_options = atoi(value);
            valid_lines++;
        } else if ( !strcmp( key, "scheduled_check" ) ) {
            exec_job->scheduled_check = atoi(value);
            valid_lines++;
        } else if ( !strcmp( key, "reschedule_check" ) ) {
            exec_job->reschedule_check = atoi(value);
            valid_lines++;
        } else if ( !strcmp( key, "latency" ) ) {
            exec_job->latency = atof(value);
            valid_lines++;
        } else if ( !strcmp( key, "start_time" ) ) {
            string2timeval(value, &exec_job->core_start_time);
            valid_lines++;
        } else if ( !strcmp( key, "timeout" ) ) {
            exec_job->timeout = atoi(value);
            valid_lines++;
        } else if ( !strcmp( key, "command_line" ) ) {
            exec_job->command_line = strdup(value);
            valid_lines++;
        }
    }

#ifdef GM_DEBUG
    if(exec_job->core_start_time.tv_sec < 10000)
        write_debug_file(&decrypted_orig);
#endif

    if(valid_lines == 0) {
        gm_log( GM_LOG_ERROR, "discarded invalid job, check your encryption settings\n" );
    } else {
        do_exec_job();
    }

    /* send finish signal to parent */
    set_state(GM_JOB_END);

    /* start listening to SIGTERMs */
    sigprocmask(SIG_SETMASK, &old_mask, NULL);

    free(decrypted_orig);
    free(decrypted_data_c);
    free_job(exec_job);

    if(mod_gm_opt->max_jobs > 0 && jobs_done >= mod_gm_opt->max_jobs) {
        gm_log( GM_LOG_TRACE, "jobs done: %i -> exiting...\n", jobs_done );
        clean_worker_exit(0);
        exit( EXIT_SUCCESS );
    }

    return NULL;
}


/* do some job */
void do_exec_job( ) {
    struct timeval start_time, end_time;
    int latency;

    gm_log( GM_LOG_TRACE, "do_exec_job()\n" );

    if(exec_job->type == NULL) {
        gm_log( GM_LOG_ERROR, "discarded invalid job, no type given\n" );
        return;
    }
    if(exec_job->command_line == NULL) {
        gm_log( GM_LOG_ERROR, "discarded invalid job, no command line given\n" );
        return;
    }

    if ( !strcmp( exec_job->type, "service" ) ) {
        gm_log( GM_LOG_DEBUG, "got service job: %s - %s\n", exec_job->host_name, exec_job->service_description);
    }
    else if ( !strcmp( exec_job->type, "host" ) ) {
        gm_log( GM_LOG_DEBUG, "got host job: %s\n", exec_job->host_name);
    }
    else if ( !strcmp( exec_job->type, "event" ) ) {
        gm_log( GM_LOG_DEBUG, "got eventhandler job\n");
    }

    /* check proper timeout value */
    if( exec_job->timeout <= 0 ) {
        exec_job->timeout = mod_gm_opt->job_timeout;
    }

    gm_log( GM_LOG_TRACE, "timeout %i\n", exec_job->timeout);

    /* get the check start time */
    gettimeofday(&start_time,NULL);
    exec_job->start_time = start_time;
    latency = exec_job->core_start_time.tv_sec - start_time.tv_sec;

    /* job is too old */
    if(latency > mod_gm_opt->max_age) {
        exec_job->return_code   = 3;

        gm_log( GM_LOG_INFO, "discarded too old %s job: %i > %i\n", exec_job->type, (int)latency, mod_gm_opt->max_age);

        gettimeofday(&end_time, NULL);
        exec_job->finish_time = end_time;

        if ( !strcmp( exec_job->type, "service" ) || !strcmp( exec_job->type, "host" ) ) {
            exec_job->output = strdup("(Could Not Start Check In Time)");
            send_result_back();
        }

        return;
    }

    exec_job->early_timeout = 0;

    /* run the command */
    gm_log( GM_LOG_TRACE, "command: %s\n", exec_job->command_line);
    execute_safe_command(exec_job, mod_gm_opt->fork_on_exec, hostname);

    if ( !strcmp( exec_job->type, "service" ) || !strcmp( exec_job->type, "host" ) ) {
        send_result_back();
    }

    return;
}


/* send results back */
void send_result_back() {
    gm_log( GM_LOG_TRACE, "send_result_back()\n" );

    if(exec_job->result_queue == NULL) {
        return;
    }
    if(exec_job->output == NULL) {
        return;
    }

    gm_log( GM_LOG_TRACE, "queue: %s\n", exec_job->result_queue );
    temp_buffer1[0]='\x0';
    snprintf( temp_buffer1, sizeof( temp_buffer1 )-1, "host_name=%s\ncore_start_time=%i.%i\nstart_time=%i.%i\nfinish_time=%i.%i\nlatency=%f\nreturn_code=%i\nexited_ok=%i\n",
              exec_job->host_name,
              ( int )exec_job->core_start_time.tv_sec,
              ( int )exec_job->core_start_time.tv_usec,
              ( int )exec_job->start_time.tv_sec,
              ( int )exec_job->start_time.tv_usec,
              ( int )exec_job->finish_time.tv_sec,
              ( int )exec_job->finish_time.tv_usec,
              exec_job->latency,
              exec_job->return_code,
              exec_job->exited_ok
            );
    temp_buffer1[sizeof( temp_buffer1 )-1]='\x0';

    if(exec_job->service_description != NULL) {
        temp_buffer2[0]='\x0';
        strncat(temp_buffer2, "service_description=", (sizeof(temp_buffer2)-1));
        strncat(temp_buffer2, exec_job->service_description, (sizeof(temp_buffer2)-1));
        strncat(temp_buffer2, "\n", (sizeof(temp_buffer2)-1));

        strncat(temp_buffer1, temp_buffer2, (sizeof(temp_buffer1)-1));
    }
    temp_buffer1[sizeof( temp_buffer1 )-1]='\x0';

    if(exec_job->output != NULL) {
        temp_buffer2[0]='\x0';
        strncat(temp_buffer2, "output=", (sizeof(temp_buffer2)-1));
        if(mod_gm_opt->debug_result) {
            strncat(temp_buffer2, "(", (sizeof(temp_buffer2)-1));
            strncat(temp_buffer2, hostname, (sizeof(temp_buffer2)-1));
            strncat(temp_buffer2, ") - ", (sizeof(temp_buffer2)-1));
        }
        strncat(temp_buffer2, exec_job->output, (sizeof(temp_buffer2)-1));
        strncat(temp_buffer2, "\n\n\n", (sizeof(temp_buffer2)-1));
        strncat(temp_buffer1, temp_buffer2, (sizeof(temp_buffer1)-1));
    }
    strncat(temp_buffer1, "\n", (sizeof(temp_buffer1)-2));
    temp_buffer1[sizeof( temp_buffer1 )-1]='\x0';

    gm_log( GM_LOG_TRACE, "data:\n%s\n", temp_buffer1);

    if(add_job_to_queue( &client,
                         mod_gm_opt->server_list,
                         exec_job->result_queue,
                         NULL,
                         temp_buffer1,
                         GM_JOB_PRIO_NORMAL,
                         GM_DEFAULT_JOB_RETRIES,
                         mod_gm_opt->transportmode,
                         TRUE
                        ) == GM_OK) {
        gm_log( GM_LOG_TRACE, "send_result_back() finished successfully\n" );
    }
    else {
        gm_log( GM_LOG_TRACE, "send_result_back() finished unsuccessfully\n" );
    }

    if( mod_gm_opt->dupserver_num ) {
        strncpy(temp_buffer2, "type=passive\n", (sizeof(temp_buffer1)-2));
        strncat(temp_buffer2, temp_buffer1, (sizeof(temp_buffer2)-2));
        temp_buffer2[sizeof( temp_buffer2 )-1]='\x0';
        if( add_job_to_queue( &client_dup,
                              mod_gm_opt->dupserver_list,
                              exec_job->result_queue,
                              NULL,
                              temp_buffer2,
                              GM_JOB_PRIO_NORMAL,
                              GM_DEFAULT_JOB_RETRIES,
                              mod_gm_opt->transportmode,
                              TRUE
                            ) == GM_OK) {
            gm_log( GM_LOG_TRACE, "send_result_back() finished successfully for duplicate server.\n" );
        }
        else {
            gm_log( GM_LOG_TRACE, "send_result_back() finished unsuccessfully for duplicate server\n" );
        }
    }
    else {
        gm_log( GM_LOG_TRACE, "send_result_back() has no duplicate servers to send to.\n" );
    }

    return;
}


/* create the worker */
int set_worker( gearman_worker_st *w ) {
    int x = 0;

    gm_log( GM_LOG_TRACE, "set_worker()\n" );

    create_worker( mod_gm_opt->server_list, w );

    if(worker_run_mode == GM_WORKER_STATUS) {
        /* register status function */
        char status_queue[GM_BUFFERSIZE];
        snprintf(status_queue, GM_BUFFERSIZE, "worker_%s", mod_gm_opt->identifier );
        worker_add_function( w, status_queue, return_status );
    }
    else {
        /* normal worker */
        if(mod_gm_opt->hosts == GM_ENABLED)
            worker_add_function( w, "host", get_job );

        if(mod_gm_opt->services == GM_ENABLED)
            worker_add_function( w, "service", get_job );

        if(mod_gm_opt->events == GM_ENABLED)
            worker_add_function( w, "eventhandler", get_job );

        while ( mod_gm_opt->hostgroups_list[x] != NULL ) {
            char buffer[GM_BUFFERSIZE];
            snprintf( buffer, (sizeof(buffer)-1), "hostgroup_%s", mod_gm_opt->hostgroups_list[x] );
            worker_add_function( w, buffer, get_job );
            x++;
        }

        x = 0;
        while ( mod_gm_opt->servicegroups_list[x] != NULL ) {
            char buffer[GM_BUFFERSIZE];
            snprintf( buffer, (sizeof(buffer)-1), "servicegroup_%s", mod_gm_opt->servicegroups_list[x] );
            worker_add_function( w, buffer, get_job );
            x++;
        }
    }

    /* add our dummy queue, gearman sometimes forgets the last added queue */
    worker_add_function( w, "dummy", dummy);

    return GM_OK;
}


/* called when worker runs into idle timeout */
void idle_sighandler(int sig) {
    gm_log( GM_LOG_TRACE, "idle_sighandler(%i)\n", sig );
    clean_worker_exit(0);
    exit( EXIT_SUCCESS );
}


/* tell parent our state */
void set_state(int status) {
    int *shm;

    gm_log( GM_LOG_TRACE, "set_state(%d)\n", status );

    /* Now we attach the segment to our data space. */
    if ((shm = shmat(shmid, NULL, 0)) == (int *) -1) {
        perror("shmat");
        gm_log( GM_LOG_TRACE, "worker finished: %d\n", getpid() );
        clean_worker_exit(0);
        exit( EXIT_FAILURE );
    }

    if(status == GM_JOB_START)
        shm[shm_index] = current_pid;
    if(status == GM_JOB_END) {
        shm[0]++; /* increase jobs done */
        /* pid in our status slot changed, this should not happen -> exit */
        if( shm[shm_index] != current_pid && shm[shm_index] != -current_pid ) {
            gm_log( GM_LOG_ERROR, "double used worker slot: %d != %d\n", current_pid, shm[shm_index] );
            clean_worker_exit(0);
            exit( EXIT_FAILURE );
        }
        shm[shm_index] = -current_pid;
    }

    /* detach from shared memory */
    if(shmdt(shm) < 0)
        perror("shmdt");

    return;
}


/* do a clean exit */
void clean_worker_exit(int sig) {
    int *shm;

    gm_log( GM_LOG_TRACE, "clean_worker_exit(%d)\n", sig);

    gm_log( GM_LOG_TRACE, "cleaning worker\n");
    gearman_worker_unregister_all(&worker);
    gearman_job_free_all( &worker );
    gm_log( GM_LOG_TRACE, "cleaning client\n");
    gearman_client_free( &client );
    mod_gm_free_opt(mod_gm_opt);

    /* Now we attach the segment to our data space. */
    if ((shm = shmat(shmid, NULL, 0)) == (int *) -1) {
        perror("shmat");
        gm_log( GM_LOG_TRACE, "worker finished: %d\n", getpid() );
        clean_worker_exit(0);
        exit( EXIT_FAILURE );
    }
    shm[shm_index] = -1;

    /* detach from shared memory */
    if(shmdt(shm) < 0)
        perror("shmdt");

    if(worker_run_mode != GM_WORKER_STANDALONE)
        exit( EXIT_SUCCESS );

    /*
     * clean up shared memory
     * will be removed when last client detaches
     */
    if( shmctl( shmid, IPC_RMID, 0 ) == -1 ) {
        perror("shmctl");
    }

    exit( EXIT_SUCCESS );
}


/* answer status querys */
void *return_status( gearman_job_st *job, void *context, size_t *result_size, gearman_return_t *ret_ptr ) {
    int wsize;
    char workload[GM_BUFFERSIZE];
    int *shm;
    char * result;

    gm_log( GM_LOG_TRACE, "return_status()\n" );

    /* contect is unused */
    context = context;

    /* get the data */
    wsize = gearman_job_workload_size(job);
    strncpy(workload, (const char*)gearman_job_workload(job), wsize);
    workload[wsize] = '\0';
    gm_log( GM_LOG_TRACE, "got status job %s\n", gearman_job_handle( job ) );
    gm_log( GM_LOG_TRACE, "%d +++>\n%s\n<+++\n", strlen(workload), workload );

    /* set result pointer to success */
    *ret_ptr= GEARMAN_SUCCESS;

    /* set size of result */
    result = malloc(GM_BUFFERSIZE);
    *result_size = GM_BUFFERSIZE;

    /* Now we attach the segment to our data space. */
    if ((shm = shmat(shmid, NULL, 0)) == (int *) -1) {
        perror("shmat");
        *result_size = 0;
        return NULL;
    }

    snprintf(result, GM_BUFFERSIZE, "%s has %i worker and is working on %i jobs. Version: %s|worker=%i jobs=%ic", hostname, shm[1], shm[2], GM_VERSION, shm[1], shm[0] );

    /* detach from shared memory */
    if(shmdt(shm) < 0)
        perror("shmdt");

    return((void*)result);
}


#ifdef GM_DEBUG
/* write text to a debug file */
void write_debug_file(char ** text) {
    FILE * fd;
    fd = fopen( "/tmp/mod_gearman_worker.txt", "a+" );
    if(fd == NULL)
        perror("fopen");
    fputs( "------------->\n", fd );
    fputs( *text, fd );
    fputs( "\n<-------------\n", fd );
    fclose( fd );
}
#endif
