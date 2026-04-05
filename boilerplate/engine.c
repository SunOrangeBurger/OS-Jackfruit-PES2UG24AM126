/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
 *
 * Intentionally partial starter:
 *   - command-line shape is defined
 *   - key runtime data structures are defined
 *   - bounded-buffer skeleton is defined
 *   - supervisor / client split is outlined
 *
 * Students are expected to design:
 *   - the control-plane IPC implementation
 *   - container lifecycle and metadata synchronization
 *   - clone + namespace setup for each container
 *   - producer/consumer behavior for log buffering
 *   - signal handling and graceful shutdown
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "monitor_ioctl.h"

#define STACK_SIZE (1024 * 1024)
#define CONTAINER_ID_LEN 32
#define CONTROL_PATH "/tmp/mini_runtime.sock"
#define LOG_DIR "logs"
#define CONTROL_MESSAGE_LEN 256
#define CHILD_COMMAND_LEN 256
#define LOG_CHUNK_SIZE 4096
#define LOG_BUFFER_CAPACITY 16
#define DEFAULT_SOFT_LIMIT (40UL << 20)
#define DEFAULT_HARD_LIMIT (64UL << 20)

typedef enum {
    CMD_SUPERVISOR = 0,
    CMD_START,
    CMD_RUN,
    CMD_PS,
    CMD_LOGS,
    CMD_STOP
} command_kind_t;

typedef enum {
    CONTAINER_STARTING = 0,
    CONTAINER_RUNNING,
    CONTAINER_STOPPED,
    CONTAINER_KILLED,
    CONTAINER_EXITED
} container_state_t;

typedef struct container_record {
    char id[CONTAINER_ID_LEN];
    pid_t host_pid;
    time_t started_at;
    container_state_t state;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int exit_code;
    int exit_signal;
    char log_path[PATH_MAX];
    struct container_record *next;
} container_record_t;

typedef struct {
    char container_id[CONTAINER_ID_LEN];
    size_t length;
    char data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t items[LOG_BUFFER_CAPACITY];
    size_t head;
    size_t tail;
    size_t count;
    int shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} bounded_buffer_t;

typedef struct {
    command_kind_t kind;
    char container_id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int nice_value;
} control_request_t;

typedef struct {
    int status;
    char message[CONTROL_MESSAGE_LEN];
} control_response_t;

typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int nice_value;
    int log_write_fd;
} child_config_t;

typedef struct {
    int server_fd;
    int monitor_fd;
    int should_stop;
    pthread_t logger_thread;
    bounded_buffer_t log_buffer;
    pthread_mutex_t metadata_lock;
    container_record_t *containers;
} supervisor_ctx_t;

typedef struct {
    int read_fd;
    char container_id[CONTAINER_ID_LEN];
    bounded_buffer_t *log_buffer;
} pipe_reader_arg_t;

static int bounded_buffer_push(bounded_buffer_t *buffer, const log_item_t *item);

static void *pipe_reader_thread(void *arg)
{
    pipe_reader_arg_t *parg = (pipe_reader_arg_t *)arg;
    char buffer[LOG_CHUNK_SIZE];
    ssize_t n;

    while ((n = read(parg->read_fd, buffer, sizeof(buffer) - 1)) > 0) {
        log_item_t item;
        memset(&item, 0, sizeof(item));
        strncpy(item.container_id, parg->container_id, sizeof(item.container_id) - 1);
        item.length = (size_t)n;
        memcpy(item.data, buffer, (size_t)n);
        bounded_buffer_push(parg->log_buffer, &item);
    }

    close(parg->read_fd);
    free(parg);
    return NULL;
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s supervisor <base-rootfs>\n"
            "  %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s ps\n"
            "  %s logs <id>\n"
            "  %s stop <id>\n",
            prog, prog, prog, prog, prog, prog);
}

