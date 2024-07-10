// SPDX-License-Identifier: GPL-3.0-or-later

#include "../libnetdata.h"

#include "spawn_server.h"

#if defined(OS_WINDOWS)
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#include <process.h>
#include <sys/cygwin.h>
#endif

struct spawn_server {
    size_t id;
    size_t request_id;
    const char *name;
#if !defined(OS_WINDOWS)
    SPAWN_SERVER_OPTIONS options;

    ND_UUID magic;          // for authorizing requests, the client needs to know our random UUID
                            // it is ignored for PING requests

    int pipe[2];
    int server_sock;
    pid_t server_pid;
    char *path;
    spawn_request_callback_t cb;

    int argc;
    const char **argv;
#endif
};

struct spawm_instance {
    size_t request_id;
    int client_sock;
    int write_fd;
    int read_fd;
    pid_t child_pid;

#if defined(OS_WINDOWS)
    HANDLE process_handle;
    HANDLE read_handle;
    HANDLE write_handle;
#endif
};

int spawn_server_instance_read_fd(SPAWN_INSTANCE *si) { return si->read_fd; }
int spawn_server_instance_write_fd(SPAWN_INSTANCE *si) { return si->write_fd; }
pid_t spawn_server_instance_pid(SPAWN_INSTANCE *si) { return si->child_pid; }
void spawn_server_instance_read_fd_unset(SPAWN_INSTANCE *si) { si->read_fd = -1; }
void spawn_server_instance_write_fd_unset(SPAWN_INSTANCE *si) { si->write_fd = -1; }

#if defined(OS_WINDOWS)

SPAWN_SERVER* spawn_server_create(SPAWN_SERVER_OPTIONS options __maybe_unused, const char *name, spawn_request_callback_t cb  __maybe_unused, int argc __maybe_unused, const char **argv __maybe_unused) {
    SPAWN_SERVER* server = callocz(1, sizeof(SPAWN_SERVER));
    if(name)
        server->name = strdupz(name);
    return server;
}

void spawn_server_destroy(SPAWN_SERVER *server) {
    if (server) {
        if(server->name) freez((void *)server->name);
        freez(server);
    }
}

static BUFFER *argv_to_windows(const char **argv) {
    BUFFER *wb = buffer_create(0, NULL);

    // argv[0] is the path
    char b[strlen(argv[0]) * 2 + 1024];
    cygwin_conv_path(CCP_POSIX_TO_WIN_A | CCP_ABSOLUTE, argv[0], b, sizeof(b));

    buffer_strcat(wb, "cmd.exe /C ");

    for(size_t i = 0; argv[i] ;i++) {
        const char *s = (i == 0) ? b : argv[i];
        size_t len = strlen(s);
        buffer_need_bytes(wb, len * 2 + 1);

        bool needs_quotes = false;
        for(const char *c = s; !needs_quotes && *c ; c++) {
            switch(*c) {
                case ' ':
                case '\v':
                case '\t':
                case '\n':
                case '"':
                    needs_quotes = true;
                    break;

                default:
                    break;
            }
        }

        if(needs_quotes && buffer_strlen(wb))
            buffer_strcat(wb, " \"");
        else
            buffer_putc(wb, ' ');

        for(const char *c = s; *c ; c++) {
            switch(*c) {
                case '"':
                    buffer_putc(wb, '\\');
                    // fall through

                default:
                    buffer_putc(wb, *c);
                    break;
            }
        }

        if(needs_quotes)
            buffer_strcat(wb, "\"");
    }

    return wb;
}

