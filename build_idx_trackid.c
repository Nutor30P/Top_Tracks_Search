// build_idx_trackid.c
// Construye un índice hash en disco por 'track_id' -> offset de línea (CSV).


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
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>

#define MAXF 256
#define MAGIC "IDX1TRK"
#define VERSION 1

typedef struct {
    char     magic[8];
    uint64_t capacity;   // número de slots (potencia de 2)
    uint32_t key_col;    // índice de columna usada como clave
    uint32_t version;    // versión de formato
    uint64_t reserved[3];
} __attribute__((packed)) IdxHeader;

typedef struct {
    uint64_t hash;       // 0 = slot vacío
    uint64_t offset;     // offset del inicio de la línea en el CSV
} __attribute__((packed)) Slot;

/* -------------------- util CSV -------------------- */
static size_t parse_csv_line(const char *line, char **out, size_t max_fields) {
    size_t n = 0, L = strlen(line);
    char *buf = (char*)malloc(L + 1);
    if (!buf) return 0;
    size_t bi = 0; int inq = 0;

    for (size_t i=0; i<L; ++i) {
        char c = line[i];
        if (c == '"') {
            if (inq && i+1 < L && line[i+1] == '"') { buf[bi++] = '"'; ++i; }
            else inq = !inq;
        } else if (c == ',' && !inq) {
            buf[bi] = '\0';
            if (n < max_fields) out[n++] = strdup(buf);
            bi = 0;
        } else if (c == '\r' || c == '\n') {
            // ignore
        } else {
            buf[bi++] = c;
        }
    }
    buf[bi] = '\0';
    if (n < max_fields) out[n++] = strdup(buf);
    free(buf);
    return n;
}
static void free_fields(char **f, size_t n){ for (size_t i=0;i<n;i++) free(f[i]); }

static int find_col(char **hdr, size_t n, const char *name){
    for (size_t i=0;i<n;i++){
        if (!hdr[i]) continue;
        if (strcasecmp(hdr[i], name) == 0) return (int)i;
    }
    return -1;
}

/* -------------------- hash y helpers -------------------- */
static uint64_t fnv1a64(const char *s){
    const uint64_t FNV_OFFSET = 1469598103934665603ULL;
    const uint64_t FNV_PRIME  = 1099511628211ULL;
    uint64_t h = FNV_OFFSET;
    for (const unsigned char *p=(const unsigned char*)s; *p; ++p){ h^=(uint64_t)*p; h*=FNV_PRIME; }
    if (h==0) h=1; // reservar 0 para "vacío"
    return h;
}
static uint64_t next_pow2(uint64_t v){
    if (v <= 1) return 1;
    v--; v|=v>>1; v|=v>>2; v|=v>>4; v|=v>>8; v|=v>>16; v|=v>>32; v++;
    return v;
}

/* -------------------- recuento de filas -------------------- */
static uint64_t count_rows(FILE *fp){
    // fp POSICIONADO AL INICIO DEL ARCHIVO
    char *line = NULL; size_t bufcap = 0; ssize_t len;
    uint64_t n = 0;

    // saltar encabezado
    len = getline(&line, &bufcap, fp);
    if (len <= 0){ free(line); return 0; }

    while ((len = getline(&line, &bufcap, fp)) > 0) n++;
    free(line);
    return n;
}

/* -------------------- tabla hash (linear probing) -------------------- */
static void insert_slot(Slot *slots, uint64_t table_cap, uint64_t h, uint64_t off){
    uint64_t i = h & (table_cap - 1);
    for (;;) {
        if (slots[i].hash == 0){
            slots[i].hash   = h;
            slots[i].offset = off;
            return;
        }
        i = (i + 1) & (table_cap - 1);
    }
}

