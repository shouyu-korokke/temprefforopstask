#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <netinet/in.h>
#include <sys/socket.h>

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        exit(1);
    }
    int port = atoi(argv[1]);

    int listen_fd;
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
    printf("Listening on port %d...\n", port);

    int client_fd = accept(listen_fd, NULL, NULL);
    if (client_fd == -1) { perror("accept"); exit(1); }

    printf("Client connected\n");
    close(client_fd);
    close(listen_fd);

    return 0;
}