SPAWN_INSTANCE* spawn_server_exec(SPAWN_SERVER *server, int stderr_fd, int custom_fd __maybe_unused, const char **argv, const void *data __maybe_unused, size_t data_size __maybe_unused, SPAWN_INSTANCE_TYPE type) {
    static SPINLOCK spinlock = NETDATA_SPINLOCK_INITIALIZER;

    if (type != SPAWN_INSTANCE_TYPE_EXEC)
        return NULL;

    int pipe_stdin[2] = { -1, -1 }, pipe_stdout[2] = { -1, -1 };

    errno_clear();

    SPAWN_INSTANCE *instance = callocz(1, sizeof(*instance));
    instance->request_id = __atomic_add_fetch(&server->request_id, 1, __ATOMIC_RELAXED);

    CLEAN_BUFFER *wb = argv_to_windows(argv);
    char *command = (char *)buffer_tostring(wb);

    if (pipe(pipe_stdin) == -1) {
        nd_log(NDLS_COLLECTORS, NDLP_ERR,
               "SPAWN PARENT: Cannot create stdin pipe() for request No %zu, command: %s",
               instance->request_id, command);
        goto cleanup;
    }

    if (pipe(pipe_stdout) == -1) {
        nd_log(NDLS_COLLECTORS, NDLP_ERR,
               "SPAWN PARENT: Cannot create stdout pipe() for request No %zu, command: %s",
               instance->request_id, command);
        goto cleanup;
    }

    // do not run multiple times this section
    // to prevent handles leaking
    spinlock_lock(&spinlock);

    // Convert POSIX file descriptors to Windows handles
    HANDLE stdin_read_handle = (HANDLE)_get_osfhandle(pipe_stdin[0]);
    HANDLE stdout_write_handle = (HANDLE)_get_osfhandle(pipe_stdout[1]);
    HANDLE stderr_handle = (HANDLE)_get_osfhandle(stderr_fd);

    if (stdin_read_handle == INVALID_HANDLE_VALUE || stdout_write_handle == INVALID_HANDLE_VALUE || stderr_handle == INVALID_HANDLE_VALUE) {
        spinlock_unlock(&spinlock);
        nd_log(NDLS_COLLECTORS, NDLP_ERR,
               "SPAWN PARENT: Invalid handle value(s) for request No %zu, command: %s",
               instance->request_id, command);
        goto cleanup;
    }

    // Set handle inheritance
    if (!SetHandleInformation(stdin_read_handle, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT) ||
        !SetHandleInformation(stdout_write_handle, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT) ||
        !SetHandleInformation(stderr_handle, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT)) {
        spinlock_unlock(&spinlock);
        nd_log(NDLS_COLLECTORS, NDLP_ERR,
               "SPAWN PARENT: Cannot set handle(s) inheritance for request No %zu, command: %s",
               instance->request_id, command);
        goto cleanup;
    }

    // Set up the STARTUPINFO structure
    STARTUPINFO si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = stdin_read_handle;
    si.hStdOutput = stdout_write_handle;
    si.hStdError = stderr_handle;

    nd_log(NDLS_COLLECTORS, NDLP_ERR,
           "SPAWN PARENT: Running request No %zu, command: %s",
           instance->request_id, command);

    // Spawn the process
    if (!CreateProcess(NULL, command, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        spinlock_unlock(&spinlock);
        nd_log(NDLS_COLLECTORS, NDLP_ERR,
               "SPAWN PARENT: cannot CreateProcess() for request No %zu, command: %s",
               instance->request_id, command);
        goto cleanup;
    }

    CloseHandle(pi.hThread);

    // end of the critical section
    spinlock_unlock(&spinlock);

    // Close unused pipe ends
    close(pipe_stdin[0]); pipe_stdin[0] = -1;
    close(pipe_stdout[1]); pipe_stdout[1] = -1;

    // Store process information in instance
    instance->child_pid = cygwin_winpid_to_pid(pi.dwProcessId);
    if(instance->child_pid == -1) instance->child_pid = pi.dwProcessId;

    instance->process_handle = pi.hProcess;

    // Convert handles to POSIX file descriptors
    instance->write_fd = pipe_stdin[1];
    instance->read_fd = pipe_stdout[0];

    errno_clear();
    nd_log(NDLS_COLLECTORS, NDLP_ERR,
           "SPAWN PARENT: created process for request No %zu, pid %d, command: %s",
           instance->request_id, (int)instance->child_pid, command);

    return instance;

cleanup:
    if (pipe_stdin[0] >= 0) close(pipe_stdin[0]);
    if (pipe_stdin[1] >= 0) close(pipe_stdin[1]);
    if (pipe_stdout[0] >= 0) close(pipe_stdout[0]);
    if (pipe_stdout[1] >= 0) close(pipe_stdout[1]);
    freez(instance);
    return NULL;
}

int spawn_server_exec_kill(SPAWN_SERVER *server __maybe_unused, SPAWN_INSTANCE *instance) {
    if(instance->read_fd != -1) { close(instance->read_fd); instance->read_fd = -1; }
    if(instance->write_fd != -1) { close(instance->write_fd); instance->write_fd = -1; }
    CloseHandle(instance->read_handle); instance->read_handle = NULL;
    CloseHandle(instance->write_handle); instance->write_handle = NULL;

    TerminateProcess(instance->process_handle, 0);

    DWORD exit_code;
    GetExitCodeProcess(instance->process_handle, &exit_code);
    CloseHandle(instance->process_handle);

    nd_log(NDLS_COLLECTORS, NDLP_ERR,
           "SPAWN PARENT: child of request No %zu, pid %d, killed and exited with code %d",
           instance->request_id, (int)instance->child_pid, (int)exit_code);

    freez(instance);
    return (int)exit_code;
}

int spawn_server_exec_wait(SPAWN_SERVER *server __maybe_unused, SPAWN_INSTANCE *instance) {
    if(instance->read_fd != -1) { close(instance->read_fd); instance->read_fd = -1; }
    if(instance->write_fd != -1) { close(instance->write_fd); instance->write_fd = -1; }
    CloseHandle(instance->read_handle); instance->read_handle = NULL;
    CloseHandle(instance->write_handle); instance->write_handle = NULL;

    WaitForSingleObject(instance->process_handle, INFINITE);

    DWORD exit_code = -1;
    GetExitCodeProcess(instance->process_handle, &exit_code);
    CloseHandle(instance->process_handle);

    nd_log(NDLS_COLLECTORS, NDLP_ERR,
           "SPAWN PARENT: child of request No %zu, pid %d, waited and exited with code %d",
           instance->request_id, (int)instance->child_pid, (int)exit_code);

    freez(instance);
    return (int)exit_code;
}

#else // !OS_WINDOWS

#ifdef __APPLE__
#include <crt_externs.h>
#define environ (*_NSGetEnviron())
#else
extern char **environ;
#endif

static size_t spawn_server_id = 0;
static volatile bool spawn_server_exit = false;
static volatile bool spawn_server_sigchld = false;
static SPAWN_REQUEST *spawn_server_requests = NULL;

// --------------------------------------------------------------------------------------------------------------------

static int connect_to_spawn_server(const char *path, bool log) {
    int sock = -1;

    if ((sock = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
        if(log)
            nd_log(NDLS_COLLECTORS, NDLP_ERR, "SPAWN PARENT: cannot create socket() to connect to spawn server.");
        return -1;
    }

    struct sockaddr_un server_addr = {
        .sun_family = AF_UNIX,
    };
    strcpy(server_addr.sun_path, path);

    if (connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
        if(log)
            nd_log(NDLS_COLLECTORS, NDLP_ERR, "SPAWN PARENT: Cannot connect() to spawn server.");
        close(sock);
        return -1;
    }

    return sock;
}

// --------------------------------------------------------------------------------------------------------------------
// the child created by the spawn server

typedef enum __attribute__((packed)) {
    STATUS_REPORT_STARTED,
    STATUS_REPORT_FAILED,
    STATUS_REPORT_EXITED,
    STATUS_REPORT_PING,
} STATUS_REPORT;

struct status_report {
    STATUS_REPORT status;
    union {
        struct {
            pid_t pid;
        } started;

        struct {
            int err_no;
        } failed;

        struct {
            int waitpid_status;
        } exited;
    };
};

static void spawn_server_send_status_ping(int fd) {
    struct status_report sr = {
        .status = STATUS_REPORT_PING,
    };

    if(write(fd, &sr, sizeof(sr)) != sizeof(sr))
        nd_log(NDLS_COLLECTORS, NDLP_ERR, "SPAWN SERVER: Cannot send ping status report");
}

static void spawn_server_send_status_success(int fd) {
    const struct status_report sr = {
        .status = STATUS_REPORT_STARTED,
        .started = {
            .pid = getpid(),
        },
    };

    if(write(fd, &sr, sizeof(sr)) != sizeof(sr))
        nd_log(NDLS_COLLECTORS, NDLP_ERR, "SPAWN SERVER: Cannot send success status report");
}

static void spawn_server_send_status_failure(int fd) {
    struct status_report sr = {
        .status = STATUS_REPORT_FAILED,
        .failed = {
            .err_no = errno,
        },
    };

    if(write(fd, &sr, sizeof(sr)) != sizeof(sr))
        nd_log(NDLS_COLLECTORS, NDLP_ERR, "SPAWN SERVER: Cannot send failure status report");
}

static void spawn_server_send_status_exit(int fd, int waitpid_status) {
    struct status_report sr = {
        .status = STATUS_REPORT_EXITED,
        .exited = {
            .waitpid_status = waitpid_status,
        },
    };

    if(write(fd, &sr, sizeof(sr)) != sizeof(sr))
        nd_log(NDLS_COLLECTORS, NDLP_ERR, "SPAWN SERVER: Cannot send exit status report");
}

static void spawn_server_run_child(SPAWN_SERVER *server, SPAWN_REQUEST *request) {
    // fprintf(stderr, "CHILD: running request %zu on pid %d\n", request->request_id, getpid());

    // close the server sockets;
    close(server->server_sock); server->server_sock = -1;
    if(server->pipe[0] != -1) { close(server->pipe[0]); server->pipe[0] = -1; }
    if(server->pipe[1] != -1) { close(server->pipe[1]); server->pipe[1] = -1; }

    // set the process name
    {
        char buf[15];
        snprintfz(buf, sizeof(buf), "chld-%zu-r%zu", server->id, request->request_id);
        os_setproctitle(buf, server->argc, server->argv);
    }

    // get the fds from the request
    int stdin_fd = request->fds[0];
    int stdout_fd = request->fds[1];
    int stderr_fd = request->fds[2];
    int custom_fd = request->fds[3];

    // change stdio fds to the ones in the request
    if (dup2(stdin_fd, STDIN_FILENO) == -1) {
        spawn_server_send_status_failure(stdout_fd);
        exit(1);
    }
    if (dup2(stdout_fd, STDOUT_FILENO) == -1) {
        spawn_server_send_status_failure(stdout_fd);
        exit(1);
    }
    if (dup2(stderr_fd, STDERR_FILENO) == -1) {
        spawn_server_send_status_failure(stdout_fd);
        exit(1);
    }

    // close the excess fds
    close(stdin_fd); stdin_fd = request->fds[0] = STDIN_FILENO;
    close(stdout_fd); stdout_fd = request->fds[1] = STDOUT_FILENO;
    close(stderr_fd); stderr_fd = request->fds[2] = STDERR_FILENO;

    // overwrite the process environment
    environ = (char **)request->environment;

    // Perform different actions based on the type
    switch (request->type) {

        case SPAWN_INSTANCE_TYPE_EXEC:
            spawn_server_send_status_success(request->socket);
            close(request->socket); request->socket = -1;
            close(custom_fd); custom_fd = -1;
            execvp(request->argv[0], (char **)request->argv);
            nd_log(NDLS_COLLECTORS, NDLP_ERR,
                "SPAWN SERVER: Failed to execute command of request No %zu (argv[0] = '%s')",
                request->request_id, request->argv[0]);
            exit(1);
            break;

        case SPAWN_INSTANCE_TYPE_CALLBACK:
            if(server->cb == NULL) {
                errno = ENOENT;
                spawn_server_send_status_failure(request->socket);
                close(request->socket); request->socket = -1;
                exit(1);
            }
            spawn_server_send_status_success(request->socket);
            close(request->socket); request->socket = -1;
            server->cb(request);
            exit(0);
            break;

        default:
            nd_log(NDLS_COLLECTORS, NDLP_ERR, "SPAWN SERVER: unknown request type %u", request->type);
            exit(1);
    }
}

// --------------------------------------------------------------------------------------------------------------------
// Encoding and decoding of spawn server request argv type of data

// Function to encode argv or envp
static void* encode_argv(const char **argv, size_t *out_size) {
    size_t buffer_size = 1024; // Initial buffer size
    size_t buffer_used = 0;
    char *buffer = mallocz(buffer_size);

    if(argv) {
        for (const char **p = argv; *p != NULL; p++) {
            if (strlen(*p) == 0)
                continue; // Skip empty strings

            size_t len = strlen(*p) + 1;
            size_t wanted_size = buffer_used + len + 1;

            if (wanted_size >= buffer_size) {
                buffer_size *= 2;

                if(buffer_size < wanted_size)
                    buffer_size = wanted_size;

                buffer = reallocz(buffer, buffer_size);
            }

            memcpy(&buffer[buffer_used], *p, len);
            buffer_used += len;
        }
    }

    buffer[buffer_used++] = '\0'; // Final empty string
    *out_size = buffer_used;

    return buffer;
}

// Function to decode argv or envp
static const char** decode_argv(const char *buffer, size_t size) {
    size_t count = 0;
    const char *ptr = buffer;
    while (ptr < buffer + size) {
        if(ptr && *ptr) {
            count++;
            ptr += strlen(ptr) + 1;
        }
        else
            break;
    }

    const char **argv = mallocz((count + 1) * sizeof(char *));

    ptr = buffer;
    for (size_t i = 0; i < count; i++) {
        argv[i] = ptr;
        ptr += strlen(ptr) + 1;
    }
    argv[count] = NULL; // Null-terminate the array

    return argv;
}

// --------------------------------------------------------------------------------------------------------------------
// Sending and receiving requests

typedef enum __attribute__((packed)) {
    SPAWN_SERVER_MSG_INVALID = 0,
    SPAWN_SERVER_MSG_REQUEST,
    SPAWN_SERVER_MSG_PING,
} SPAWN_SERVER_MSG;

static bool spawn_server_is_running(const char *path) {
    struct msghdr msg = {0};
    struct iovec iov[7];
    SPAWN_SERVER_MSG msg_type = SPAWN_SERVER_MSG_PING;
    size_t dummy_size = 0;
    SPAWN_INSTANCE_TYPE dummy_type = 0;
    ND_UUID magic = UUID_ZERO;
    char cmsgbuf[CMSG_SPACE(sizeof(int))];

    iov[0].iov_base = &msg_type;
    iov[0].iov_len = sizeof(msg_type);

    iov[1].iov_base = magic.uuid;
    iov[1].iov_len = sizeof(magic.uuid);

    iov[2].iov_base = &dummy_size;
    iov[2].iov_len = sizeof(dummy_size);

    iov[3].iov_base = &dummy_size;
    iov[3].iov_len = sizeof(dummy_size);

    iov[4].iov_base = &dummy_size;
    iov[4].iov_len = sizeof(dummy_size);

    iov[5].iov_base = &dummy_size;
    iov[5].iov_len = sizeof(dummy_size);

    iov[6].iov_base = &dummy_type;
    iov[6].iov_len = sizeof(dummy_type);

    msg.msg_iov = iov;
    msg.msg_iovlen = 7;
    msg.msg_control = cmsgbuf;
    msg.msg_controllen = sizeof(cmsgbuf);

    int sock = connect_to_spawn_server(path, false);
    if(sock == -1)
        return false;

    int rc = sendmsg(sock, &msg, 0);
    if (rc < 0) {
        // cannot send the message
        close(sock);
        return false;
    }

    // Receive response
    struct status_report sr = { 0 };
    if (read(sock, &sr, sizeof(sr)) != sizeof(sr)) {
        // cannot receive a ping reply
        close(sock);
        return false;
    }

    close(sock);
    return sr.status == STATUS_REPORT_PING;
}

static bool spawn_server_send_request(ND_UUID *magic, SPAWN_REQUEST *request) {
    bool ret = false;

    size_t env_size = 0;
    void *encoded_env = encode_argv(request->environment, &env_size);
    if (!encoded_env)
        goto cleanup;

    size_t argv_size = 0;
    void *encoded_argv = encode_argv(request->argv, &argv_size);
    if (!encoded_argv)
        goto cleanup;

    struct msghdr msg = {0};
    struct cmsghdr *cmsg;
    SPAWN_SERVER_MSG msg_type = SPAWN_SERVER_MSG_REQUEST;
    char cmsgbuf[CMSG_SPACE(sizeof(int) * SPAWN_SERVER_TRANSFER_FDS)];
    struct iovec iov[11];


    // We send 1 request with 10 iovec in it
    // The request will be received in 2 parts
    // 1. the first 6 iovec which include the sizes of the memory allocations required
    // 2. the last 4 iovec which require the memory allocations to be received

    iov[0].iov_base = &msg_type;
    iov[0].iov_len = sizeof(msg_type);

    iov[1].iov_base = magic->uuid;
    iov[1].iov_len = sizeof(magic->uuid);

    iov[2].iov_base = &request->request_id;
    iov[2].iov_len = sizeof(request->request_id);

    iov[3].iov_base = &env_size;
    iov[3].iov_len = sizeof(env_size);

    iov[4].iov_base = &argv_size;
    iov[4].iov_len = sizeof(argv_size);

    iov[5].iov_base = &request->data_size;
    iov[5].iov_len = sizeof(request->data_size);

    iov[6].iov_base = &request->type;  // Added this line
    iov[6].iov_len = sizeof(request->type);

    iov[7].iov_base = encoded_env;
    iov[7].iov_len = env_size;

    iov[8].iov_base = encoded_argv;
    iov[8].iov_len = argv_size;

    iov[9].iov_base = (char *)request->data;
    iov[9].iov_len = request->data_size;

    iov[10].iov_base = NULL;
    iov[10].iov_len = 0;

    msg.msg_iov = iov;
    msg.msg_iovlen = 11;
    msg.msg_control = cmsgbuf;
    msg.msg_controllen = CMSG_SPACE(sizeof(int) * SPAWN_SERVER_TRANSFER_FDS);

    cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int) * SPAWN_SERVER_TRANSFER_FDS);

    memcpy(CMSG_DATA(cmsg), request->fds, sizeof(int) * SPAWN_SERVER_TRANSFER_FDS);

    int rc = sendmsg(request->socket, &msg, 0);

    if (rc < 0) {
        nd_log(NDLS_COLLECTORS, NDLP_ERR, "SPAWN PARENT: Failed to sendmsg() request to spawn server using socket %d.", request->socket);
        goto cleanup;
    }
    else {
        ret = true;
        // fprintf(stderr, "PARENT: sent request %zu on socket %d (fds: %d, %d, %d, %d) from tid %d\n",
        //     request->request_id, request->socket, request->fds[0], request->fds[1], request->fds[2], request->fds[3], os_gettid());
    }

cleanup:
    freez(encoded_env);
    freez(encoded_argv);
    return ret;
}

