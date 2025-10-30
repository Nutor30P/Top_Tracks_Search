/* track_server.c
   Servidor TCP para insertar registros en merged_data.csv y actualizar:
   - Índice por ID (tracks.idx) via add_track_and_index(...)
   - Índice de texto incremental: nameidx/updates/bXX.log (delta)
   Protocolo:
     ADD|<track_id>|<name>|<artist>|<album>|<duration_ms>\n
   Respuesta:
     OK <offset>\n   |   ERR <mensaje>\n
*/

#define _FILE_OFFSET_BITS 64
#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <ctype.h>

#include "add_track.h"

#ifndef SERVER_PORT
#define SERVER_PORT 5555
#endif

#define RECV_BUF 8192

/* ----------------- Utilidades ------------------ */
static void trim_crlf(char *s) {
    size_t n = strlen(s);
    while (n && (s[n-1]=='\n' || s[n-1]=='\r')) s[--n] = '\0';
}

/* split por '|' in-place. Devuelve #campos */
static int split_fields(char *line, char *out[], int max_out) {
    int count = 0;
    char *p = line;
    while (count < max_out) {
        out[count++] = p;
        char *bar = strchr(p, '|');
        if (!bar) break;
        *bar = '\0';
        p = bar + 1;
    }
    return count;
}

static void send_msg(int fd, const char *msg) {
    (void)send(fd, msg, strlen(msg), 0);
}

/* ----------------- Normalización & tokens (como p1-dataProgram, versión simple) ------------------ */
static void norm_push(char **buf, size_t *len, size_t *cap, char ch){
    if(*len+1>=*cap){ *cap=(*cap?*cap*2:64); *buf=realloc(*buf,*cap); }
    (*buf)[(*len)++]=ch;
}
static char *normalize_utf8_basic_srv(const char *s){
    char *out=NULL; size_t L=0,C=0;
    for(const unsigned char *p=(const unsigned char*)s; *p; ){
        if (*p < 0x80){ char c=(char)tolower(*p++); norm_push(&out,&L,&C,c); }
        else if (p[0]==0xC3 && p[1]){
            unsigned char c2=p[1]; char m=0;
            switch(c2){
                case 0xA1: case 0x81: m='a'; break; // á/Á
                case 0xA9: case 0x89: m='e'; break; // é/É
                case 0xAD: case 0x8D: m='i'; break; // í/Í
                case 0xB3: case 0x93: m='o'; break; // ó/Ó
                case 0xBA: case 0x9A: m='u'; break; // ú/Ú
                case 0xBC: case 0x9C: m='u'; break; // ü/Ü
                case 0xB1: case 0x91: m='n'; break; // ñ/Ñ
                default: m=0; break;
            }
            if (m){ norm_push(&out,&L,&C,m); p+=2; } else { p+=2; }
        } else { p++; }
    }
    norm_push(&out,&L,&C,'\0'); return out?out:strdup("");
}
static size_t tokenize_simple(const char *norm, char ***out_tokens){
    size_t cap=8,n=0; char **tok=malloc(cap*sizeof(char*));
    size_t i=0,L=strlen(norm);
    while(i<L){
        while(i<L && !isalnum((unsigned char)norm[i])) i++;
        if(i>=L) break;
        size_t j=i; while(j<L && isalnum((unsigned char)norm[j])) j++;
        if(n==cap){ cap*=2; tok=realloc(tok,cap*sizeof(char*)); }
        tok[n++]=strndup(norm+i,j-i);
        i=j;
    }
    *out_tokens=tok; return n;
}

/* ----------------- Hash FNV-1a 64 ------------------ */
static inline uint64_t fnv1a64_srv(const char *s){
    const uint64_t OFF=1469598103934665603ULL, PR=1099511628211ULL;
    uint64_t h = OFF;
    for (const unsigned char *p=(const unsigned char*)s; *p; ++p) {
        h ^= *p;
        h *= PR;
    }
    if (h == 0) { h = 1; }
    return h;
}

/* ----------------- Delta incremental nameidx/updates ------------------ */
static void ensure_dir(const char *path){
    struct stat st; if (stat(path,&st)==0 && S_ISDIR(st.st_mode)) return;
    mkdir(path, 0775);
}

