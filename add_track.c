/* add_track.c
   Append seguro al CSV gigante + actualización del índice tracks.idx (formato IDX1TRK)
   - Hash FNV-1a 64
   - Probing lineal
   - Validación por track_id real leyendo la columna key_col del CSV en el offset
*/

#define _FILE_OFFSET_BITS 64
#include "add_track.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <errno.h>

/* ============================================================
   Estructuras ON-DISK del índice por ID (deben coincidir con p1-dataProgram)
   ============================================================ */
typedef struct {
    char     magic[8];
    uint64_t capacity;   // número de slots (potencia de 2)
    uint32_t key_col;    // columna de track_id en el CSV
    uint32_t version;
    uint64_t reserved[3];
} __attribute__((packed)) IdxHeader;

typedef struct { uint64_t hash, offset; } __attribute__((packed)) Slot;

/* ============================================================
   Hash FNV-1a 64 (idéntico al usado en p1-dataProgram)
   ============================================================ */
static inline uint64_t fnv1a64_local(const char *s){
    const uint64_t OFF=1469598103934665603ULL, PR=1099511628211ULL;
    uint64_t h=OFF;
    for (const unsigned char *p=(const unsigned char*)s; *p; ++p){
        h ^= (uint64_t)(*p);
        h *= PR;
    }
    if (h==0) h=1; /* reservamos 0 como “vacío” en slots */
    return h;
}

/* ============================================================
   CSV mínimo (parser que respeta comillas) para comparar key_col
   ============================================================ */
static size_t parse_csv_line_min(const char *line, char **out, size_t max_fields){
    size_t n=0, L=strlen(line), bi=0; int inq=0;
    char *buf=(char*)malloc(L+1); if(!buf) return 0;
    for(size_t i=0;i<L;i++){
        char c=line[i];
        if(c=='"'){
            if(inq && i+1<L && line[i+1]=='"'){ buf[bi++]='"'; i++; }
            else inq=!inq;
        } else if(c==',' && !inq){
            buf[bi]='\0'; if(n<max_fields) out[n++]=strdup(buf); bi=0;
        } else if(c=='\r' || c=='\n'){
            /* ignorar finales de línea */
        } else {
            buf[bi++]=c;
        }
    }
    buf[bi]='\0'; if(n<max_fields) out[n++]=strdup(buf);
    free(buf); return n;
}
static void free_fields_min(char **f, size_t n){ for(size_t i=0;i<n;i++) free(f[i]); }

/* ============================================================
   Utilidades de acceso al índice
   ============================================================ */
static int read_header(FILE *f, IdxHeader *h){
    if (fseeko(f, 0, SEEK_SET)!=0) return -1;
    if (fread(h, sizeof(*h), 1, f)!=1) return -1;
    if (strncmp(h->magic,"IDX1TRK",7)!=0 || h->capacity==0) return -1;
    return 0;
}
static int read_slot(FILE *f, uint64_t idx, Slot *s){
    off_t base = (off_t)sizeof(IdxHeader) + (off_t)idx * (off_t)sizeof(Slot);
    if (fseeko(f, base, SEEK_SET)!=0) return -1;
    if (fread(s, sizeof(*s), 1, f)!=1) return -1;
    return 0;
}
static int write_slot(FILE *f, uint64_t idx, const Slot *s){
    off_t base = (off_t)sizeof(IdxHeader) + (off_t)idx * (off_t)sizeof(Slot);
    if (fseeko(f, base, SEEK_SET)!=0) return -1;
    if (fwrite(s, sizeof(*s), 1, f)!=1) return -1;
    return 0;
}

/* Compara que en el CSV, en la columna key_col del offset dado, esté exactamente track_id */
static int csv_track_id_equals(const char *csv_path, uint64_t key_col, uint64_t offset, const char *track_id){
    FILE *fp = fopen(csv_path, "r"); if(!fp) return 0;
    if (fseeko(fp, (off_t)offset, SEEK_SET)!=0){ fclose(fp); return 0; }
    char *line=NULL; size_t cap=0; ssize_t len=getline(&line,&cap,fp);
    int eq=0;
    if (len>0){
        char *f[64]={0}; size_t nf=parse_csv_line_min(line,f,64);
        if (key_col < nf && f[key_col] && strcmp(f[key_col], track_id)==0) eq=1;
        free_fields_min(f,nf);
    }
    free(line); fclose(fp);
    return eq;
}

/* Inserta (o actualiza) en tabla con direccionamiento abierto y probing lineal.
   Devuelve 0=OK, -1=error (errno set: ENOSPC si la tabla está llena). */