static void spawn_server_receive_request(int sock, SPAWN_SERVER *server) {
    struct msghdr msg = {0};
    struct iovec iov[7];
    SPAWN_SERVER_MSG msg_type = SPAWN_SERVER_MSG_INVALID;
    size_t request_id;
    size_t env_size;
    size_t argv_size;
    size_t data_size;
    ND_UUID magic = UUID_ZERO;
    SPAWN_INSTANCE_TYPE type;
    char cmsgbuf[CMSG_SPACE(sizeof(int) * SPAWN_SERVER_TRANSFER_FDS)];
    char *envp = NULL, *argv = NULL, *data = NULL;
    int stdin_fd = -1, stdout_fd = -1, stderr_fd = -1, custom_fd = -1;

    // First recvmsg() to read sizes and control message
    iov[0].iov_base = &msg_type;
    iov[0].iov_len = sizeof(msg_type);

    iov[1].iov_base = magic.uuid;
    iov[1].iov_len = sizeof(magic.uuid);

    iov[2].iov_base = &request_id;
    iov[2].iov_len = sizeof(request_id);

    iov[3].iov_base = &env_size;
    iov[3].iov_len = sizeof(env_size);

    iov[4].iov_base = &argv_size;
    iov[4].iov_len = sizeof(argv_size);

    iov[5].iov_base = &data_size;
    iov[5].iov_len = sizeof(data_size);

    iov[6].iov_base = &type;
    iov[6].iov_len = sizeof(type);

    msg.msg_iov = iov;
    msg.msg_iovlen = 7;
    msg.msg_control = cmsgbuf;
    msg.msg_controllen = sizeof(cmsgbuf);

    if (recvmsg(sock, &msg, 0) < 0) {
        nd_log(NDLS_COLLECTORS, NDLP_ERR,
            "SPAWN SERVER: failed to recvmsg() the first part of the request.");
        close(sock);
        return;
    }

    if(msg_type == SPAWN_SERVER_MSG_PING) {
        spawn_server_send_status_ping(sock);
        close(sock);
        return;
    }

    if(!UUIDeq(magic, server->magic)) {
        nd_log(NDLS_COLLECTORS, NDLP_ERR,
            "SPAWN SERVER: Invalid authorization key for request %zu. "
            "Rejecting request.",
            request_id);
        close(sock);
        return;
    }

    if(type == SPAWN_INSTANCE_TYPE_EXEC && !(server->options & SPAWN_SERVER_OPTION_EXEC)) {
        nd_log(NDLS_COLLECTORS, NDLP_ERR,
            "SPAWN SERVER: Request %zu wants to exec, but exec is not allowed for this spawn server. "
            "Rejecting request.",
            request_id);
        close(sock);
        return;
    }

    if(type == SPAWN_INSTANCE_TYPE_CALLBACK && !(server->options & SPAWN_SERVER_OPTION_CALLBACK)) {
        nd_log(NDLS_COLLECTORS, NDLP_ERR,
            "SPAWN SERVER: Request %zu wants to run a callback, but callbacks are not allowed for this spawn server. "
            "Rejecting request.",
            request_id);
        close(sock);
        return;
    }

    // Extract file descriptors from control message
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    if (cmsg == NULL || cmsg->cmsg_len != CMSG_LEN(sizeof(int) * SPAWN_SERVER_TRANSFER_FDS)) {
        nd_log(NDLS_COLLECTORS, NDLP_ERR,
            "SPAWN SERVER: Received invalid control message (expected %zu bytes, received %zu bytes)",
            CMSG_LEN(sizeof(int) * SPAWN_SERVER_TRANSFER_FDS), cmsg?cmsg->cmsg_len:0);
        close(sock);
        return;
    }

    if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS) {
        nd_log(NDLS_COLLECTORS, NDLP_ERR, "SPAWN SERVER: Received unexpected control message type.");
        close(sock);
        return;
    }

    int *fds = (int *)CMSG_DATA(cmsg);
    stdin_fd = fds[0];
    stdout_fd = fds[1];
    stderr_fd = fds[2];
    custom_fd = fds[3];

    if (stdin_fd < 0 || stdout_fd < 0 || stderr_fd < 0) {
        nd_log(NDLS_COLLECTORS, NDLP_ERR,
            "SPAWN SERVER: invalid file descriptors received, stdin = %d, stdout = %d, stderr = %d",
            stdin_fd, stdout_fd, stderr_fd);
        close(sock);
        goto cleanup;
    }

    // Second recvmsg() to read buffer contents
    iov[0].iov_base = envp = mallocz(env_size);
    iov[0].iov_len = env_size;
    iov[1].iov_base = argv = mallocz(argv_size);
    iov[1].iov_len = argv_size;
    iov[2].iov_base = data = mallocz(data_size);
    iov[2].iov_len = data_size;

    msg.msg_iov = iov;
    msg.msg_iovlen = 3;
    msg.msg_control = NULL;
    msg.msg_controllen = 0;

    ssize_t total_bytes_received = recvmsg(sock, &msg, 0);
    if (total_bytes_received < 0) {
        nd_log(NDLS_COLLECTORS, NDLP_ERR, "SPAWN SERVER: failed to recvmsg() the second part of the request.");
        close(sock);
        goto cleanup;
    }

    // fprintf(stderr, "SPAWN SERVER: received request %zu (fds: %d, %d, %d, %d)\n", request_id,
    //     stdin_fd, stdout_fd, stderr_fd, custom_fd);

    SPAWN_REQUEST *request = mallocz(sizeof(*request));
    *request = (SPAWN_REQUEST){
        .pid = 0,
        .request_id = request_id,
        .socket = sock,
        .fds = {
            [0] = stdin_fd,
            [1] = stdout_fd,
            [2] = stderr_fd,
            [3] = custom_fd,
        },
        .environment = decode_argv(envp, env_size),
        .argv = decode_argv(argv, argv_size),
        .data = data,
        .data_size = data_size,
        .type = type
    };

    pid_t pid = fork();
    if (pid == 0) {
        // the child
        spawn_server_run_child(server, request);
        exit(63);
    }
    else if (pid > 0) {
        // the parent
        request->environment = NULL; // will be free'd at cleanup
        request->argv = NULL; // will be free'd at cleanup
        request->data = NULL; // will be free'd at cleanup
        request->data_size = 0;
        request->pid = pid;
        request->fds[0] = -1;
        request->fds[1] = -1;
        request->fds[2] = -1;
        request->fds[3] = -1;
        DOUBLE_LINKED_LIST_APPEND_ITEM_UNSAFE(spawn_server_requests, request, prev, next);

        // do not fork this socket on other children
        sock_setcloexec(request->socket);
    }
    else {
        nd_log(NDLS_COLLECTORS, NDLP_ERR, "SPAWN SERVER: Failed to fork() child.");
        spawn_server_send_status_failure(stdout_fd);
        // the other allocations (envp, argv, data) will be free'd at cleanup
        freez((void *)request);
        close(sock);
    }

