#include <curl/curl.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <pthread.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/timerfd.h>

#include "error.h"
#include "log.h"
#include "monitoring.h"
#include "procfs.h"

typedef struct {
    char *url;
    char *credentials;
    char *wf_label;
    char *wf_uuid;
    char *dag_job_id;
    char *condor_job_id;
    char *xformation;
    char *task_id;
    int socket;
    int interval;
} MonitoringContext;

/* This pipe is used to send a shutdown message from the main thread to the
 * monitoring thread */
static int signal_pipe[2];
static pthread_t monitoring_thread;

// a util function for reading env variables by the main kickstart process
// with monitoring endpoint data or set default values
static int initialize_monitoring_context(MonitoringContext *ctx) {
    char* envptr;

    envptr = getenv("KICKSTART_MON_ENDPOINT_URL");
    if (envptr == NULL) {
        printerr("ERROR: KICKSTART_MON_ENDPOINT_URL not specified\n");
        return -1;
    }
    ctx->url = strdup(envptr);

    envptr = getenv("KICKSTART_MON_ENDPOINT_CREDENTIALS");
    if (envptr == NULL) {
        printerr("ERROR: KICKSTART_MON_ENDPOINT_CREDENTIALS not specified\n");
        return -1;
    }
    ctx->credentials = strdup(envptr);

    envptr = getenv("PEGASUS_WF_UUID");
    if (envptr == NULL) {
        printerr("ERROR: PEGASUS_WF_UUID not specified\n");
        return -1;
    }
    ctx->wf_uuid = strdup(envptr);

    envptr = getenv("PEGASUS_WF_LABEL");
    if (envptr == NULL) {
        printerr("ERROR: PEGASUS_WF_LABEL not specified\n");
        return -1;
    }
    ctx->wf_label = strdup(envptr);

    envptr = getenv("PEGASUS_DAG_JOB_ID");
    if (envptr == NULL) {
        printerr("ERROR: PEGASUS_DAG_JOB_ID not specified\n");
        return -1;
    }
    ctx->dag_job_id = strdup(envptr);

    envptr = getenv("CONDOR_JOBID");
    if (envptr == NULL) {
        printerr("ERROR: CONDOR_JOBID not specified\n");
        return -1;
    }
    ctx->condor_job_id = strdup(envptr);

    envptr = getenv("PEGASUS_XFORMATION");
    if (envptr == NULL) {
        ctx->xformation = NULL;
    } else {
        ctx->xformation = strdup(envptr);
    }

    envptr = getenv("PEGASUS_TASK_ID");
    if (envptr == NULL) {
        ctx->task_id = NULL;
    } else {
        ctx->task_id = strdup(envptr);
    }

    return 0;
}

static void release_monitoring_context(MonitoringContext* ctx) {
    if (ctx == NULL) return;
    if (ctx->url != NULL) free(ctx->url);
    if (ctx->credentials != NULL) free(ctx->credentials);
    if (ctx->wf_uuid != NULL) free(ctx->wf_uuid);
    if (ctx->wf_label != NULL) free(ctx->wf_label);
    if (ctx->dag_job_id != NULL) free(ctx->dag_job_id);
    if (ctx->condor_job_id != NULL) free(ctx->condor_job_id);
    if (ctx->xformation != NULL) free(ctx->xformation);
    if (ctx->task_id != NULL) free(ctx->task_id);
    free(ctx);
}

static size_t write_callback(char *ptr, size_t size, size_t nmemb, void *userdata) {
    //    we do nothing for now
    return size * nmemb;
}

