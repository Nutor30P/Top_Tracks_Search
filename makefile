# ===== Makefile =====
CC      := gcc
CFLAGS  := -O2 -std=c11 -Wall -Wextra -D_POSIX_C_SOURCE=200809L -D_FILE_OFFSET_BITS=64

# Binario principal (el que exige la entrega)
MAIN := p1-dataProgram

.PHONY: all clean indexes

# ---- reglas principales ----
all: $(MAIN)

$(MAIN): p1-dataProgram.c add_track.c add_track.h
	$(CC) $(CFLAGS) -o $@ p1-dataProgram.c add_track.c

# ---- herramientas opcionales (solo se compilan si ejecutas sus targets) ----
build_idx: build_idx_trackid.c
	$(CC) $(CFLAGS) -o $@ $<

build_name_index: build_name_index.c
	$(CC) $(CFLAGS) -o $@ $<

lookup: lookup_trackid.c
	$(CC) $(CFLAGS) -o $@ $<

search_name: search_name.c
	$(CC) $(CFLAGS) -o $@ $<

track_server: track_server.c add_track.c add_track.h
	$(CC) $(CFLAGS) -o $@ track_server.c add_track.c

track_client: track_client.c
	$(CC) $(CFLAGS) -o $@ $<

# Construye ambos índices (ejecútalo una sola vez o cuando cambie el CSV)
indexes: build_idx build_name_index
	./build_idx merged_data.csv tracks.idx
	./build_name_index merged_data.csv nameidx

clean:
	rm -f $(MAIN) build_idx build_name_index lookup search_name track_server track_client