cleanup:
    if(stdin_fd != -1) close(stdin_fd);
    if(stdout_fd != -1) close(stdout_fd);
    if(stderr_fd != -1) close(stderr_fd);
    if(custom_fd != -1) close(custom_fd);
    freez(envp);
    freez(argv);
    freez(data);
}

// --------------------------------------------------------------------------------------------------------------------
// the spawn server main event loop

static void spawn_server_sigchld_handler(int signo __maybe_unused) {
    spawn_server_sigchld = true;
}

static void spawn_server_sigterm_handler(int signo __maybe_unused) {
    spawn_server_exit = true;
}

static SPAWN_REQUEST *find_request_by_pid(pid_t pid) {
    for(SPAWN_REQUEST *rq = spawn_server_requests; rq ;rq = rq->next)
        if(rq->pid == pid)
            return rq;

    return NULL;
}

static void spawn_server_process_sigchld(void) {
    // nd_log(NDLS_COLLECTORS, NDLP_INFO, "SPAWN SERVER: checking for exited children");

    int status;
    pid_t pid;

    // Loop to check for exited child processes
    while ((pid = waitpid((pid_t)(-1), &status, WNOHANG)) != 0) {
        if(pid == -1)
            break;

        SPAWN_REQUEST *rq = find_request_by_pid(pid);
        size_t request_id = rq ? rq->request_id : 0;
        bool send_report_remove_request = false;

        if(WIFEXITED(status)) {
            if(WEXITSTATUS(status))
                nd_log(NDLS_COLLECTORS, NDLP_INFO,
                    "SPAWN SERVER: child with pid %d (request %zu) exited normally with exit code %d",
                    pid, request_id, WEXITSTATUS(status));
            send_report_remove_request = true;
        }
        else if(WIFSIGNALED(status)) {
            if(WCOREDUMP(status))
                nd_log(NDLS_COLLECTORS, NDLP_INFO,
                    "SPAWN SERVER: child with pid %d (request %zu) coredump'd due to signal %d",
                    pid, request_id, WTERMSIG(status));
            else
                nd_log(NDLS_COLLECTORS, NDLP_INFO,
                    "SPAWN SERVER: child with pid %d (request %zu) killed by signal %d",
                    pid, request_id, WTERMSIG(status));
            send_report_remove_request = true;
        }
        else if(WIFSTOPPED(status)) {
            nd_log(NDLS_COLLECTORS, NDLP_INFO,
                "SPAWN SERVER: child with pid %d (request %zu) stopped due to signal %d",
                pid, request_id, WSTOPSIG(status));
            send_report_remove_request = false;
        }
        else if(WIFCONTINUED(status)) {
            nd_log(NDLS_COLLECTORS, NDLP_INFO,
                "SPAWN SERVER: child with pid %d (request %zu) continued due to signal %d",
                pid, request_id, SIGCONT);
            send_report_remove_request = false;
        }
        else {
            nd_log(NDLS_COLLECTORS, NDLP_INFO,
                "SPAWN SERVER: child with pid %d (request %zu) reports unhandled status",
                pid, request_id);
            send_report_remove_request = false;
        }

        if(send_report_remove_request && rq) {
            spawn_server_send_status_exit(rq->socket, status);
            close(rq->socket);
            DOUBLE_LINKED_LIST_REMOVE_ITEM_UNSAFE(spawn_server_requests, rq, prev, next);
            freez(rq);
        }
    }
}

