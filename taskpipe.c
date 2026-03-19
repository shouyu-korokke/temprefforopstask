#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/*
 * This program implements a multi-process communication system where:
 * - n child processes communicate with the parent via one shared pipe R
 * - Parent communicates with children via n dedicated pipes P1 to Pn
 * - On SIGINT (Ctrl+C), parent sends a random lowercase letter to a random child
 * - Children receiving a char send a buffer of that char (size 1-200) via pipe R
 * - Children terminate with 20% probability on SIGINT
 * - Parent prints all data from R and terminates when all children are gone
 * - Broken pipes don't terminate the program; parent stops using that pipe
 * - Buffer size is limited to PIPE_BUF considerations for atomic writes
 */

#define ERR(source) \
    (fprintf(stderr, "%s:%d\n", __FILE__, __LINE__), perror(source), kill(0, SIGKILL), exit(EXIT_FAILURE))

// MAX_BUFF must be in one byte range
#define MAX_BUFF 200

volatile sig_atomic_t last_signal = 0;

// Helper function to set signal handlers safely
int sethandler(void (*f)(int), int sigNo)
{
    struct sigaction act;
    memset(&act, 0, sizeof(struct sigaction));
    act.sa_handler = f;
    if (-1 == sigaction(sigNo, &act, NULL))
        return -1;
    return 0;
}

// Signal handler for parent: records the signal received
void sig_handler(int sig) { last_signal = sig; }

// Signal handler for children: terminates with 20% probability on SIGINT
void sig_killme(int sig)
{
    if (rand() % 5 == 0)
        exit(EXIT_SUCCESS);
}

// SIGCHLD handler: reaps zombie child processes
void sigchld_handler(int sig)
{
    pid_t pid;
    for (;;)
    {
        pid = waitpid(0, NULL, WNOHANG);
        if (0 == pid)
            return;
        if (0 >= pid)
        {
            if (ECHILD == errno)
                return;
            ERR("waitpid:");
        }
    }
}

// Child process main work loop:
// - Reads a character from its dedicated pipe (fd)
// - Generates a random buffer size (1-200) filled with that character
// - Writes the buffer to the shared pipe R
// - Loops indefinitely until terminated
void child_work(int fd, int R)
{
    char c, buf[MAX_BUFF + 1];
    unsigned char s;
    srand(getpid());  // Seed random with PID for uniqueness
    if (sethandler(sig_killme, SIGINT))
        ERR("Setting SIGINT handler in child");
    for (;;)
    {
        if (TEMP_FAILURE_RETRY(read(fd, &c, 1)) < 1)
            ERR("read");
        s = 1 + rand() % MAX_BUFF;  // Random size 1-200
        buf[0] = s;  // First byte is the size
        memset(buf + 1, c, s);  // Fill buffer with the character
        if (TEMP_FAILURE_RETRY(write(R, buf, s + 1)) < 0)
            ERR("write to R");
    }
}

// Parent process main work loop:
// - On SIGINT: selects random child pipe, sends random [a-z] char
// - Reads from shared pipe R: first byte is size, then data
// - Prints the received buffer
// - Terminates when R is closed (all children gone)
void parent_work(int n, int *fds, int R)
{
    unsigned char c;
    char buf[MAX_BUFF];
    int status, i;
    srand(getpid());  // Seed random for parent
    if (sethandler(sig_handler, SIGINT))
        ERR("Setting SIGINT handler in parent");
    for (;;)
    {
        if (SIGINT == last_signal)
        {
            i = rand() % n;  // Select random child
            while (0 == fds[i % n] && i < 2 * n)  // Skip closed pipes
                i++;
            i %= n;
            if (fds[i])
            {
                c = 'a' + rand() % ('z' - 'a');  // Random lowercase letter
                status = TEMP_FAILURE_RETRY(write(fds[i], &c, 1));
                if (status != 1)
                {
                    if (TEMP_FAILURE_RETRY(close(fds[i])))
                        ERR("close");
                    fds[i] = 0;  // Mark pipe as broken
                }
            }
            last_signal = 0;
        }
        status = read(R, &c, 1);
        if (status < 0 && errno == EINTR)
            continue;  // Interrupted by signal, retry
        if (status < 0)
            ERR("read header from R");
        if (0 == status)
            break;  // Pipe closed, all children gone
        if (TEMP_FAILURE_RETRY(read(R, buf, c)) < c)
            ERR("read data from R");
        buf[(int)c] = 0;  // Null-terminate for printing
        printf("\n%s\n", buf);
    }
}