static int parse_mib_flag(const char *flag,
                          const char *value,
                          unsigned long *target_bytes)
{
    char *end = NULL;
    unsigned long mib;

    errno = 0;
    mib = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        fprintf(stderr, "Invalid value for %s: %s\n", flag, value);
        return -1;
    }

    if (mib > ULONG_MAX / (1UL << 20)) {
        fprintf(stderr, "Value for %s is too large: %s\n", flag, value);
        return -1;
    }

    *target_bytes = mib * (1UL << 20);
    return 0;
}

static int parse_optional_flags(control_request_t *req,
                                int argc,
                                char *argv[],
                                int start_index)
{
    int i;

    for (i = start_index; i < argc; i += 2) {
        char *end = NULL;
        long nice_value;

        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for option: %s\n", argv[i]);
            return -1;
        }

        if (strcmp(argv[i], "--soft-mib") == 0) {
            if (parse_mib_flag("--soft-mib", argv[i + 1], &req->soft_limit_bytes) != 0)
                return -1;
            continue;
        }

        if (strcmp(argv[i], "--hard-mib") == 0) {
            if (parse_mib_flag("--hard-mib", argv[i + 1], &req->hard_limit_bytes) != 0)
                return -1;
            continue;
        }

        if (strcmp(argv[i], "--nice") == 0) {
            errno = 0;
            nice_value = strtol(argv[i + 1], &end, 10);
            if (errno != 0 || end == argv[i + 1] || *end != '\0' ||
                nice_value < -20 || nice_value > 19) {
                fprintf(stderr,
                        "Invalid value for --nice (expected -20..19): %s\n",
                        argv[i + 1]);
                return -1;
            }
            req->nice_value = (int)nice_value;
            continue;
        }

        fprintf(stderr, "Unknown option: %s\n", argv[i]);
        return -1;
    }

    if (req->soft_limit_bytes > req->hard_limit_bytes) {
        fprintf(stderr, "Invalid limits: soft limit cannot exceed hard limit\n");
        return -1;
    }

    return 0;
}

static const char *state_to_string(container_state_t state)
{
    switch (state) {
    case CONTAINER_STARTING:
        return "starting";
    case CONTAINER_RUNNING:
        return "running";
    case CONTAINER_STOPPED:
        return "stopped";
    case CONTAINER_KILLED:
        return "killed";
    case CONTAINER_EXITED:
        return "exited";
    default:
        return "unknown";
    }
}

static int bounded_buffer_init(bounded_buffer_t *buffer)
{
    int rc;

    memset(buffer, 0, sizeof(*buffer));

    rc = pthread_mutex_init(&buffer->mutex, NULL);
    if (rc != 0)
        return rc;

    rc = pthread_cond_init(&buffer->not_empty, NULL);
    if (rc != 0) {
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }

    rc = pthread_cond_init(&buffer->not_full, NULL);
    if (rc != 0) {
        pthread_cond_destroy(&buffer->not_empty);
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }

    return 0;
}

static void bounded_buffer_destroy(bounded_buffer_t *buffer)
{
    pthread_cond_destroy(&buffer->not_full);
    pthread_cond_destroy(&buffer->not_empty);
    pthread_mutex_destroy(&buffer->mutex);
}

