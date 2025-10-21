# ===== Makefile =====
CC      := gcc
CFLAGS  := -O2 -std=c11 -Wall -Wextra -D_POSIX_C_SOURCE=200809L -D_FILE_OFFSET_BITS=64

# Binario principal (el que exige la entrega)
MAIN := p1-dataProgram

# ---- reglas principales ----
all: $(MAIN)

$(MAIN): p1-dataProgram.c
	$(CC) $(CFLAGS) -o $@ $<

# ---- herramientas opcionales (solo se compilan si ejecutas sus targets) ----
build_idx: build_idx_trackid.c
	$(CC) $(CFLAGS) -o $@ $<

build_name_index: build_name_index.c
	$(CC) $(CFLAGS) -o $@ $<

lookup: lookup_trackid.c
	$(CC) $(CFLAGS) -o $@ $<

search_name: search_name.c
	$(CC) $(CFLAGS) -o $@ $<

track_server: track_server.c
	$(CC) $(CFLAGS) -o $@ $<

track_client: track_client.c
	$(CC) $(CFLAGS) -o $@ $<

# Construye ambos índices (ejecútalo una sola vez o cuando cambie el CSV)
indexes: build_idx build_name_index
	./build_idx merged_data.csv tracks.idx
	./build_name_index merged_data.csv nameidx

# Limpieza
clean:
	rm -f $(MAIN) build_idx build_name_index lookup search_name track_server track_client
	@echo "Listo: clean"

# Empaquetar para entregar: genera apellido1-apellido2.tar.gz
# Uso: make dist LASTNAME1=perez LASTNAME2=gomez
dist:
	@if [ -z "$(LASTNAME1)" ] || [ -z "$(LASTNAME2)" ]; then \
	  echo "Uso: make dist LASTNAME1=apellido1 LASTNAME2=apellido2"; exit 1; fi
	rm -rf $(LASTNAME1)-$(LASTNAME2)
	mkdir -p $(LASTNAME1)-$(LASTNAME2)
	cp -p p1-dataProgram.c Makefile LEEME.txt $(LASTNAME1)-$(LASTNAME2)/
	tar -czf $(LASTNAME1)-$(LASTNAME2).tar.gz $(LASTNAME1)-$(LASTNAME2)
	@echo "Paquete: $(LASTNAME1)-$(LASTNAME2).tar.gz"
# =====================
