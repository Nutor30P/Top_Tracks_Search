Top Tracks Search (C + Linux)

Buscador ultrarrápido de canciones del Top 200 de Spotify usando CSV + índices en disco.
Permite buscar por:

track_id (búsqueda exacta, O(1) promedio)

Nombre / Artista (1–3 palabras, AND; ignora mayúsculas y tildes)

La salida es compacta:

<track_id> | <track_name> | <artist> | <date> | <region>

🧱 Estructura del proyecto
.
├─ p1-dataProgram.c      # Programa principal (menú)
├─ build_idx_trackid.c   # Construye índice por track_id  -> tracks.idx
├─ build_name_index.c    # Construye índice invertido     -> nameidx/
├─ lookup_trackid.c      # Lookup simple por track_id (utilidad)
├─ search_name.c         # Buscador simple por palabras (utilidad)
├─ track_server.c        # (opcional) servidor FIFO
├─ track_client.c        # (opcional) cliente FIFO
├─ Makefile              # Compilación y utilidades
├─ .gitignore
└─ README.md

⚙️ Requisitos

Linux / WSL

gcc y herramientas de build (build-essential)

Dataset: merged_data.csv (no se incluye por tamaño)

📥 Obtener el dataset
Opción A — Descargar automáticamente (OneDrive)

Edita el Makefile y coloca tu URL directa en DATA_URL
(Las URLs de OneDrive deben tener el formato: https://onedrive.live.com/download?resid=...)

Luego ejecuta:

make fetch-data

Opción B — Manual

Coloca merged_data.csv en la carpeta del proyecto.

Nota: Si cambias el CSV, deberás reconstruir los índices.

🛠️ Compilar
make


Esto genera el binario principal p1-dataProgram.

Si quieres compilar todas las utilidades (opcional):
make build_idx build_name_index lookup search_name

🗂️ Construir índices (una vez)
# índice por track_id (hash table de direccionamiento abierto)
./build_idx merged_data.csv tracks.idx

# índice invertido (nombre + artista)
./build_name_index merged_data.csv nameidx


Puedes automatizar ambos con:

make indexes

▶️ Ejecutar
./p1-dataProgram


Al iniciar, el programa te pedirá las rutas (puedes aceptar los defaults si estás en la carpeta del proyecto):

CSV: merged_data.csv

Índice ID: tracks.idx

Índice texto: nameidx/

Luego verás el menú:

1. Ingresar primer criterio de búsqueda (track_id)
2. Ingresar segundo criterio de búsqueda (palabra nombre/artista)
3. Ingresar tercer criterio de búsqueda (si aplica)
4. Realizar búsqueda
5. Salir

Ejemplos rápidos

Por ID:

1 → 6rQSrBHf7HLZjtcMZ4S4b0
4 → Ejecutar


Por nombre/artista (palabras, AND):

2 → reggaeton
3 → lento
4 → Ejecutar


La búsqueda ignora tildes y mayúsculas (Beyoncé == beyonce).

🔎 ¿Qué hace internamente?

track_id: se calcula hash, se busca en tracks.idx → se obtiene offset → se lee solo esa línea del CSV.

nombre/artista: se normalizan palabras, se buscan en nameidx/ las listas de offsets y se intersecan → se leen solo esas líneas.

Todo en modo lectura; el CSV no se modifica.

🧪 Utilidades (opcionales)

./lookup merged_data.csv tracks.idx <track_id>

./search_name merged_data.csv nameidx <pal1> [pal2] [pal3]

./track_server ... y ./track_client ... (demo FIFO)

🧰 Makefile: comandos útiles
make               # compila p1-dataProgram
make indexes       # construye tracks.idx + nameidx/ (requiere merged_data.csv)
make fetch-data    # descarga merged_data.csv desde OneDrive (configura DATA_URL)
make clean         # limpia binarios
make dist LASTNAME1=apellido1 LASTNAME2=apellido2  # empaqueta para entrega

🚑 Problemas frecuentes

“NOT_FOUND” al buscar por ID:

El track_id no existe, o los índices no corresponden al CSV actual. Reconstruye con make indexes.

nameidx/ vacío o faltante:

Ejecuta ./build_name_index merged_data.csv nameidx o make indexes.

Descarga de OneDrive falla:

Verifica que la URL sea de descarga directa (.../download?resid=...).

CSV muy grande / lento:

Los índices están pensados para saltos aleatorios. Asegura que el CSV esté en disco local (no en red).

📄 Licencia

Usa y modifica para fines académicos. Si lo reutilizas, por favor da crédito al autor del repo.

¿Sugerencias o mejoras? ¡Pull requests bienvenidos! ✨
