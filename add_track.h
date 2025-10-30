#pragma once
#include <stdbool.h>
#include <stddef.h>

/* Estructura de entrada para un registro nuevo */
typedef struct {
    const char *track_id;
    const char *name;
    const char *artist;
    const char *album;
    const char *duration_ms; /* lo dejamos como string para máxima compatibilidad */
} TrackRecord;

/* Inserta una línea al CSV y (en el siguiente paso) actualiza el índice hash.
   Devuelve true si todo sale bien. out_offset = byte offset donde quedó la línea. */
bool add_track_and_index(
    const char *csv_path,
    const char *idx_path,
    const TrackRecord *rec,
    long *out_offset,
    char *errbuf, size_t errbuf_sz
);
