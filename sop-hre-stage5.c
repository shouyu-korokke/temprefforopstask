/*
 * sop-hre-stage5.c
 * Holy Roman Empire Imperial Election Server
 *
 * OVERVIEW OF TECHNIQUES USED:
 * ─────────────────────────────────────────────────────────────────────────────
 * - TCP server with epoll (edge-triggered) for multiplexed I/O
 * - signalfd: converts SIGINT into a readable fd, handled inside the epoll loop
 *   cleanly without async signal handler complications
 * - epoll_wait timeout: used for the 1-minute server deadline
 * - Per-connection state tracked in an array of ElectorInfo structs
 * - Graceful shutdown: close all connections, tally and print votes
 *
 * BUILD:
 *   gcc -O2 -Wall -o sop-hre sop-hre-stage5.c
 *
 * USAGE:
 *   ./sop-hre <port>
 *
 * CLIENTS:
 *   nc localhost <port>
 *   First send a digit 1-7 to identify as an elector.
 *   Then send digits 1-3 to cast/change your vote.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/signalfd.h>
#include <arpa/inet.h>

/* ─── Constants ─────────────────────────────────────────────────────────── */

#define MAX_EVENTS      64
#define BUF_SIZE        1024
#define NUM_ELECTORS    7
#define NUM_CANDIDATES  3
#define TIMEOUT_MS      60000   /* 1 minute in milliseconds */

/* ─── Per-connection state ───────────────────────────────────────────────── */

/*
 * One entry per elector slot (indexed 0–6, corresponding to elector IDs 1–7).
 * socket_fd == -1 means this elector is not currently connected.
 * vote == 0 means no vote cast yet.
 */
typedef struct {
    int socket_fd;
    int vote;       /* 0 = no vote, 1–3 = candidate number */
} ElectorInfo;

/* ─── Helper: make a fd non-blocking ────────────────────────────────────── */

/*
 * Required for edge-triggered epoll (EPOLLET).
 * In ET mode epoll fires once per state change, so you must drain the fd
 * completely in a loop. A blocking fd would stall the entire event loop
 * if you overshoot and call read() with nothing left.
 */
static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* ─── Elector lookup helpers ─────────────────────────────────────────────── */

/* Returns 1 if fd belongs to an identified elector, 0 otherwise. */
int elector_is_onlist(ElectorInfo *electors, int fd) {
    for (int i = 0; i < NUM_ELECTORS; i++)
        if (electors[i].socket_fd == fd)
            return 1;
    return 0;
}

/*
 * Returns the slot index (0–6) for the given fd, or -1 if not found.
 * Used for reverse lookup when a client disconnects or errors out,
 * so we can clear their slot and allow reconnection.
 */
int find_elector_by_fd(ElectorInfo *electors, int fd) {
    for (int i = 0; i < NUM_ELECTORS; i++)
        if (electors[i].socket_fd == fd)
            return i;
    return -1;
}

/* ─── Cleanup helper ─────────────────────────────────────────────────────── */

/*
 * Called on both timeout and SIGINT.
 * Closes all active elector connections, then the listening socket
 * and epoll instance.
 */
static void close_all(ElectorInfo *electors, int listen_fd, int epoll_fd, int sig_fd) {
    for (int i = 0; i < NUM_ELECTORS; i++) {
        if (electors[i].socket_fd != -1) {
            close(electors[i].socket_fd);
            electors[i].socket_fd = -1;
        }
    }
    close(listen_fd);
    close(epoll_fd);
    close(sig_fd);
}

/* ─── Vote tally and result printer ─────────────────────────────────────── */