static int append_nameidx_delta(const char *namedir, const char *token, uint64_t offset){
    if(!token || !*token) return 0;
    uint64_t h = fnv1a64_srv(token);
	char updir[512];
	int nwu = snprintf(updir, sizeof updir, "%s/updates", namedir);
	if (nwu < 0 || (size_t)nwu >= sizeof updir) { errno = ENAMETOOLONG; return -1; }

	ensure_dir(namedir);
	ensure_dir(updir);

	int b = (int)(h & 0xFF);
	char path[512];
	int nw = snprintf(path, sizeof path, "%s/b%02x.log", updir, b & 0xFF);
	if (nw < 0 || (size_t)nw >= sizeof path) { errno = ENAMETOOLONG; return -1; }

	FILE *f = fopen(path, "ab");
	if (!f) return -1;
    /* formato texto: h offset\n  (hex y decimal) */
    fprintf(f, "%016" PRIx64 " %" PRIu64 "\n", h, offset);
    fclose(f);
    return 0;
}

static void record_nameidx_updates(const char *namedir, const char *name, const char *artist, uint64_t offset){
    char *n1 = normalize_utf8_basic_srv(name);
    char *n2 = normalize_utf8_basic_srv(artist);
    char **t1=NULL, **t2=NULL; size_t k1=tokenize_simple(n1,&t1), k2=tokenize_simple(n2,&t2);
    for(size_t i=0;i<k1;i++){ append_nameidx_delta(namedir, t1[i], offset); free(t1[i]); }
    for(size_t i=0;i<k2;i++){ append_nameidx_delta(namedir, t2[i], offset); free(t2[i]); }
    free(t1); free(t2); free(n1); free(n2);
}

/* ----------------- Handler de conexión ------------------ */
static void handle_client(int cfd, const char *csv_path, const char *idx_path, const char *namedir) {
    char buf[RECV_BUF];
    ssize_t n = recv(cfd, buf, sizeof(buf)-1, 0);
    if (n <= 0) return;
    buf[n] = '\0';
    trim_crlf(buf);

    /* Formato esperado:
       ADD|<track_id>|<name>|<artist>|<album>|<duration_ms>
    */
    char *f[8] = {0};
    int k = split_fields(buf, f, 8);
    if (k < 2 || strcasecmp(f[0], "ADD") != 0) {
        send_msg(cfd, "ERR uso: ADD|track_id|name|artist|album|duration_ms\n");
        return;
    }
    if (k < 6) {
        send_msg(cfd, "ERR faltan campos\n");
        return;
    }

    TrackRecord rec = {
        .track_id = f[1],
        .name     = f[2],
        .artist   = f[3],
        .album    = f[4],
        .duration_ms = f[5],
    };

    long ofs = -1;
    char err[256];
    if (add_track_and_index(csv_path, idx_path, &rec, &ofs, err, sizeof err)) {
        /* registrar delta para búsquedas por texto */
        record_nameidx_updates(namedir, rec.name, rec.artist, (uint64_t)ofs);

        char ok[128];
        snprintf(ok, sizeof ok, "OK %ld\n", ofs);
        send_msg(cfd, ok);
    } else {
        char emsg[320];
        snprintf(emsg, sizeof emsg, "ERR %s\n", err);
        send_msg(cfd, emsg);
    }
}

/* ----------------- main ------------------ */
int main(int argc, char **argv) {
    const char *csv_path = (argc > 1 ? argv[1] : "merged_data.csv");
    const char *idx_path = (argc > 2 ? argv[2] : "tracks.idx");
    const char *namedir  = (argc > 3 ? argv[3] : "nameidx");
    int port             = (argc > 4 ? atoi(argv[4]) : SERVER_PORT);

    signal(SIGPIPE, SIG_IGN);

    int sfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sfd < 0) { perror("socket"); return 1; }

    int yes = 1;
    setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port   = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(sfd, (struct sockaddr*)&a, sizeof a) < 0) { perror("bind"); close(sfd); return 1; }
    if (listen(sfd, 16) < 0) { perror("listen"); close(sfd); return 1; }

    fprintf(stderr,
            "track_server escuchando en puerto %d (CSV=%s IDX=%s NAMEIDX=%s)\n",
            port, csv_path, idx_path, namedir);

    for (;;) {
        int cfd = accept(sfd, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            break;
        }
        /* mono-hilo, un comando por conexión */
        handle_client(cfd, csv_path, idx_path, namedir);
        close(cfd);
    }

    close(sfd);
    return 0;
}
