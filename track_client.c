#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void usage(const char *prog){
    fprintf(stderr,
      "Uso:\n"
      "  %s <host> <port> ADD <track_id> <name> <artist> <album> <duration_ms>\n"
      "  %s <host> <port> SEARCH <palabra1> [<palabra2>] [<palabra3>]\n", prog, prog);
}

int main(int argc, char **argv) {
    if (argc < 5) { usage(argv[0]); return 1; }
    const char *host = argv[1]; int port = atoi(argv[2]);
    const char *cmd  = argv[3];

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    struct sockaddr_in a = {0};
    a.sin_family = AF_INET; a.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &a.sin_addr) != 1) { perror("inet_pton"); close(fd); return 1; }
    if (connect(fd, (struct sockaddr*)&a, sizeof a) < 0) { perror("connect"); close(fd); return 1; }

    char line[4096]; line[0]='\0';

    if (!strcasecmp(cmd, "ADD")) {
        if (argc < 10) { usage(argv[0]); close(fd); return 1; }
        snprintf(line, sizeof line, "ADD|%s|%s|%s|%s|%s\n",
                 argv[4], argv[5], argv[6], argv[7], argv[8]);
    } else if (!strcasecmp(cmd, "SEARCH")) {
        // SEARCH|w1[|w2][|w3]
        snprintf(line, sizeof line, "SEARCH|%s", argv[4]);
        if (argc > 5) { strncat(line, "|", sizeof line - strlen(line) - 1); strncat(line, argv[5], sizeof line - strlen(line) - 1); }
        if (argc > 6) { strncat(line, "|", sizeof line - strlen(line) - 1); strncat(line, argv[6], sizeof line - strlen(line) - 1); }
        strncat(line, "\n", sizeof line - strlen(line) - 1);
    } else {
        usage(argv[0]); close(fd); return 1;
    }

    send(fd, line, strlen(line), 0);

    char buf[2048];
    ssize_t n;
    while ((n = recv(fd, buf, sizeof buf - 1, 0)) > 0) {
        buf[n] = '\0';
        fputs(buf, stdout);
        // parar cuando llegue END (en SEARCH)
        if (strstr(buf, "\nEND\n")) break;
    }

    close(fd);
    return 0;
}