static void bounded_buffer_begin_shutdown(bounded_buffer_t *buffer)
{
    pthread_mutex_lock(&buffer->mutex);
    buffer->shutting_down = 1;
    pthread_cond_broadcast(&buffer->not_empty);
    pthread_cond_broadcast(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
}

/*
 * TODO:
 * Implement producer-side insertion into the bounded buffer.
 *
 * Requirements:
 *   - block or fail according to your chosen policy when the buffer is full
 *   - wake consumers correctly
 *   - stop cleanly if shutdown begins
 */
int bounded_buffer_push(bounded_buffer_t *buffer, const log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);

    while (buffer->count == LOG_BUFFER_CAPACITY && !buffer->shutting_down)
        pthread_cond_wait(&buffer->not_full, &buffer->mutex);

    if (buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    buffer->items[buffer->tail] = *item;
    buffer->tail = (buffer->tail + 1) % LOG_BUFFER_CAPACITY;
    buffer->count++;

    pthread_cond_signal(&buffer->not_empty);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

/*
 * TODO:
 * Implement consumer-side removal from the bounded buffer.
 *
 * Requirements:
 *   - wait correctly while the buffer is empty
 *   - return a useful status when shutdown is in progress
 *   - avoid races with producers and shutdown
 */
int bounded_buffer_pop(bounded_buffer_t *buffer, log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);

    while (buffer->count == 0 && !buffer->shutting_down)
        pthread_cond_wait(&buffer->not_empty, &buffer->mutex);

    if (buffer->count == 0 && buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    *item = buffer->items[buffer->head];
    buffer->head = (buffer->head + 1) % LOG_BUFFER_CAPACITY;
    buffer->count--;

    pthread_cond_signal(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

/*
 * TODO:
 * Implement the logging consumer thread.
 *
 * Suggested responsibilities:
 *   - remove log chunks from the bounded buffer
 *   - route each chunk to the correct per-container log file
 *   - exit cleanly when shutdown begins and pending work is drained
 */
void *logging_thread(void *arg)
{
    supervisor_ctx_t *ctx = (supervisor_ctx_t *)arg;
    log_item_t item;

    while (1) {
        if (bounded_buffer_pop(&ctx->log_buffer, &item) != 0)
            break;

        char log_path[PATH_MAX];
        snprintf(log_path, sizeof(log_path), "%s/%s.log", LOG_DIR, item.container_id);

        int fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) {
            write(fd, item.data, item.length);
            close(fd);
        }
    }

    return NULL;
}

/*
 * TODO:
 * Implement the clone child entrypoint.
 *
 * Required outcomes:
 *   - isolated PID / UTS / mount context
 *   - chroot or pivot_root into rootfs
 *   - working /proc inside container
 *   - stdout / stderr redirected to the supervisor logging path
 *   - configured command executed inside the container
 */
int child_fn(void *arg)
{
    child_config_t *config = (child_config_t *)arg;

    if (sethostname(config->id, strlen(config->id)) != 0) {
        perror("sethostname");
        return 1;
    }

    if (chroot(config->rootfs) != 0) {
        perror("chroot");
        return 1;
    }

    if (chdir("/") != 0) {
        perror("chdir");
        return 1;
    }

    if (mount("proc", "/proc", "proc", 0, NULL) != 0) {
        perror("mount /proc");
    }

    if (config->nice_value != 0) {
        if (nice(config->nice_value) == -1 && errno != 0) {
            perror("nice");
        }
    }

    if (config->log_write_fd >= 0) {
        dup2(config->log_write_fd, STDOUT_FILENO);
        dup2(config->log_write_fd, STDERR_FILENO);
        close(config->log_write_fd);
    }

    char *argv[] = {config->command, NULL};
    char *envp[] = {NULL};
    execve(config->command, argv, envp);

    perror("execve");
    return 1;
}

int register_with_monitor(int monitor_fd,
                          const char *container_id,
                          pid_t host_pid,
                          unsigned long soft_limit_bytes,
                          unsigned long hard_limit_bytes)
{
    struct monitor_request req;

    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    req.soft_limit_bytes = soft_limit_bytes;
    req.hard_limit_bytes = hard_limit_bytes;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_REGISTER, &req) < 0)
        return -1;

    return 0;
}

int unregister_from_monitor(int monitor_fd, const char *container_id, pid_t host_pid)
{
    struct monitor_request req;

    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_UNREGISTER, &req) < 0)
        return -1;

    return 0;
}

/*
 * TODO:
 * Implement the long-running supervisor process.
 *
 * Suggested responsibilities:
 *   - create and bind the control-plane IPC endpoint
 *   - initialize shared metadata and the bounded buffer
 *   - start the logging thread
 *   - accept control requests and update container state
 *   - reap children and respond to signals
 */
