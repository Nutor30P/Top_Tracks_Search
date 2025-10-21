/*
  lookup_trackid.c
  Busca una fila por track_id usando tracks.idx (hash -> offset).

  Compilar:
    gcc -O2 -std=c11 -Wall -Wextra -D_POSIX_C_SOURCE=200809L -D_FILE_OFFSET_BITS=64 \
      -o lookup lookup_trackid.c

  Usar:
    ./lookup merged_data.csv tracks.idx <track_id>
*/

#define _POSIX_C_SOURCE 200809L
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>

typedef struct {
    char     magic[8];
    uint64_t capacity;   // número de slots
    uint32_t key_col;    // índice de columna clave (track_id)
    uint32_t version;
    uint64_t reserved[3];
} __attribute__((packed)) IdxHeader;

typedef struct {
    uint64_t hash;       // 0 = vacío
    uint64_t offset;     // offset en el CSV (inicio de línea)
} __attribute__((packed)) Slot;

/* ---- CSV parser simple (respeta comillas) ---- */
#define MAXF 256
static size_t parse_csv_line(const char *line, char **out, size_t max_fields) {
    size_t n = 0, L = strlen(line);
    char *buf = (char*)malloc(L + 1);
    if (!buf) return 0;
    size_t bi = 0; int inq = 0;
    for (size_t i=0;i<L;i++){
        char c=line[i];
        if (c=='"'){
            if (inq && i+1<L && line[i+1]=='"'){ buf[bi++]='"'; i++; }
            else inq = !inq;
        } else if (c==',' && !inq){
            buf[bi]='\0'; if (n<max_fields) out[n++]=strdup(buf); bi=0;
        } else if (c=='\r' || c=='\n'){
            /* ignore */
        } else buf[bi++]=c;
    }
    buf[bi]='\0'; if (n<max_fields) out[n++]=strdup(buf);
    free(buf); return n;
}
static void free_fields(char **f, size_t n){ for(size_t i=0;i<n;i++) free(f[i]); }

/* ---- hash ---- */
static uint64_t fnv1a64(const char *s){
    const uint64_t OFF=1469598103934665603ULL, PR=1099511628211ULL;
    uint64_t h=OFF;
    for(const unsigned char *p=(const unsigned char*)s; *p; ++p){
        h ^= (uint64_t)*p;
        h *= PR;
    }
    if (h==0) { h=1; }  // reserva 0 para “vacío”
    return h;
}

int main(int argc, char **argv){
    if (argc < 4){
        fprintf(stderr, "Uso: %s <dataset.csv> <tracks.idx> <track_id>\n", argv[0]);
        return 1;
    }
    const char *csv_path = argv[1], *idx_path = argv[2], *key = argv[3];

    // Mapear índice
    int fd = open(idx_path, O_RDONLY);
    if (fd < 0){ fprintf(stderr, "Índice: %s\n", strerror(errno)); return 1; }
    off_t sz = lseek(fd, 0, SEEK_END);
    void *map = mmap(NULL, sz, PROT_READ, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED){ fprintf(stderr, "mmap: %s\n", strerror(errno)); close(fd); return 1; }

    IdxHeader *h = (IdxHeader*)map;
    if (strncmp(h->magic, "IDX1TRK", 7)!=0){
        fprintf(stderr, "Índice inválido.\n"); munmap(map,sz); close(fd); return 1;
    }
    uint64_t cap = h->capacity;
    uint32_t key_col = h->key_col;
    Slot *slots = (Slot*)((char*)map + sizeof(IdxHeader));

    // Abrir CSV con buffer grande
    FILE *fp = fopen(csv_path, "r");
    if (!fp){ fprintf(stderr, "CSV: %s\n", strerror(errno)); munmap(map,sz); close(fd); return 1; }
    setvbuf(fp, NULL, _IOFBF, 4*1024*1024);

    // Buscar
    uint64_t hv = fnv1a64(key);
    uint64_t i = hv & (cap - 1), start = i;
    char *line=NULL; size_t bufcap=0; ssize_t len;
    int found = 0;

    for (;;){
        Slot s = slots[i];
        if (s.hash == 0) break; // vacío => no está
        if (s.hash == hv){
            if (fseeko(fp, (off_t)s.offset, SEEK_SET) == 0) {
                len = getline(&line, &bufcap, fp);
                if (len > 0) {
                    char *f[MAXF]={0};
                    size_t nx = parse_csv_line(line, f, MAXF);
                    if (nx > key_col && f[key_col] && strcmp(f[key_col], key)==0){
                        fwrite(line, 1, (size_t)len, stdout);   // éxito
                        free_fields(f,nx);
                        found = 1;
                        break;
                    }
                    free_fields(f,nx);
                }
            }
        }
        i = (i + 1) & (cap - 1);
        if (i == start) break; // tabla completa
    }

    if (!found) printf("NOT_FOUND\n");

    free(line);
    fclose(fp);
    munmap(map, sz);
    close(fd);
    return 0;
}