/* sending this message to rabbitmq */
static void send_msg_to_mq(char *msg_buff, MonitoringContext *ctx) {
    CURL *curl = curl_easy_init();
    if (curl == NULL) {
        printerr("[mon-thread] Error initializing curl\n");
        return;
    }

    curl_easy_setopt(curl, CURLOPT_URL, ctx->url);
    curl_easy_setopt(curl, CURLOPT_USERPWD, ctx->credentials);
    curl_easy_setopt(curl, CURLOPT_POST, 1);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);

    char payload[BUFSIZ];
    if (snprintf(payload, BUFSIZ,
        "{\"properties\":{},\"routing_key\":\"%s\",\"payload\":\"%s\",\"payload_encoding\":\"string\"}",
        ctx->wf_uuid, msg_buff) >= BUFSIZ) {
        printerr("[mon-thread] Message too large for buffer: %lu\n", strlen(msg_buff));
        return;
    }
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload);

    struct curl_slist *http_header = NULL;
    http_header = curl_slist_append(http_header, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, http_header);

    /* Perform the request, res will get the return code */
    CURLcode res = curl_easy_perform(curl);
    /* Check for errors */
    if (res != CURLE_OK) {
        printerr("[mon-thread] an error occured while sending measurement: %s\n",
            curl_easy_strerror(res));
    }

    /* always cleanup */
    curl_easy_cleanup(curl);
    curl_slist_free_all(http_header);
}

/* purpose: find an ephemeral port available on a machine for further socket-based communication;
 *          opens a new socket on an ephemeral port, returns this port number and hostname
 *          where kickstart will listen for monitoring information
 * paramtr: kickstart_hostname (OUT): a pointer where the hostname of the kickstart machine will be stored,
 *          kickstart_port (OUT): a pointer where the available ephemeral port number will be stored
 * returns:	0  success
 *		    -1 failure
 */
