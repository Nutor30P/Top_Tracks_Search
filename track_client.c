#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef SERVER_PORT
#define SERVER_PORT 5555
#endif

int main(int argc, char **argv) {
    if (argc < 8) {
        fprintf(stderr, "Uso: %s <host> <port> ADD <track_id> <name> <artist> <album> <duration_ms>\n", argv[0]);
        return 1;
    }
    const char *host = argv[1];
    int port = atoi(argv[2]);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &a.sin_addr) != 1) { perror("inet_pton"); close(fd); return 1; }

    if (connect(fd, (struct sockaddr*)&a, sizeof a) < 0) { perror("connect"); close(fd); return 1; }

    char line[4096];
    snprintf(line, sizeof line, "ADD|%s|%s|%s|%s|%s\n",
             argv[4], argv[5], argv[6], argv[7], argv[8]);

    send(fd, line, strlen(line), 0);

    char buf[2048];
    ssize_t n = recv(fd, buf, sizeof buf - 1, 0);
    if (n > 0) { buf[n] = '\0'; fputs(buf, stdout); }

    close(fd);
    return 0;
}
