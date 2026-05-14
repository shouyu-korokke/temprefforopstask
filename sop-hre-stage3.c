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

typedef struct {
    int socket_fd;
    int vote;         // 0 = no vote, 1-3
} ElectorInfo;

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int elector_is_onlist(ElectorInfo *electorinfos, int fd) {
    for (int i = 0; i < 7; i++) {
        if (electorinfos[i].socket_fd == fd)
            return 1;
    }
    return 0;
}

int find_elector_by_fd(ElectorInfo *electorinfos, int fd) {
    for (int i = 0; i < 7; i++) {
        if (electorinfos[i].socket_fd == fd)
            return i;
    }
    return -1;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        exit(1);
    }
    int port = atoi(argv[1]);
    const char *states[] = {"Mainz", "Trier", "Cologne", "Bohemia", "Palatinate", "Saxony", "Brandenburg"};

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

    ElectorInfo electorinfos[7];
    for (int i = 0; i < 7; i++) {
        electorinfos[i].socket_fd = -1;
        electorinfos[i].vote = 0;
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
                }
            } else {
                // Handle errors / hang-up
                if (events[i].events & (EPOLLERR | EPOLLHUP)) {
                    printf("Client disconnected: fd=%d\n", events[i].data.fd);
                    int slot = find_elector_by_fd(electorinfos, events[i].data.fd);
                    if (slot != -1) {
                        electorinfos[slot].socket_fd = -1;
                        electorinfos[slot].vote = 0;
                    }
                    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, events[i].data.fd, NULL);
                    close(events[i].data.fd);
                    continue;
                }

                int elector_fd = events[i].data.fd;

                if (!elector_is_onlist(electorinfos, elector_fd)) {
                    // Unidentified client — waiting for elector number
                    char identificationcode;
                    ssize_t count = read(elector_fd, &identificationcode, sizeof(identificationcode));

                    if (count >= 1) {
                        int id = identificationcode - '0';
                        if (id < 1 || id > 7) {
                            fprintf(stderr, "Invalid elector ID from fd=%d: %c\n", elector_fd, identificationcode);
                            epoll_ctl(epoll_fd, EPOLL_CTL_DEL, elector_fd, NULL);
                            close(elector_fd);
                        } else {
                            if (electorinfos[id - 1].socket_fd != -1) {
                                fprintf(stderr, "Elector %d already connected, rejecting fd=%d\n", id, elector_fd);
                                epoll_ctl(epoll_fd, EPOLL_CTL_DEL, elector_fd, NULL);
                                close(elector_fd);
                                continue;
                            } else {
                                electorinfos[id - 1].socket_fd = elector_fd;
                                printf("Elector %d connected: fd=%d\n", id, elector_fd);
                                char welcome_msg[64];
                                snprintf(welcome_msg, sizeof(welcome_msg), "Welcome, elector of %s!\n", states[id - 1]);
                                send(elector_fd, welcome_msg, strlen(welcome_msg), 0);
                            }
                        }
                    } else {
                        // count == 0 (disconnect) or -1 (error)
                        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, elector_fd, NULL);
                        close(elector_fd);
                    }
                } else {
                    // Identified elector — handle votes
                    int slot = find_elector_by_fd(electorinfos, elector_fd);
                    for (;;) {
                        char buf[BUF_SIZE];
                        ssize_t count = read(elector_fd, buf, sizeof(buf));
                        if (count == -1) {
                            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                            perror("read"); break;
                        } else if (count == 0) {
                            printf("Elector %d disconnected: fd=%d\n", slot + 1, elector_fd);
                            if (slot != -1) {
                                electorinfos[slot].socket_fd = -1;
                                electorinfos[slot].vote = 0;
                            }
                            epoll_ctl(epoll_fd, EPOLL_CTL_DEL, elector_fd, NULL);
                            close(elector_fd);
                            break;
                        } else {
                            for (ssize_t j = 0; j < count; j++) {
                                char vote_char = buf[j];
                                if (vote_char >= '1' && vote_char <= '3') {
                                    int vote = vote_char - '0';
                                    if (slot != -1) {
                                        electorinfos[slot].vote = vote;
                                        printf("Elector %d voted for candidate %d\n", slot + 1, vote);
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    return 0;
}