static int create_ephemeral_endpoint(char *kickstart_hostname, int *kickstart_port) {

    int listenfd = socket(AF_INET, SOCK_STREAM|SOCK_CLOEXEC, 0);
    if (listenfd < 0 ) {
        printerr("ERROR[socket]: %s\n", strerror(errno));
        return -1;
    }

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY); 
    serv_addr.sin_port = 0;

    if (bind(listenfd, (const struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        printerr("ERROR[bind]: %s\n", strerror(errno));
        goto error;
    }

    if (listen(listenfd, 1) < 0 ) {
        printerr("ERROR[listen]: %s\n", strerror(errno));
        goto error;
    }

    memset(&serv_addr, 0, sizeof(serv_addr));
    socklen_t addr_len = sizeof(serv_addr);
    if (getsockname(listenfd, (struct sockaddr*)&serv_addr, &addr_len) == -1) {
        printerr("ERROR[getsockname]: %s\n", strerror(errno));
        goto error;
    }

    if (gethostname(kickstart_hostname, BUFSIZ)) {
        printerr("ERROR[gethostname]: %s\n", strerror(errno));
        return -1;
    }
    *kickstart_port = ntohs(serv_addr.sin_port);

    return listenfd;

error:
    close(listenfd);
    return -1;
}

static int handle_client(MonitoringContext *ctx) {

    /* Accept a network connection and read the message */
    struct sockaddr_in client_addr;
    socklen_t client_add_len = sizeof(client_addr);
    memset(&client_addr, 0, sizeof(client_addr));
    int incoming_socket = accept(ctx->socket, (struct sockaddr *)&client_addr, &client_add_len);
    if (incoming_socket < 0) {
        printerr("[mon-thread] ERROR[accept]: %s\n", strerror(errno));
        if (errno == EINTR) {
            goto next;
        } else {
            return -1;
        }
    }

    ProcStats stats;
    int sz = recv(incoming_socket, &stats, sizeof(ProcStats), 0);
    if (sz < 0) {
        printerr("[mon-thread] ERROR[recv]: %s\n", strerror(errno));
        goto next;
    }
    if (sz != sizeof(ProcStats)) {
        printerr("Invalid message\n");
        goto next;
    }

    if (stats.host == 0) { // FIXME This should be non-zero
        /* This is a message from libinterpose */
        /* TODO Update the state of the local process in our list */
        fprintf(stdout, "INTERPOSE pid=%d exe=%s utime=%.3f stime=%.3f iowait=%.3f "
                "vm=%llu rss=%llu threads=%d bread=%llu bwrite=%llu "
                "rchar=%llu wchar=%llu syscr=%lu syscw=%lu\n",
                stats.pid, stats.exe, stats.utime, stats.stime, stats.iowait, stats.vm, stats.rss,
                stats.threads, stats.read_bytes, stats.write_bytes,
                stats.rchar, stats.wchar, stats.syscr, stats.syscw);
    } else {
        /* This is a message from another host */
        /* TODO Update the state of the remote process in our list */
        fprintf(stdout, "REMOTE pid=%d exe=%s utime=%.3f stime=%.3f iowait=%.3f "
            "vm=%llu rss=%llu threads=%d bread=%llu bwrite=%llu "
            "rchar=%llu wchar=%llu syscr=%lu syscw=%lu\n",
            stats.pid, stats.exe, stats.utime, stats.stime, stats.iowait,
            stats.vm, stats.rss,
            stats.threads, stats.read_bytes, stats.write_bytes,
            stats.rchar, stats.wchar, stats.syscr, stats.syscw);
    }

next:
    close(incoming_socket);
    return 0;
}

static int send_report(MonitoringContext *ctx, ProcStatsList **listptr) {
    ProcStats stats;
    procfs_merge_stats_list(*listptr, &stats);

    char msg[BUFSIZ];
    snprintf(msg, BUFSIZ, "wf_uuid=%s wf_label=%s dag_job_id=%s condor_job_id=%s "
            "xformation=%s task_id=%s pid=%d exe=%s utime=%.3f stime=%.3f iowait=%.3f "
            "vm=%llu rss=%llu threads=%d bread=%llu bwrite=%llu "
            "rchar=%llu wchar=%llu syscr=%lu syscw=%lu",
            ctx->wf_uuid, ctx->wf_label, ctx->dag_job_id, ctx->condor_job_id,
            ctx->xformation, ctx->task_id, stats.pid, stats.exe, stats.utime, stats.stime, stats.iowait,
            stats.vm, stats.rss, stats.threads, stats.read_bytes, stats.write_bytes,
            stats.rchar, stats.wchar, stats.syscr, stats.syscw);

    fprintf(stdout, "LOCAL %s\n", msg);
    fprintf(stdout, "%lu\n", sizeof(ProcStats));

    send_msg_to_mq(msg, ctx);

    return 0;
}

void* monitoring_thread_func(void* arg) {
    MonitoringContext *ctx = (MonitoringContext *)arg;

    info("Monitoring thread starting...");
    debug("url: %s", ctx->url);
    debug("credentials: %s", ctx->credentials);
    debug("wf uuid: %s", ctx->wf_uuid);
    debug("wf label: %s", ctx->wf_label);
    debug("dag job id: %s", ctx->dag_job_id);
    debug("condor job id: %s", ctx->condor_job_id);
    debug("xformation: %s", ctx->xformation);
    debug("task id: %s", ctx->task_id);
    debug("process group: %d", getpgid(0));

    curl_global_init(CURL_GLOBAL_ALL);

    /* Create timer for monitoring interval */
    int timer = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
    struct itimerspec timercfg;
    timercfg.it_value.tv_sec = ctx->interval;
    timercfg.it_value.tv_nsec = 0;
    timercfg.it_interval.tv_sec = ctx->interval; /* Fire every interval seconds */
    timercfg.it_interval.tv_nsec = 0;
    if (timerfd_settime(timer, 0, &timercfg, NULL) < 0) {
        printerr("Error setting timerfd time: %s\n", strerror(errno));
        pthread_exit(NULL);
    }

    ProcStatsList *stats = NULL;

    int signalled = 0;
    while (!signalled) {

        /* Poll signal_pipe, socket, and timer to see which one is readable */
        struct pollfd fds[3];
        fds[0].fd = signal_pipe[0];
        fds[0].events = POLLIN;
        fds[1].fd = ctx->socket;
        fds[1].events = POLLIN;
        fds[2].fd = timer;
        fds[2].events = POLLIN;
        if (poll(fds, 3, -1) <= 0) {
            printerr("Error polling: %s\n", strerror(errno));
            break;
        }

        /* If signal_pipe[0] is readable, then stop the thread. Note that you
         * could theoretically have some clients waiting for accept(). That is
         * not too likely because by the time we are stoping the thread,
         * wait() has returned in the main thread, so there shouldn't be any
         * clients left, but if libinterpose is used, then it will send a
         * last message when it exits, which might not arrive until later.
         */
        if (fds[0].revents & POLLIN) {
            debug("Monitoring thread caught signal");
            signalled = 1;
        }

        /* Got a client connection */
        if (fds[1].revents & POLLIN) {
            if (handle_client(ctx) < 0) {
                break;
            }

            if (signalled) {
                /* Must be the last process on libinterpose, send a message */
                send_report(ctx, &stats);
            }
        }

        /* Timer fired */
        if (fds[2].revents & POLLIN) {
            unsigned long long expirations = 0;
            if (read(timer, &expirations, sizeof(expirations)) < 0) {
                error("timerfd read failed: %s\n", strerror(errno));
            } else if (expirations > 1) {
                warn("timer expired %llu times\n", expirations);
            }

            /* Update the list of process stats */
            procfs_read_stats_group(&stats);

            /* Send a monitoring message */
            send_report(ctx, &stats);
        }
    }

    info("Monitoring thread exiting");
    procfs_free_stats_list(stats);
    close(timer);
    close(ctx->socket);
    curl_global_cleanup();
    release_monitoring_context(ctx);
    pthread_exit(NULL);
}

int start_monitoring_thread(int interval) {
    /* Make sure the calling process is in its own process group */
    setpgid(0, 0);

    /* Find a host and port to use */
    char socket_host[BUFSIZ];
    int socket_port;
    int socket = create_ephemeral_endpoint(socket_host, &socket_port);
    if (socket < 0) {
        printerr("Couldn't find an endpoint for communication with kickstart\n");
        return -1;
    }

    /* TODO Determine whether to report to a higher-level kickstart, or to monitord, or to rabbitmq */

    /* Set the monitoring environment */
    char envvar[10];
    setenv("KICKSTART_MON", "1", 1);
    snprintf(envvar, 10, "%d", interval);
    setenv("KICKSTART_MON_INTERVAL", envvar, 1);
    setenv("KICKSTART_MON_HOST", socket_host, 1);
    snprintf(envvar, 10, "%d", socket_port);
    setenv("KICKSTART_MON_PORT", envvar, 1);

    /* Set up parameters for the thread */
    MonitoringContext *ctx = calloc(1, sizeof(MonitoringContext));
    if (initialize_monitoring_context(ctx) < 0) {
        return -1;
    }
    ctx->socket = socket;
    ctx->interval = interval;

    /* Create a pipe to signal between the main thread and the monitor thread */
    int rc = pipe(signal_pipe);
    if (rc < 0) {
        printerr("ERROR: Unable to create signal pipe: %s\n", strerror(errno));
        return rc;
    }
    rc = fcntl(signal_pipe[0], F_SETFD, FD_CLOEXEC);
    if (rc < 0) {
        printerr("WARNING: Unable to set CLOEXEC on pipe: %s\n", strerror(errno));
    }
    rc = fcntl(signal_pipe[1], F_SETFD, FD_CLOEXEC);
    if (rc < 0) {
        printerr("WARNING: Unable to set CLOEXEC on pipe: %s\n", strerror(errno));
    }

    /* Start and detach the monitoring thread */
    rc = pthread_create(&monitoring_thread, NULL, monitoring_thread_func, (void*)ctx);
    if (rc) {
        printerr("ERROR: return code from pthread_create() is %d: %s\n", rc, strerror(errno));
        return rc;
    }

    return 0;
}

int stop_monitoring_thread() {
    /* Signal the thread to stop */
    char msg = 1;
    int rc = write(signal_pipe[1], &msg, 1);
    if (rc <= 0) {
        printerr("ERROR: Problem signalling monitoring thread: %s\n", strerror(errno));
        return rc;
    }

    /* Wait for the monitoring thread */
    pthread_join(monitoring_thread, NULL);

    /* Close the pipe */
    close(signal_pipe[0]);
    close(signal_pipe[1]);

    return 0;
}