static void print_results(ElectorInfo *electors) {
    const char *candidates[] = {"Francis I", "Charles V", "Henry VIII"};
    int counts[NUM_CANDIDATES + 1];  /* index 0 = no vote, 1–3 = candidates */
    memset(counts, 0, sizeof(counts));

    for (int i = 0; i < NUM_ELECTORS; i++)
        counts[electors[i].vote]++;

    printf("\n═══════════════════════════════════\n");
    printf("       IMPERIAL ELECTION RESULTS   \n");
    printf("═══════════════════════════════════\n");
    for (int c = 1; c <= NUM_CANDIDATES; c++)
        printf("  %s: %d vote(s)\n", candidates[c - 1], counts[c]);
    printf("  No vote cast: %d elector(s)\n", counts[0]);
    printf("═══════════════════════════════════\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * MAIN
 * ═══════════════════════════════════════════════════════════════════════════ */

int main(int argc, char **argv) {

    /* ── Step 1: Parse arguments ─────────────────────────────────────────── */

    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        exit(1);
    }
    int port = atoi(argv[1]);

    const char *states[] = {
        "Mainz", "Trier", "Cologne", "Bohemia",
        "Palatinate", "Saxony", "Brandenburg"
    };

    /* ── Step 2: Declare variables ───────────────────────────────────────── */

    int listen_fd, epoll_fd, sig_fd;
    struct epoll_event ev, events[MAX_EVENTS];
    struct sockaddr_in addr;

    ElectorInfo electors[NUM_ELECTORS];
    for (int i = 0; i < NUM_ELECTORS; i++) {
        electors[i].socket_fd = -1;
        electors[i].vote = 0;
    }

    /* ── Step 3: Create the listening TCP socket ─────────────────────────── */

    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd == -1) { perror("socket"); exit(1); }

    /* ── Step 4: Allow address reuse ─────────────────────────────────────── */
    /*
     * Without SO_REUSEADDR, restarting the server within ~60s of shutdown
     * gives "bind: Address already in use" because the OS keeps the port in
     * TIME_WAIT state to catch stray late-arriving packets.
     */
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    /* ── Step 5: Bind ────────────────────────────────────────────────────── */

    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;  /* accept on all interfaces */
    addr.sin_port        = htons(port); /* htons: host→network byte order */

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        perror("bind"); exit(1);
    }

    /* ── Step 6: Listen and set non-blocking ─────────────────────────────── */

    if (listen(listen_fd, SOMAXCONN) == -1) {
        perror("listen"); exit(1);
    }
    set_nonblocking(listen_fd);
    printf("Listening on port %d... (timeout: %d seconds)\n",
           port, TIMEOUT_MS / 1000);

    /* ── Step 7: Set up signalfd for SIGINT ──────────────────────────────── */
    /*
     * signalfd converts a signal into a readable file descriptor.
     * This lets SIGINT participate in the epoll event loop just like
     * any socket, avoiding the pitfalls of async signal handlers
     * (which can interrupt syscalls and corrupt state unpredictably).
     *
     * Steps:
     *   a) Block the signal from normal delivery with sigprocmask — otherwise
     *      the process would be killed by SIGINT before epoll sees it.
     *   b) Create a signalfd watching that signal mask.
     *   c) Register the signalfd with epoll.
     *
     * When SIGINT arrives, epoll fires on sig_fd. You read a
     * struct signalfd_siginfo from it to confirm, then handle cleanly.
     */
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    if (sigprocmask(SIG_BLOCK, &mask, NULL) == -1) {
        perror("sigprocmask"); exit(1);
    }
    sig_fd = signalfd(-1, &mask, SFD_NONBLOCK);
    if (sig_fd == -1) { perror("signalfd"); exit(1); }

    /* ── Step 8 (a): Create epoll instance ───────────────────────────────── */
    /*
     * epoll_create1(0): creates the epoll instance.
     * The kernel maintains a watch list (red-black tree) of fds to monitor.
     * epoll_wait() blocks until one or more become ready, then fills
     * your events[] array with the ready ones.
     */
    epoll_fd = epoll_create1(0);
    if (epoll_fd == -1) { perror("epoll_create1"); exit(1); }

    /* ── Step 8 (b): Register fds with epoll ─────────────────────────────── */
    /*
     * We register three fds:
     *   listen_fd — level-triggered (LT): fires while connections are pending
     *   sig_fd    — level-triggered (LT): fires while signal info is readable
     * Client fds will be registered as edge-triggered (ET) after accept().
     *
     * epoll_ctl operations:
     *   EPOLL_CTL_ADD — add fd to watch list
     *   EPOLL_CTL_MOD — modify existing entry
     *   EPOLL_CTL_DEL — remove from watch list (do before close())
     */
    ev.events  = EPOLLIN;
    ev.data.fd = listen_fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &ev) == -1) {
        perror("epoll_ctl: listen_fd"); exit(1);
    }

    ev.events  = EPOLLIN;
    ev.data.fd = sig_fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, sig_fd, &ev) == -1) {
        perror("epoll_ctl: sig_fd"); exit(1);
    }

    /* ── Step 9: Record start time for timeout ───────────────────────────── */
    /*
     * We compute the remaining time before each epoll_wait call.
     * epoll_wait's last argument is a timeout in milliseconds:
     *   -1  = block forever
     *    0  = return immediately
     *   >0  = wait at most N milliseconds
     * When it returns 0 (timed out), we break the event loop.
     */
    struct timespec start_ts;
    clock_gettime(CLOCK_MONOTONIC, &start_ts);

    /* ── Step 10: Event loop ─────────────────────────────────────────────── */

    int running = 1;
    while (running) {

        /* Compute remaining time until deadline */
        struct timespec now_ts;
        clock_gettime(CLOCK_MONOTONIC, &now_ts);
        long elapsed_ms = (now_ts.tv_sec  - start_ts.tv_sec)  * 1000
                        + (now_ts.tv_nsec - start_ts.tv_nsec) / 1000000;
        int remaining_ms = TIMEOUT_MS - (int)elapsed_ms;
        if (remaining_ms <= 0) {
            printf("\nTimeout reached. Shutting down.\n");
            break;
        }

        /* ── Step 10a: Wait for events ───────────────────────────────────── */
        int n = epoll_wait(epoll_fd, events, MAX_EVENTS, remaining_ms);
        if (n == -1) {
            if (errno == EINTR) continue;   /* interrupted by unblocked signal */
            perror("epoll_wait"); break;
        }
        if (n == 0) {
            /* epoll_wait timed out — deadline reached */
            printf("\nTimeout reached. Shutting down.\n");
            break;
        }

        /* ── Step 11: Iterate ready events ──────────────────────────────── */
        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;

            /* ── Step 12a: SIGINT received ───────────────────────────────── */
            if (fd == sig_fd) {
                /*
                 * Read the signalfd_siginfo struct to consume the signal.
                 * If you don't read it, epoll will keep firing on sig_fd.
                 */
                struct signalfd_siginfo sig_info;
                read(sig_fd, &sig_info, sizeof(sig_info));
                printf("\nSIGINT received. Shutting down.\n");
                running = 0;
                break;  /* break inner for(i) loop; while(running) exits too */
            }

            /* ── Step 12b: New TCP connection ────────────────────────────── */
            else if (fd == listen_fd) {
                /*
                 * Drain the accept backlog in a loop.
                 * In ET mode on listen_fd (we use LT here, but the inner loop
                 * is good practice): multiple clients may have connected
                 * between epoll_wait calls. Accept all of them now.
                 */
                for (;;) {
                    struct sockaddr_in client_addr;
                    socklen_t client_len = sizeof(client_addr);
                    int client_fd = accept(listen_fd,
                                          (struct sockaddr *)&client_addr,
                                          &client_len);
                    if (client_fd == -1) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        perror("accept"); break;
                    }
                    set_nonblocking(client_fd);

                    /*
                     * Register new client as edge-triggered (EPOLLET).
                     * ET fires once per state change, so the read loop
                     * must drain until EAGAIN on every notification.
                     */
                    ev.events  = EPOLLIN | EPOLLET;
                    ev.data.fd = client_fd;
                    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &ev) == -1) {
                        perror("epoll_ctl: client_fd");
                        close(client_fd);
                        break;
                    }
                    /* Client is now connected but unidentified — no welcome yet */
                }
            }

            /* ── Step 12c: Error or hang-up on a client fd ───────────────── */
            else if (events[i].events & (EPOLLERR | EPOLLHUP)) {
                /*
                 * EPOLLERR: socket-level error (e.g. TCP RST received).
                 * EPOLLHUP: peer closed the connection.
                 * Both are implicitly monitored — no need to add them to ev.events.
                 * Always DEL before close() to avoid a race where a new fd
                 * reuses the same number and gets watched by accident.
                 */
                printf("Client disconnected (error/hup): fd=%d\n", fd);
                int slot = find_elector_by_fd(electors, fd);
                if (slot != -1) {
                    electors[slot].socket_fd = -1;
                    electors[slot].vote = 0;
                }
                epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL);
                close(fd);
            }

            /* ── Step 12d: Data on a client fd ───────────────────────────── */
            else if (events[i].events & EPOLLIN) {

                if (!elector_is_onlist(electors, fd)) {

                    /* ── Unidentified client: expect elector number 1–7 ──── */
                    /*
                     * Read just one byte for identification.
                     * count >= 1: got at least the ID digit (netcat sends \n too,
                     *   but we only look at the first byte).
                     * count == 0: client disconnected before identifying.
                     * count == -1: error.
                     */
                    char idcode;
                    ssize_t count = read(fd, &idcode, sizeof(idcode));

                    if (count >= 1) {
                        int id = idcode - '0';
                        if (id < 1 || id > NUM_ELECTORS) {
                            fprintf(stderr, "Invalid elector ID from fd=%d: %c\n", fd, idcode);
                            epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL);
                            close(fd);
                        } else if (electors[id - 1].socket_fd != -1) {
                            /* Duplicate elector — reject newcomer */
                            fprintf(stderr, "Elector %d already connected, rejecting fd=%d\n", id, fd);
                            epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL);
                            close(fd);
                        } else {
                            /* Valid new elector — register and welcome */
                            electors[id - 1].socket_fd = fd;
                            printf("Elector %d (%s) connected: fd=%d\n",
                                   id, states[id - 1], fd);
                            char welcome[64];
                            snprintf(welcome, sizeof(welcome),
                                     "Welcome, elector of %s!\n", states[id - 1]);
                            send(fd, welcome, strlen(welcome), 0);
                        }
                    } else {
                        /* Disconnected or error before identifying */
                        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL);
                        close(fd);
                    }

                } else {

                    /* ── Identified elector: handle votes ────────────────── */
                    /*
                     * ET mode requires draining the read buffer completely.
                     * Loop until read() returns -1 with EAGAIN/EWOULDBLOCK,
                     * meaning the kernel buffer is empty.
                     * Each byte is checked: '1'–'3' updates the vote,
                     * everything else (including '\n' from netcat) is ignored.
                     */
                    int slot = find_elector_by_fd(electors, fd);

                    for (;;) {
                        char buf[BUF_SIZE];
                        ssize_t count = read(fd, buf, sizeof(buf));

                        if (count == -1) {
                            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                            perror("read");
                            /* Fall through to disconnect handling */
                            count = 0;
                        }

                        if (count == 0) {
                            /* Orderly disconnect */
                            printf("Elector %d disconnected: fd=%d\n", slot + 1, fd);
                            if (slot != -1) {
                                electors[slot].socket_fd = -1;
                                electors[slot].vote = 0;
                            }
                            epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL);
                            close(fd);
                            break;
                        }

                        /* Process each byte for a valid vote character */
                        for (ssize_t j = 0; j < count; j++) {
                            char ch = buf[j];
                            if (ch >= '1' && ch <= '3') {
                                int vote = ch - '0';
                                if (slot != -1) {
                                    electors[slot].vote = vote;
                                    printf("Elector %d (%s) voted for candidate %d\n",
                                           slot + 1, states[slot], vote);
                                }
                            }
                            /* Other characters silently ignored per spec */
                        }
                    }
                }
            }
        } /* end for(i) over events */
    } /* end while(running) */

    /* ── Step 13: Graceful shutdown ─────────────────────────────────────── */
    /*
     * Close all open elector connections, the listening socket,
     * epoll instance, and signalfd.
     * Then tally and print the final vote counts.
     */
    close_all(electors, listen_fd, epoll_fd, sig_fd);
    print_results(electors);

    return 0;
}