static void signals_unblock(void) {
    sigset_t sigset;
    sigfillset(&sigset);

    if(pthread_sigmask(SIG_UNBLOCK, &sigset, NULL) == -1) {
        netdata_log_error("SIGNAL: Could not unblock signals for threads");
    }
}

static void spawn_server_event_loop(SPAWN_SERVER *server) {
    int pipe_fd = server->pipe[1];
    close(server->pipe[0]); server->pipe[0] = -1;

    signals_unblock();

    // Set up the signal handler for SIGCHLD and SIGTERM
    struct sigaction sa;
    sa.sa_handler = spawn_server_sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    if (sigaction(SIGCHLD, &sa, NULL) == -1) {
        nd_log(NDLS_COLLECTORS, NDLP_ERR, "SPAWN SERVER: sigaction() failed for SIGCHLD");
        exit(1);
    }

    sa.sa_handler = spawn_server_sigterm_handler;
    if (sigaction(SIGTERM, &sa, NULL) == -1) {
        nd_log(NDLS_COLLECTORS, NDLP_ERR, "SPAWN SERVER: sigaction() failed for SIGTERM");
        exit(1);
    }

    struct status_report sr = {
        .status = STATUS_REPORT_STARTED,
        .started = {
            .pid = getpid(),
        },
    };
    if (write(pipe_fd, &sr, sizeof(sr)) != sizeof(sr)) {
        nd_log(NDLS_COLLECTORS, NDLP_ERR, "SPAWN SERVER: failed to write initial status report.");
        exit(1);
    }

    struct pollfd fds[2];
    fds[0].fd = server->server_sock;
    fds[0].events = POLLIN;
    fds[1].fd = pipe_fd;
    fds[1].events = POLLHUP | POLLERR;

    while(!spawn_server_exit) {
        int ret = poll(fds, 2, -1);
        if (spawn_server_sigchld) {
            spawn_server_sigchld = false;
            spawn_server_process_sigchld();

            if(ret == -1)
                continue;
        }

        if (ret == -1) {
            nd_log(NDLS_COLLECTORS, NDLP_ERR, "SPAWN SERVER: poll() failed");
            break;
        }

        if (fds[1].revents & (POLLHUP|POLLERR)) {
            // Pipe has been closed (parent has exited)
            nd_log(NDLS_COLLECTORS, NDLP_DEBUG, "SPAWN SERVER: Parent process has exited");
            break;
        }

        if (fds[0].revents & POLLIN) {
            int client_sock = accept(server->server_sock, NULL, NULL);
            if (client_sock == -1) {
                nd_log(NDLS_COLLECTORS, NDLP_ERR, "SPAWN SERVER: accept() failed");
                continue;
            }

            spawn_server_receive_request(client_sock, server);
        }
    }

    // Cleanup before exiting
    unlink(server->path);

    // stop all children
    if(spawn_server_requests) {
        // nd_log(NDLS_COLLECTORS, NDLP_INFO, "SPAWN SERVER: killing all children...");
        size_t killed = 0;
        for(SPAWN_REQUEST *rq = spawn_server_requests; rq ; rq = rq->next) {
            kill(rq->pid, SIGTERM);
            killed++;
        }
        while(spawn_server_requests) {
            spawn_server_process_sigchld();
            tinysleep();
        }
        // nd_log(NDLS_COLLECTORS, NDLP_INFO, "SPAWN SERVER: all %zu children finished", killed);
    }

    exit(1);
}