static int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t ctx;
    int rc;
    struct sockaddr_un addr;

    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd = -1;
    ctx.monitor_fd = -1;

    rc = pthread_mutex_init(&ctx.metadata_lock, NULL);
    if (rc != 0) {
        errno = rc;
        perror("pthread_mutex_init");
        return 1;
    }

    rc = bounded_buffer_init(&ctx.log_buffer);
    if (rc != 0) {
        errno = rc;
        perror("bounded_buffer_init");
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    /*
     * TODO:
     *   1) open /dev/container_monitor
     *   2) create the control socket / FIFO / shared-memory channel
     *   3) install SIGCHLD / SIGINT / SIGTERM handling
     *   4) spawn the logger thread
     *   5) enter the supervisor event loop
     */

    mkdir(LOG_DIR, 0755);

    ctx.monitor_fd = open("/dev/container_monitor", O_RDWR);
    if (ctx.monitor_fd < 0) {
        perror("open /dev/container_monitor");
    }

    unlink(CONTROL_PATH);
    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx.server_fd < 0) {
        perror("socket");
        if (ctx.monitor_fd >= 0) close(ctx.monitor_fd);
        bounded_buffer_destroy(&ctx.log_buffer);
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(ctx.server_fd);
        if (ctx.monitor_fd >= 0) close(ctx.monitor_fd);
        bounded_buffer_destroy(&ctx.log_buffer);
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    if (listen(ctx.server_fd, 5) < 0) {
        perror("listen");
        close(ctx.server_fd);
        if (ctx.monitor_fd >= 0) close(ctx.monitor_fd);
        bounded_buffer_destroy(&ctx.log_buffer);
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    rc = pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx);
    if (rc != 0) {
        errno = rc;
        perror("pthread_create");
        close(ctx.server_fd);
        if (ctx.monitor_fd >= 0) close(ctx.monitor_fd);
        bounded_buffer_destroy(&ctx.log_buffer);
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    printf("Supervisor started with base-rootfs: %s\n", rootfs);
    printf("Control socket: %s\n", CONTROL_PATH);

    while (!ctx.should_stop) {
        fd_set readfds;
        struct timeval tv;
        int client_fd;

        FD_ZERO(&readfds);
        FD_SET(ctx.server_fd, &readfds);
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        rc = select(ctx.server_fd + 1, &readfds, NULL, NULL, &tv);
        if (rc < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }

        if (rc == 0) {
            int status;
            pid_t pid = waitpid(-1, &status, WNOHANG);
            if (pid > 0) {
                pthread_mutex_lock(&ctx.metadata_lock);
                container_record_t *rec = ctx.containers;
                while (rec) {
                    if (rec->host_pid == pid) {
                        if (WIFEXITED(status)) {
                            rec->exit_code = WEXITSTATUS(status);
                            rec->state = CONTAINER_EXITED;
                        } else if (WIFSIGNALED(status)) {
                            rec->exit_signal = WTERMSIG(status);
                            rec->state = (rec->exit_signal == SIGKILL) ? CONTAINER_KILLED : CONTAINER_STOPPED;
                        }
                        if (ctx.monitor_fd >= 0) {
                            unregister_from_monitor(ctx.monitor_fd, rec->id, pid);
                        }
                        break;
                    }
                    rec = rec->next;
                }
                pthread_mutex_unlock(&ctx.metadata_lock);
            }
            continue;
        }

        client_fd = accept(ctx.server_fd, NULL, NULL);
        if (client_fd < 0) {
            perror("accept");
            continue;
        }

        control_request_t req;
        control_response_t resp;
        memset(&resp, 0, sizeof(resp));

        if (read(client_fd, &req, sizeof(req)) != sizeof(req)) {
            close(client_fd);
            continue;
        }

        if (req.kind == CMD_START || req.kind == CMD_RUN) {
            int pipefd[2];
            if (pipe(pipefd) < 0) {
                resp.status = 1;
                snprintf(resp.message, sizeof(resp.message), "pipe failed");
                write(client_fd, &resp, sizeof(resp));
                close(client_fd);
                continue;
            }

            child_config_t config;
            memset(&config, 0, sizeof(config));
            strncpy(config.id, req.container_id, sizeof(config.id) - 1);
            strncpy(config.rootfs, req.rootfs, sizeof(config.rootfs) - 1);
            strncpy(config.command, req.command, sizeof(config.command) - 1);
            config.nice_value = req.nice_value;
            config.log_write_fd = pipefd[1];

            char *stack = malloc(STACK_SIZE);
            if (!stack) {
                resp.status = 1;
                snprintf(resp.message, sizeof(resp.message), "malloc failed");
                write(client_fd, &resp, sizeof(resp));
                close(pipefd[0]);
                close(pipefd[1]);
                close(client_fd);
                continue;
            }

            pid_t pid = clone(child_fn, stack + STACK_SIZE,
                            CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
                            &config);

            close(pipefd[1]);

            if (pid < 0) {
                resp.status = 1;
                snprintf(resp.message, sizeof(resp.message), "clone failed");
                write(client_fd, &resp, sizeof(resp));
                close(pipefd[0]);
                free(stack);
                close(client_fd);
                continue;
            }

            container_record_t *rec = malloc(sizeof(*rec));
            if (rec) {
                memset(rec, 0, sizeof(*rec));
                strncpy(rec->id, req.container_id, sizeof(rec->id) - 1);
                rec->host_pid = pid;
                rec->started_at = time(NULL);
                rec->state = CONTAINER_RUNNING;
                rec->soft_limit_bytes = req.soft_limit_bytes;
                rec->hard_limit_bytes = req.hard_limit_bytes;
                snprintf(rec->log_path, sizeof(rec->log_path), "%s/%s.log", LOG_DIR, req.container_id);

                pthread_mutex_lock(&ctx.metadata_lock);
                rec->next = ctx.containers;
                ctx.containers = rec;
                pthread_mutex_unlock(&ctx.metadata_lock);

                if (ctx.monitor_fd >= 0) {
                    register_with_monitor(ctx.monitor_fd, req.container_id, pid,
                                        req.soft_limit_bytes, req.hard_limit_bytes);
                }
            }

            pthread_t reader_thread;
            pipe_reader_arg_t *parg = malloc(sizeof(*parg));
            if (parg) {
                parg->read_fd = pipefd[0];
                strncpy(parg->container_id, req.container_id, sizeof(parg->container_id) - 1);
                parg->log_buffer = &ctx.log_buffer;
                pthread_create(&reader_thread, NULL, pipe_reader_thread, parg);
                pthread_detach(reader_thread);
            } else {
                close(pipefd[0]);
            }

            resp.status = 0;
            snprintf(resp.message, sizeof(resp.message), "Container %s started with PID %d", req.container_id, pid);
            write(client_fd, &resp, sizeof(resp));

        } else if (req.kind == CMD_PS) {
            char buffer[4096] = "";
            int offset = 0;

            pthread_mutex_lock(&ctx.metadata_lock);
            container_record_t *rec = ctx.containers;
            offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                             "ID\tPID\tSTATE\tSOFT_MiB\tHARD_MiB\n");
            while (rec && offset < (int)sizeof(buffer) - 200) {
                offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                                 "%s\t%d\t%s\t%lu\t%lu\n",
                                 rec->id, rec->host_pid, state_to_string(rec->state),
                                 rec->soft_limit_bytes >> 20, rec->hard_limit_bytes >> 20);
                rec = rec->next;
            }
            pthread_mutex_unlock(&ctx.metadata_lock);

            resp.status = 0;
            strncpy(resp.message, buffer, sizeof(resp.message) - 1);
            write(client_fd, &resp, sizeof(resp));

        } else if (req.kind == CMD_LOGS) {
            char log_path[PATH_MAX];
            snprintf(log_path, sizeof(log_path), "%s/%s.log", LOG_DIR, req.container_id);

            int fd = open(log_path, O_RDONLY);
            if (fd < 0) {
                resp.status = 1;
                snprintf(resp.message, sizeof(resp.message), "Log file not found");
            } else {
                char buffer[4096];
                ssize_t n = read(fd, buffer, sizeof(buffer) - 1);
                if (n > 0) {
                    buffer[n] = '\0';
                    resp.status = 0;
                    strncpy(resp.message, buffer, sizeof(resp.message) - 1);
                } else {
                    resp.status = 0;
                    snprintf(resp.message, sizeof(resp.message), "Log is empty");
                }
                close(fd);
            }
            write(client_fd, &resp, sizeof(resp));

        } else if (req.kind == CMD_STOP) {
            pthread_mutex_lock(&ctx.metadata_lock);
            container_record_t *rec = ctx.containers;
            int found = 0;
            while (rec) {
                if (strcmp(rec->id, req.container_id) == 0) {
                    if (rec->state == CONTAINER_RUNNING) {
                        kill(rec->host_pid, SIGTERM);
                        rec->state = CONTAINER_STOPPED;
                        found = 1;
                    }
                    break;
                }
                rec = rec->next;
            }
            pthread_mutex_unlock(&ctx.metadata_lock);

            resp.status = found ? 0 : 1;
            snprintf(resp.message, sizeof(resp.message), found ? "Container stopped" : "Container not found");
            write(client_fd, &resp, sizeof(resp));
        }

        close(client_fd);
    }

    bounded_buffer_begin_shutdown(&ctx.log_buffer);
    pthread_join(ctx.logger_thread, NULL);

    close(ctx.server_fd);
    if (ctx.monitor_fd >= 0) close(ctx.monitor_fd);
    bounded_buffer_destroy(&ctx.log_buffer);
    pthread_mutex_destroy(&ctx.metadata_lock);
    return 0;
}

