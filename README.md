

<h1>🎵 Top Tracks Search 🔍</h1>

<div class="center">
  <p class="badges">
    <img alt="Linux" src="https://img.shields.io/badge/Linux-FCC624?style=for-the-badge&logo=linux&logoColor=black">
    <img alt="C" src="https://img.shields.io/badge/C-00599C?style=for-the-badge&logo=c&logoColor=white">
    <img alt="GitHub" src="https://img.shields.io/badge/GitHub-100000?style=for-the-badge&logo=github&logoColor=white">
  </p>
  <p><strong>Buscador ultrarrápido de canciones del Top 200 de Spotify usando CSV + índices en disco (hash + invertido)</strong></p>
  <p class="toc">
    <a href="#caracteristicas">🚀 Características</a> •
    <a href="#estructura">📁 Estructura</a> •
    <a href="#instalacion">⚙️ Instalación</a> •
    <a href="#indices">🗂️ Índices</a> •
    <a href="#uso">🎮 Uso</a> •
    <a href="#cliente-servidor">🔌 Cliente-Servidor</a> •
    <a href="#desarrollo">🔧 Desarrollo</a>
  </p>
</div>

<h2 id="caracteristicas">✨ Características</h2>
<div class="card">
  <table>
    <thead><tr><th>🚀 Velocidad</th><th>🔍 Búsqueda</th><th>💾 Optimización</th><th>🌐 Red</th></tr></thead>
    <tbody>
      <tr>
        <td><strong>O(1)</strong> por <code>track_id</code> con <code>tracks.idx</code> (linear probing)</td>
        <td><strong>1–3 palabras</strong> (AND) por nombre/artista; insensible a mayúsculas/tildes</td>
        <td>Índices en disco; no se carga el CSV completo. <br><strong>Delta incremental</strong> en <code>nameidx/updates/</code></td>
        <td>Sockets TCP: comandos <code>ADD</code> (insertar) y <code>SEARCH</code> (buscar por texto). <br>Resultados <strong>recientes primero</strong></td>
      </tr>
    </tbody>
  </table>
</div>

<h2 id="estructura">🏗️ Estructura del Proyecto</h2>
<pre><code>top-tracks-search/
├── src/
│   ├── p1-dataProgram.c          # Programa principal (menú local: ID, texto, agregar)
│   ├── build_idx_trackid.c       # Índice por ID → tracks.idx
│   ├── build_name_index.c        # Índice invertido base → nameidx/
│   ├── lookup_trackid.c          # Utilidad: búsqueda por ID
│   ├── search_name.c             # Utilidad: búsqueda por palabras (local)
│   ├── add_track.c / add_track.h # Append CSV + actualización de índices
│   ├── track_server.c            # Servidor TCP: ADD y SEARCH (base + delta)
│   └── track_client.c            # Cliente TCP: ADD / SEARCH
├── nameidx/                      # Índice invertido (b00..bff + updates/)
├── tracks.idx                    # Índice hash por ID
├── merged_data.csv               # Dataset
├── Makefile
└── README.md
</code></pre>

<h2 id="instalacion">⚡ Instalación</h2>

<h3>Prerrequisitos</h3>
<pre><code># Ubuntu/Debian
sudo apt update && sudo apt install -y build-essential

# CentOS/RHEL
sudo yum groupinstall -y "Development Tools"
</code></pre>

<h3>Compilación</h3>
<pre><code># Clonar & compilar
git clone https://github.com/tu-usuario/top-tracks-search.git
cd top-tracks-search
make

# Índices (si cambias CSV, vuelve a correrlos)
make indexes

# Cliente/Servidor TCP
make track_server track_client
</code></pre>

<h2 id="indices">📥 Dataset & 🗂️ Construcción de Índices</h2>

<h3>Dataset</h3>
<pre><code># Opción A: descarga automática (configura URL en el Makefile)
make fetch-data

# Opción B: manual
# Coloca merged_data.csv en la raíz del repo
</code></pre>
<p class="muted"><strong>Nota:</strong> si cambias el CSV, reconstruye índices con <code>make indexes</code>.</p>

<h3>Construcción</h3>
<pre><code># Índice hash por ID
./build_idx merged_data.csv tracks.idx

# Índice invertido base (nombre/artista)
./build_name_index merged_data.csv nameidx

# Ambos en uno
make indexes
</code></pre>
<p><strong>Incremental (nuevo):</strong> las altas hechas por <code>ADD</code> se registran en <code>nameidx/updates/bXX.log</code> como delta; no necesitas reconstruir la base para que aparezcan en búsquedas.</p>

<h2 id="uso">🎮 Uso (local)</h2>

<h3>Programa principal</h3>
<pre><code>./p1-dataProgram
</code></pre>

<h3>Ejemplos</h3>
<p><strong>Por Track ID</strong></p>
<pre><code># Menú
0 → 6rQSrBHf7HLZjtcMZ4S4b0
3 → Realizar búsqueda
</code></pre>