static int index_insert_trackid_idx1trk(const char *idx_path, const char *csv_path,
                                        const char *track_id, long offset){
    FILE *f = fopen(idx_path, "rb+");
    if (!f) return -1;

    IdxHeader H;
    if (read_header(f, &H)!=0){ fclose(f); return -1; }
    uint64_t cap = H.capacity;
    uint64_t hv  = fnv1a64_local(track_id);
    uint64_t i   = hv & (cap - 1);
    uint64_t start = i;

    for(;;){
        Slot s;
        if (read_slot(f, i, &s)!=0){ fclose(f); return -1; }

        if (s.hash == 0){
            /* Slot vacío -> insertar */
            Slot nw = { .hash = hv, .offset = (uint64_t)offset };
            if (write_slot(f, i, &nw)!=0){ fclose(f); return -1; }
            fflush(f); fclose(f);
            return 0;
        }

        if (s.hash == hv){
            /* Posible duplicado: confirmamos por track_id exacto en CSV */
		if (csv_track_id_equals(csv_path, H.key_col, s.offset, track_id)){
		    fclose(f);
    		    errno = EEXIST;     /* duplicado */
    		    return -1;
		}            /* mismo hash pero distinto track_id real: colisión -> seguimos */
        }

        i = (i + 1) & (cap - 1);
        if (i == start){ fclose(f); errno = ENOSPC; return -1; } /* tabla llena */
    }
}

/* ============================================================
   Escritura simple de campos CSV (sin escapado completo por ahora)
   ============================================================ */
static void csv_write_field(FILE *f, const char *s){
    /* Versión mínima: asume que no hay comas/quotes problemáticos.
       Si luego lo necesitas, cambiamos a rutina RFC4180 (comillas dobles, etc.). */
    fputs(s ? s : "", f);
}

/* ============================================================
   API pública
   ============================================================ */
bool add_track_and_index(
    const char *csv_path,
    const char *idx_path,
    const TrackRecord *rec,
    long *out_offset,
    char *errbuf, size_t errbuf_sz
){
    if (!rec || !rec->track_id || !rec->name || !rec->artist || !rec->album || !rec->duration_ms){
        if (errbuf && errbuf_sz) snprintf(errbuf, errbuf_sz, "Campos incompletos");
        return false;
    }

    /* 1) Append al CSV y capturar offset */
    FILE *csv = fopen(csv_path, "ab+");
    if (!csv) {
        if (errbuf && errbuf_sz) snprintf(errbuf, errbuf_sz, "CSV open: %s", strerror(errno));
        return false;
    }
    if (fseeko(csv, 0, SEEK_END) != 0) {
        if (errbuf && errbuf_sz) snprintf(errbuf, errbuf_sz, "CSV seek end: %s", strerror(errno));
        fclose(csv);
        return false;
    }
    long ofs = ftello(csv);
    if (ofs < 0) {
        if (errbuf && errbuf_sz) snprintf(errbuf, errbuf_sz, "CSV tell: %s", strerror(errno));
        fclose(csv);
        return false;
    }

    /* Línea CSV: track_id,name,artist,album,duration_ms\n */
    csv_write_field(csv, rec->track_id);  fputc(',', csv);
    csv_write_field(csv, rec->name);      fputc(',', csv);
    csv_write_field(csv, rec->artist);    fputc(',', csv);
    csv_write_field(csv, rec->album);     fputc(',', csv);
    csv_write_field(csv, rec->duration_ms);
    fputc('\n', csv);

    if (fflush(csv) != 0) {
        if (errbuf && errbuf_sz) snprintf(errbuf, errbuf_sz, "CSV flush: %s", strerror(errno));
        fclose(csv);
        return false;
    }
    fclose(csv);

    /* 2) Actualizar índice IDX1TRK */
    if (idx_path && idx_path[0]) {
        if (index_insert_trackid_idx1trk(idx_path, csv_path, rec->track_id, ofs) != 0) {
            if (errbuf && errbuf_sz) {
                if (errno == ENOSPC) snprintf(errbuf, errbuf_sz, "Índice lleno (ENOSPC): requiere rehash");
		else if (errno == EEXIST) snprintf(errbuf, errbuf_sz, "track_id ya existe");
		else snprintf(errbuf, errbuf_sz, "Index insert: %s", strerror(errno));
            }
            return false;
        }
    }

    if (out_offset) *out_offset = ofs;
    return true;
}