/* -------------------- main -------------------- */
int main(int argc, char **argv){
    if (argc < 3){
        fprintf(stderr, "Uso: %s <dataset.csv> <tracks.idx>\n", argv[0]);
        return 1;
    }
    const char *csv_path = argv[1];
    const char *idx_path = argv[2];

    FILE *fp = fopen(csv_path, "r");
    if (!fp){ fprintf(stderr, "No abre CSV: %s\n", strerror(errno)); return 1; }

    // Leer encabezado para detectar columna 'track_id'
    char *line = NULL; size_t bufcap = 0; ssize_t len = getline(&line, &bufcap, fp);
    if (len <= 0){ fprintf(stderr, "CSV vacío.\n"); fclose(fp); free(line); return 1; }

    char *hdr[MAXF] = {0};
    size_t nf = parse_csv_line(line, hdr, MAXF);
    int col = find_col(hdr, nf, "track_id");
    if (col < 0){
        fprintf(stderr, "No se encontró la columna 'track_id'.\n");
        fclose(fp); free(line); free_fields(hdr, nf); return 1;
    }
    fprintf(stderr, "Columna track_id = %d\n", col);
    free_fields(hdr, nf);

    // Rewind y contar filas
    if (fseeko(fp, 0, SEEK_SET) != 0){ perror("fseeko"); fclose(fp); free(line); return 1; }
    uint64_t nrows = count_rows(fp);
    fprintf(stderr, "Filas de datos: %llu\n", (unsigned long long)nrows);

    // Capacidad de la tabla (carga ~0.5)
    uint64_t table_cap = next_pow2(nrows * 2 + 1);
    fprintf(stderr, "Capacidad tabla: %llu slots\n", (unsigned long long)table_cap);

    // Crear y mapear archivo de índice
    int fd = open(idx_path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0){ fprintf(stderr, "No se puede crear índice: %s\n", strerror(errno)); fclose(fp); free(line); return 1; }

    size_t header_size = sizeof(IdxHeader);
    size_t slots_size  = sizeof(Slot) * table_cap;
    off_t total = (off_t)(header_size + slots_size);

    if (ftruncate(fd, total) != 0){
        fprintf(stderr, "ftruncate: %s\n", strerror(errno));
        close(fd); fclose(fp); free(line); return 1;
    }

    void *map = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED){
        fprintf(stderr, "mmap: %s\n", strerror(errno));
        close(fd); fclose(fp); free(line); return 1;
    }
    memset(map, 0, total);

    IdxHeader *hdrp = (IdxHeader*)map;
    memcpy(hdrp->magic, MAGIC, strlen(MAGIC));
    hdrp->capacity = table_cap;
    hdrp->key_col  = (uint32_t)col;
    hdrp->version  = VERSION;

    Slot *slots = (Slot*)((char*)map + header_size);

    // Segunda pasada: poblar índice
    if (fseeko(fp, 0, SEEK_SET) != 0){ perror("fseeko"); munmap(map,total); close(fd); fclose(fp); free(line); return 1; }
    // saltar encabezado
    len = getline(&line, &bufcap, fp);
    (void)len;

    uint64_t inserted = 0;
    for (;;) {
        off_t off = ftello(fp);                    // inicio de la línea
        len = getline(&line, &bufcap, fp);
        if (len <= 0) break;

        char *f[MAXF] = {0};
        size_t nx = parse_csv_line(line, f, MAXF);
        if (nx > (size_t)col && f[col] && f[col][0]){
            uint64_t h = fnv1a64(f[col]);
            insert_slot(slots, table_cap, h, (uint64_t)off);
            inserted++;
        }
        free_fields(f, nx);

        if ((inserted % 1000000ULL) == 0 && inserted > 0)
            fprintf(stderr, "Insertadas: %llu\n", (unsigned long long)inserted);
    }

    msync(map, total, MS_SYNC);
    munmap(map, total);
    close(fd);
    fclose(fp);
    free(line);

    fprintf(stderr, "Listo. Insertadas %llu claves.\n", (unsigned long long)inserted);
    return 0;
}