<p><strong>Por Nombre/Artista (AND)</strong></p>
<pre><code>1 → reggaeton
2 → lento
3 → Realizar búsqueda
</code></pre>
<p class="muted">Normalización: minúsculas + remoción de tildes básicas (<code>Beyoncé</code> == <code>beyonce</code>). <br>Las búsquedas por texto listan primero los últimos <code>MAX_SHOW</code> offsets (recientes primero).</p>

<h3>Utilidades CLI (local)</h3>
<pre><code># Búsqueda por ID
./lookup merged_data.csv tracks.idx "6rQSrBHf7HLZjtcMZ4S4b0"

# Búsqueda por palabras (base + delta)
./search_name merged_data.csv nameidx "reggaeton" "lento"
</code></pre>

<h2 id="cliente-servidor">🔌 Modo Cliente-Servidor</h2>

<h3>Servidor</h3>
<pre><code>./track_server merged_data.csv tracks.idx nameidx 5555
# track_server escuchando en puerto 5555 (CSV=... IDX=... NAMEIDX=nameidx)
</code></pre>

<h3>Insertar remotamente (ADD)</h3>
<pre><code>./track_client 127.0.0.1 5555 ADD feid-251 "FERXXO 151" "Feid" "Mor, No Le Temas a la Oscuridad" 185000
# → OK &lt;offset&gt;
</code></pre>

<h3>Buscar remotamente por nombre/artista (SEARCH)</h3>
<pre><code># Una palabra
./track_client 127.0.0.1 5555 SEARCH feid

# Varias palabras (AND)
./track_client 127.0.0.1 5555 SEARCH feid 151
</code></pre>

<p><strong>Respuesta del servidor</strong></p>
<pre><code>OK &lt;N&gt;
&lt;track_id&gt; | &lt;track_name&gt; | &lt;artist&gt; | &lt;date&gt; | &lt;region&gt;
...
END
</code></pre>
<p class="muted">El servidor fusiona <em>base + delta</em> y devuelve los últimos <code>MAX_SHOW</code> resultados (recientes primero). El delta se guarda en <code>nameidx/updates/bXX.log</code> con líneas: <code>&lt;hash_token_hex16&gt; &lt;offset_csv_decimal&gt;</code>.</p>

<h2>🧰 Comandos Makefile</h2>
<table>
  <thead><tr><th>Comando</th><th>Descripción</th></tr></thead>
  <tbody>
    <tr><td><code>make</code></td><td>Compila el programa principal</td></tr>
    <tr><td><code>make indexes</code></td><td>Construye <code>tracks.idx</code> y <code>nameidx/</code> base</td></tr>
    <tr><td><code>make fetch-data</code></td><td>Descarga el dataset (configura URL)</td></tr>
    <tr><td><code>make clean</code></td><td>Limpia binarios/objetos</td></tr>
    <tr><td><code>make dist</code></td><td>Empaqueta para entrega</td></tr>
    <tr><td><code>make track_server</code></td><td>Compila el servidor TCP</td></tr>
    <tr><td><code>make track_client</code></td><td>Compila el cliente TCP</td></tr>
  </tbody>
</table>

<h2 id="desarrollo">🔧 Desarrollo</h2>

<h3>Arquitectura interna (ID O(1))</h3>
<pre><code>track_id → Hash FNV-1a 64b → tracks.idx (linear probing) → offset → CSV → salida
</code></pre>

<h3>Arquitectura interna (Texto base + delta)</h3>
<pre><code>palabras → normalización + tokenización
  ↘ nameidx/bXX.idx (base)
  ↘ nameidx/updates/bXX.log (delta)
merge base+delta → intersección AND → offsets → lectura CSV → resultados (recientes primero)
</code></pre>

<h3>Troubleshooting</h3>
<table>
  <thead><tr><th>Problema</th><th>Solución</th></tr></thead>
  <tbody>
    <tr><td><code>NOT_FOUND</code> en búsqueda por ID</td><td>Ejecuta <code>make indexes</code></td></tr>
    <tr><td><code>nameidx/</code> vacío o faltante</td><td><code>./build_name_index merged_data.csv nameidx</code></td></tr>
    <tr><td>Inserción no aparece en búsqueda</td><td>Verifica <code>nameidx/updates/</code> y que ves “recientes primero”</td></tr>
    <tr><td>Descarga OneDrive falla</td><td>Revisa URL directa en <code>Makefile</code></td></tr>
    <tr><td>CSV grande/lento</td><td>Coloca el CSV en disco local (no red)</td></tr>
    <tr><td><em>Clock skew detected</em> al compilar</td><td><code>make -B</code> o <code>make clean &amp;&amp; make</code></td></tr>
  </tbody>
</table>

<h2>👨‍💻 Autor</h2>
<p><strong>Tu Nombre</strong><br>
  <a href="mailto:pbueno@unal.edu.co">pbueno@unal.edu.co</a> •
  <a href="mailto:jyanezf@unal.edu.co">jyanezf@unal.edu.co</a><br>
  LinkedIn: <a href="https://www.linkedin.com/in/pabloandresbuenolopez">www.linkedin.com/in/pabloandresbuenolopez</a>
</p>

<div class="center muted">
  <p>⭐ ¿Te gusta este proyecto? ¡Dale una estrella en GitHub! — ¿Dudas o ideas? Abre un issue.</p>
</div>

</body>
</html>
