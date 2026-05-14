#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#define MAX_EVENTS  64
#define BUF_SIZE    1024

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        exit(1);
    }
    int port = atoi(argv[1]);

    int listen_fd, epoll_fd;
    struct epoll_event ev, events[MAX_EVENTS];
    struct sockaddr_in addr;

    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd == -1) { perror("socket"); exit(1); }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        perror("bind"); exit(1);
    }

    if (listen(listen_fd, SOMAXCONN) == -1) {
        perror("listen"); exit(1);
    }
    set_nonblocking(listen_fd);
    printf("Listening on port %d...\n", port);

    epoll_fd = epoll_create1(0);
    if (epoll_fd == -1) { perror("epoll_create1"); exit(1); }

    ev.events  = EPOLLIN;
    ev.data.fd = listen_fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &ev) == -1) {
        perror("epoll_ctl: listen_fd"); exit(1);
    }

    for (;;) {
        int n = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if (n == -1) {
            if (errno == EINTR) continue;
            perror("epoll_wait"); exit(1);
        }

        for (int i = 0; i < n; i++) {
            if (events[i].data.fd == listen_fd) {
                // Accept new connections
                for (;;) {
                    struct sockaddr_in client_addr;
                    socklen_t client_len = sizeof(client_addr);
                    int client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);
                    if (client_fd == -1) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        perror("accept"); break;
                    }
                    set_nonblocking(client_fd);
                    ev.events  = EPOLLIN | EPOLLET;
                    ev.data.fd = client_fd;
                    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &ev) == -1) {
                        perror("epoll_ctl: client_fd"); close(client_fd); break;
                    }
                    char welcome_msg[] = "Welcome, elector!\n";
                    send(client_fd, welcome_msg, strlen(welcome_msg), 0);
                }
            } else {
                // Handle errors / hang-up
                if (events[i].events & (EPOLLERR | EPOLLHUP)) {
                    fprintf(stderr, "epoll error on fd=%d\n", events[i].data.fd);
                    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, events[i].data.fd, NULL);
                    close(events[i].data.fd);
                    continue;
                }

                // Read and print incoming data
                for (;;) {
                    char buf[BUF_SIZE];
                    ssize_t count = read(events[i].data.fd, buf, sizeof(buf) - 1);
                    if (count == -1) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        perror("read"); break;
                    } else if (count == 0) {
                        printf("Client disconnected: fd=%d\n", events[i].data.fd);
                        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, events[i].data.fd, NULL);
                        close(events[i].data.fd);
                        break;
                    } else {
                        printf("Received from fd=%d: %.*s\n", events[i].data.fd, (int)count, buf);
                    }
                }
            }
        }
    }

    return 0;
}
