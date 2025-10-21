// step1_inspect.c
// Compila en Linux/WSL:  gcc -O2 -std=c11 -Wall -Wextra -D_POSIX_C_SOURCE=200809L -o step1_inspect step1_inspect.c

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>   // strdup
#include <strings.h>  // strcasecmp
#include <sys/types.h> // ssize_t
#include <errno.h>

#define MAXF 256

static size_t parse_csv_line(const char *line, char **out, size_t max_fields) {
    size_t n = 0;
    size_t L = strlen(line);
    char *buf = (char*)malloc(L + 1);
    if (!buf) return 0;
    size_t bi = 0;
    int in_quotes = 0;

    for (size_t i = 0; i < L; ++i) {
        char c = line[i];
        if (c == '"') {
            if (in_quotes && i + 1 < L && line[i+1] == '"') {
                buf[bi++] = '"'; ++i;
            } else {
                in_quotes = !in_quotes;
            }
        } else if (c == ',' && !in_quotes) {
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

static int find_col(char **hdr, size_t n, const char *name) {
    for (size_t i = 0; i < n; ++i) {
        if (!hdr[i]) continue;
        if (strcasecmp(hdr[i], name) == 0) return (int)i;
        if (strcasecmp(name, "track_name") == 0 &&
            (strcasecmp(hdr[i], "track") == 0 || strcasecmp(hdr[i], "name") == 0 || strcasecmp(hdr[i], "title") == 0))
            return (int)i;
        if (strcasecmp(name, "artist") == 0 &&
            (strcasecmp(hdr[i], "artists") == 0 || strcasecmp(hdr[i], "singer") == 0))
            return (int)i;
        if (strcasecmp(name, "date") == 0 && (strcasecmp(hdr[i], "release_date") == 0))
            return (int)i;
        if (strcasecmp(name, "region") == 0 && (strcasecmp(hdr[i], "country") == 0))
            return (int)i;
    }
    return -1;
}

static void free_fields(char **f, size_t n) {
    for (size_t i = 0; i < n; ++i) free(f[i]);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Uso: %s <ruta_csv>\n", argv[0]);
        return 1;
    }
    const char *path = argv[1];
    FILE *fp = fopen(path, "r");
    if (!fp) { fprintf(stderr, "No pude abrir %s: %s\n", path, strerror(errno)); return 1; }

    char *line = NULL;
    size_t cap = 0;
    ssize_t len = getline(&line, &cap, fp);
    if (len <= 0) { fprintf(stderr, "CSV vacío o sin encabezado.\n"); fclose(fp); free(line); return 1; }

    char *fields[MAXF] = {0};
    size_t nf = parse_csv_line(line, fields, MAXF);
    if (nf == 0) { fprintf(stderr, "No se pudo parsear encabezado.\n"); fclose(fp); free(line); return 1; }

    int c_track_id = find_col(fields, nf, "track_id");
    int c_track    = find_col(fields, nf, "track_name");
    int c_artist   = find_col(fields, nf, "artist");
    int c_date     = find_col(fields, nf, "date");
    int c_region   = find_col(fields, nf, "region");

    printf("Columnas detectadas:\n");
    printf("  track_id = %d, track_name = %d, artist = %d, date = %d, region = %d\n",
           c_track_id, c_track, c_artist, c_date, c_region);

    free_fields(fields, nf);

    size_t rows = 0, shown = 0;
    while ((len = getline(&line, &cap, fp)) > 0) {
        char *f2[MAXF] = {0};
        size_t n2 = parse_csv_line(line, f2, MAXF);
        if (n2 > 0) {
            if (shown < 5) {
                const char *sid   = (c_track_id >= 0 && c_track_id < (int)n2) ? f2[c_track_id] : "";
                const char *sname = (c_track    >= 0 && c_track    < (int)n2) ? f2[c_track]    : "";
                const char *sart  = (c_artist   >= 0 && c_artist   < (int)n2) ? f2[c_artist]   : "";
                const char *sdate = (c_date     >= 0 && c_date     < (int)n2) ? f2[c_date]     : "";
                const char *sreg  = (c_region   >= 0 && c_region   < (int)n2) ? f2[c_region]   : "";
                printf("#%zu  id=%s | track=%s | artist=%s | date=%s | region=%s\n",
                       rows+1, sid, sname, sart, sdate, sreg);
                shown++;
            }
            rows++;
        }
        free_fields(f2, n2);
    }

    printf("Filas totales (excluyendo encabezado): %zu\n", rows);

    free(line);
    fclose(fp);
    return 0;
}