// Creates n child processes and their dedicated pipes:
// - For each child: creates pipe, forks, parent keeps write end, child keeps read end
// - Children close unused pipe ends and start child_work
// - Parent keeps all write ends in fds array
void create_children_and_pipes(int n, int *fds, int R)
{
    int tmpfd[2];
    int max = n;
    while (n)
    {
        if (pipe(tmpfd))
            ERR("pipe");
        switch (fork())
        {
            case 0:  // Child process
                while (n < max)  // Close pipes of already created children
                    if (fds[n] && TEMP_FAILURE_RETRY(close(fds[n++])))
                        ERR("close");
                free(fds);
                if (TEMP_FAILURE_RETRY(close(tmpfd[1])))  // Close write end
                    ERR("close");
                child_work(tmpfd[0], R);  // Start child work
                if (TEMP_FAILURE_RETRY(close(tmpfd[0])))
                    ERR("close");
                if (TEMP_FAILURE_RETRY(close(R)))
                    ERR("close");
                exit(EXIT_SUCCESS);

            case -1:
                ERR("Fork:");
        }
        if (TEMP_FAILURE_RETRY(close(tmpfd[0])))  // Parent closes read end
            ERR("close");
        fds[--n] = tmpfd[1];  // Store write end for this child
    }
}

// Prints usage information and exits
void usage(char *name)
{
    fprintf(stderr, "USAGE: %s n\n", name);
    fprintf(stderr, "0<n<=10 - number of children\n");
    exit(EXIT_FAILURE);
}

// Main function: sets up the program
// - Parses command line for n (1-10)
// - Sets up signal handlers (ignore SIGINT/SIGPIPE initially, handle SIGCHLD)
// - Creates shared pipe R
// - Allocates array for dedicated pipes
// - Creates children and pipes
// - Starts parent work loop
// - Cleans up after termination
int main(int argc, char **argv)
{
    int n, *fds, R[2];
    if (2 != argc)
        usage(argv[0]);
    n = atoi(argv[1]);
    if (n <= 0 || n > 10)
        usage(argv[0]);
    // Initially ignore SIGINT and SIGPIPE to prevent interruption during setup
    if (sethandler(SIG_IGN, SIGINT))
        ERR("Setting SIGINT handler");
    if (sethandler(SIG_IGN, SIGPIPE))
        ERR("Setting SIGINT handler");
    // Handle SIGCHLD to reap children
    if (sethandler(sigchld_handler, SIGCHLD))
        ERR("Setting parent SIGCHLD:");
    // Create shared pipe R: R[0] read, R[1] write
    if (pipe(R))
        ERR("pipe");
    // Allocate array for dedicated pipe write ends
    if (NULL == (fds = (int *)malloc(sizeof(int) * n)))
        ERR("malloc");
    // Create children and their pipes
    create_children_and_pipes(n, fds, R[1]);
    // Parent closes write end of R
    if (TEMP_FAILURE_RETRY(close(R[1])))
        ERR("close");
    // Start parent work loop
    parent_work(n, fds, R[0]);
    // Clean up: close remaining pipes
    while (n--)
        if (fds[n] && TEMP_FAILURE_RETRY(close(fds[n])))
            ERR("close");
    if (TEMP_FAILURE_RETRY(close(R[0])))
        ERR("close");
    free(fds);
    return EXIT_SUCCESS;
}