/*
 * TODO:
 * Implement the client-side control request path.
 *
 * The CLI commands should use a second IPC mechanism distinct from the
 * logging pipe. A UNIX domain socket is the most direct option, but a
 * FIFO or shared memory design is also acceptable if justified.
 */
static int send_control_request(const control_request_t *req)
{
    int sock;
    struct sockaddr_un addr;
    control_response_t resp;

    sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return 1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(sock);
        return 1;
    }

    if (write(sock, req, sizeof(*req)) != sizeof(*req)) {
        perror("write");
        close(sock);
        return 1;
    }

    if (read(sock, &resp, sizeof(resp)) != sizeof(resp)) {
        perror("read");
        close(sock);
        return 1;
    }

    printf("%s\n", resp.message);
    close(sock);
    return resp.status;
}

static int cmd_start(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_START;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    if (parse_optional_flags(&req, argc, argv, 5) != 0)
        return 1;

    return send_control_request(&req);
}

static int cmd_run(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_RUN;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    if (parse_optional_flags(&req, argc, argv, 5) != 0)
        return 1;

    return send_control_request(&req);
}

static int cmd_ps(void)
{
    control_request_t req;

    memset(&req, 0, sizeof(req));
    req.kind = CMD_PS;

    /*
     * TODO:
     * The supervisor should respond with container metadata.
     * Keep the rendering format simple enough for demos and debugging.
     */
    printf("Expected states include: %s, %s, %s, %s, %s\n",
           state_to_string(CONTAINER_STARTING),
           state_to_string(CONTAINER_RUNNING),
           state_to_string(CONTAINER_STOPPED),
           state_to_string(CONTAINER_KILLED),
           state_to_string(CONTAINER_EXITED));
    return send_control_request(&req);
}

static int cmd_logs(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s logs <id>\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_LOGS;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);

    return send_control_request(&req);
}

static int cmd_stop(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s stop <id>\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);

    return send_control_request(&req);
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]);
            return 1;
        }
        return run_supervisor(argv[2]);
    }

    if (strcmp(argv[1], "start") == 0)
        return cmd_start(argc, argv);

    if (strcmp(argv[1], "run") == 0)
        return cmd_run(argc, argv);

    if (strcmp(argv[1], "ps") == 0)
        return cmd_ps();

    if (strcmp(argv[1], "logs") == 0)
        return cmd_logs(argc, argv);

    if (strcmp(argv[1], "stop") == 0)
        return cmd_stop(argc, argv);

    usage(argv[0]);
    return 1;
}