// --------------------------------------------------------------------------------------------------------------------
// management of the spawn server

void spawn_server_destroy(SPAWN_SERVER *server) {
    if(server->pipe[0] != -1) close(server->pipe[0]);
    if(server->pipe[1] != -1) close(server->pipe[1]);
    if(server->server_sock != -1) close(server->server_sock);

    if(server->server_pid) {
        kill(server->server_pid, SIGTERM);
        waitpid(server->server_pid, NULL, 0);
    }

    if(server->path) {
        unlink(server->path);
        freez(server->path);
    }

    freez((void *)server->name);
    freez(server);
}

static bool spawn_server_create_listening_socket(SPAWN_SERVER *server) {
    if(spawn_server_is_running(server->path)) {
        nd_log(NDLS_COLLECTORS, NDLP_ERR, "SPAWN SERVER: Server is already listening on path '%s'", server->path);
        return false;
    }

    if ((server->server_sock = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
        nd_log(NDLS_COLLECTORS, NDLP_ERR, "SPAWN SERVER: Failed to create socket()");
        return false;
    }

    struct sockaddr_un server_addr = {
        .sun_family = AF_UNIX,
    };
    strcpy(server_addr.sun_path, server->path);
    unlink(server->path);
    errno = 0;

    if (bind(server->server_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
        nd_log(NDLS_COLLECTORS, NDLP_ERR, "SPAWN SERVER: Failed to bind()");
        return false;
    }

    if (listen(server->server_sock, 5) == -1) {
        nd_log(NDLS_COLLECTORS, NDLP_ERR, "SPAWN SERVER: Failed to listen()");
        return false;
    }

    return true;
}

static void replace_stdio_with_dev_null() {
    int dev_null_fd = open("/dev/null", O_RDWR);
    if (dev_null_fd == -1) {
        nd_log(NDLS_COLLECTORS, NDLP_ERR, "SPAWN SERVER: Failed to open /dev/null: %s", strerror(errno));
        return;
    }

    // Redirect stdin (fd 0)
    if (dup2(dev_null_fd, STDIN_FILENO) == -1) {
        nd_log(NDLS_COLLECTORS, NDLP_ERR, "SPAWN SERVER: Failed to redirect stdin to /dev/null: %s", strerror(errno));
        close(dev_null_fd);
        return;
    }

    // Redirect stdout (fd 1)
    if (dup2(dev_null_fd, STDOUT_FILENO) == -1) {
        nd_log(NDLS_COLLECTORS, NDLP_ERR, "SPAWN SERVER: Failed to redirect stdout to /dev/null: %s", strerror(errno));
        close(dev_null_fd);
        return;
    }

    // Close the original /dev/null file descriptor
    close(dev_null_fd);
}

SPAWN_SERVER* spawn_server_create(SPAWN_SERVER_OPTIONS options, const char *name, spawn_request_callback_t child_callback, int argc, const char **argv) {
    SPAWN_SERVER *server = callocz(1, sizeof(SPAWN_SERVER));
    server->pipe[0] = -1;
    server->pipe[1] = -1;
    server->server_sock = -1;
    server->cb = child_callback;
    server->argc = argc;
    server->argv = argv;
    server->options = options;
    server->id = __atomic_add_fetch(&spawn_server_id, 1, __ATOMIC_RELAXED);
    os_uuid_generate_random(server->magic.uuid);

    char *runtime_directory = getenv("NETDATA_CACHE_DIR");
    if(runtime_directory && !*runtime_directory) runtime_directory = NULL;
    if (runtime_directory) {
        struct stat statbuf;

        if(!*runtime_directory)
            // it is empty
                runtime_directory = NULL;

        else if (stat(runtime_directory, &statbuf) == 0 && S_ISDIR(statbuf.st_mode)) {
            // it exists and it is a directory

            if (access(runtime_directory, W_OK) != 0) {
                // it is not writable by us
                nd_log(NDLS_COLLECTORS, NDLP_ERR, "Runtime directory '%s' is not writable, falling back to '/tmp'", runtime_directory);
                runtime_directory = NULL;
            }
        }
        else {
            // it does not exist
            nd_log(NDLS_COLLECTORS, NDLP_ERR, "Runtime directory '%s' does not exist, falling back to '/tmp'", runtime_directory);
            runtime_directory = NULL;
        }
    }
    if(!runtime_directory)
        runtime_directory = "/tmp";

    char path[1024];
    if(name && *name) {
        server->name = strdupz(name);
        snprintf(path, sizeof(path), "%s/.netdata-spawn-%s.sock", runtime_directory, name);
    }
    else {
        snprintfz(path, sizeof(path), "%d-%zu", getpid(), server->id);
        server->name = strdupz(path);
        snprintf(path, sizeof(path), "%s/.netdata-spawn-%d-%zu.sock", runtime_directory, getpid(), server->id);
    }

    server->path = strdupz(path);

    if (!spawn_server_create_listening_socket(server))
        goto cleanup;

    if (pipe(server->pipe) == -1) {
        nd_log(NDLS_COLLECTORS, NDLP_ERR, "SPAWN SERVER: Cannot create status pipe()");
        goto cleanup;
    }

    pid_t pid = fork();
    if (pid == 0) {
        // the child - the spawn server
        {
            char buf[15];
            snprintfz(buf, sizeof(buf), "spawn-%s", server->name);
            os_setproctitle(buf, server->argc, server->argv);
        }

        replace_stdio_with_dev_null();
        os_close_all_non_std_open_fds_except((int[]){ server->server_sock, server->pipe[1] }, 2);
        spawn_server_event_loop(server);
    }
    else if (pid > 0) {
        // the parent
        server->server_pid = pid;
        close(server->server_sock); server->server_sock = -1;
        close(server->pipe[1]); server->pipe[1] = -1;

        struct status_report sr = { 0 };
        if (read(server->pipe[0], &sr, sizeof(sr)) != sizeof(sr)) {
            nd_log(NDLS_COLLECTORS, NDLP_ERR, "SPAWN SERVER: cannot read() initial status report from spawn server");
            goto cleanup;
        }

        if(sr.status != STATUS_REPORT_STARTED) {
            nd_log(NDLS_COLLECTORS, NDLP_ERR, "SPAWN SERVER: server did not respond with success.");
            goto cleanup;
        }

        if(sr.started.pid != server->server_pid) {
            nd_log(NDLS_COLLECTORS, NDLP_ERR, "SPAWN SERVER: server sent pid %d but we have created %d.", sr.started.pid, server->server_pid);
            goto cleanup;
        }

        return server;
    }

    nd_log(NDLS_COLLECTORS, NDLP_ERR, "SPAWN SERVER: Cannot fork()");

cleanup:
    spawn_server_destroy(server);
    return NULL;
}

// --------------------------------------------------------------------------------------------------------------------
// creating spawn server instances

void spawn_server_exec_destroy(SPAWN_INSTANCE *instance) {
    if(instance->child_pid) kill(instance->child_pid, SIGTERM);
    if(instance->write_fd != -1) close(instance->write_fd);
    if(instance->read_fd != -1) close(instance->read_fd);
    if(instance->client_sock != -1) close(instance->client_sock);
    freez(instance);
}

int spawn_server_exec_wait(SPAWN_SERVER *server __maybe_unused, SPAWN_INSTANCE *instance) {
    int rc = -1;

    // close the child pipes, to make it exit
    if(instance->write_fd != -1) { close(instance->write_fd); instance->write_fd = -1; }
    if(instance->read_fd != -1) { close(instance->read_fd); instance->read_fd = -1; }

    // get the result
    struct status_report sr = { 0 };
    if(read(instance->client_sock, &sr, sizeof(sr)) != sizeof(sr))
        nd_log(NDLS_COLLECTORS, NDLP_ERR, "SPAWN PARENT: failed to receive final status report for child %d, request %zu", instance->child_pid, instance->request_id);

    else switch(sr.status) {
        case STATUS_REPORT_EXITED:
            rc = sr.exited.waitpid_status;
            break;

        case STATUS_REPORT_STARTED:
        case STATUS_REPORT_FAILED:
        default:
            errno = 0;
            nd_log(NDLS_COLLECTORS, NDLP_ERR, "SPAWN PARENT: invalid status report to exec spawn request %zu for pid %d (status = %u)", instance->request_id, instance->child_pid, sr.status);
            break;
    }

    instance->child_pid = 0;
    spawn_server_exec_destroy(instance);
    return rc;
}

int spawn_server_exec_kill(SPAWN_SERVER *server, SPAWN_INSTANCE *instance) {
    // kill the child, if it is still running
    if(instance->child_pid) kill(instance->child_pid, SIGTERM);
    return spawn_server_exec_wait(server, instance);
}

SPAWN_INSTANCE* spawn_server_exec(SPAWN_SERVER *server, int stderr_fd, int custom_fd, const char **argv, const void *data, size_t data_size, SPAWN_INSTANCE_TYPE type) {
    int pipe_stdin[2] = { -1, -1 }, pipe_stdout[2] = { -1, -1 };

    SPAWN_INSTANCE *instance = callocz(1, sizeof(SPAWN_INSTANCE));
    instance->read_fd = -1;
    instance->write_fd = -1;

    instance->client_sock = connect_to_spawn_server(server->path, true);
    if(instance->client_sock == -1)
        goto cleanup;

    if (pipe(pipe_stdin) == -1) {
        nd_log(NDLS_COLLECTORS, NDLP_ERR, "SPAWN PARENT: Cannot create stdin pipe()");
        goto cleanup;
    }

    if (pipe(pipe_stdout) == -1) {
        nd_log(NDLS_COLLECTORS, NDLP_ERR, "SPAWN PARENT: Cannot create stdout pipe()");
        goto cleanup;
    }

    SPAWN_REQUEST request = {
        .request_id = __atomic_add_fetch(&server->request_id, 1, __ATOMIC_RELAXED),
        .socket = instance->client_sock,
        .fds = {
            [0] = pipe_stdin[0],
            [1] = pipe_stdout[1],
            [2] = stderr_fd,
            [3] = custom_fd,
        },
        .environment = (const char **)environ,
        .argv = argv,
        .data = data,
        .data_size = data_size,
        .type = type
    };

    if(!spawn_server_send_request(&server->magic, &request))
        goto cleanup;

    close(pipe_stdin[0]); pipe_stdin[0] = -1;
    instance->write_fd = pipe_stdin[1]; pipe_stdin[1] = -1;

    close(pipe_stdout[1]); pipe_stdout[1] = -1;
    instance->read_fd = pipe_stdout[0]; pipe_stdout[0] = -1;

    struct status_report sr = { 0 };
    if(read(instance->client_sock, &sr, sizeof(sr)) != sizeof(sr)) {
        nd_log(NDLS_COLLECTORS, NDLP_ERR, "SPAWN PARENT: Failed to exec spawn request %zu (cannot get initial status report)", request.request_id);
        goto cleanup;
    }

    switch(sr.status) {
        case STATUS_REPORT_STARTED:
            instance->child_pid = sr.started.pid;
            return instance;

        case STATUS_REPORT_FAILED:
            errno = sr.failed.err_no;
            nd_log(NDLS_COLLECTORS, NDLP_ERR, "SPAWN PARENT: Failed to exec spawn request %zu (check errno #1)", request.request_id);
            errno = 0;
            break;

        case STATUS_REPORT_EXITED:
            errno = ENOEXEC;
            nd_log(NDLS_COLLECTORS, NDLP_ERR, "SPAWN PARENT: Failed to exec spawn request %zu (check errno #2)", request.request_id);
            errno = 0;
            break;

        default:
            errno = 0;
            nd_log(NDLS_COLLECTORS, NDLP_ERR, "SPAWN PARENT: Invalid status report to exec spawn request %zu (received invalid data)", request.request_id);
            break;
    }

cleanup:
    if (pipe_stdin[0] >= 0) close(pipe_stdin[0]);
    if (pipe_stdin[1] >= 0) close(pipe_stdin[1]);
    if (pipe_stdout[0] >= 0) close(pipe_stdout[0]);
    if (pipe_stdout[1] >= 0) close(pipe_stdout[1]);
    spawn_server_exec_destroy(instance);
    return NULL;
}

#endif // !OS_WINDOWS
