/* track_server.c
   Servidor TCP:
     - ADD|track_id|name|artist|album|duration_ms -> inserta en CSV e indices
     - SEARCH|w1[|w2][|w3] -> busca por palabras (name/artist), base+delta, recientes primero
   Respuestas:
     - ADD:    OK <offset>\n  | ERR <mensaje>\n
     - SEARCH: OK <N>\n <linea_compacta>... END\n | ERR <mensaje>\n
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
#include <fcntl.h>
#include <stdarg.h>   // <-- NECESARIO para va_list, va_start, va_end
#include "add_track.h"

#ifndef SERVER_PORT
#define SERVER_PORT 5555
#endif

#define RECV_BUF 8192
#define NBKT 256
#define MAX_SHOW 20

/* ----------------- Utils ------------------ */
static void trim_crlf(char *s) {
    size_t n = strlen(s);
    while (n && (s[n-1]=='\n' || s[n-1]=='\r')) s[--n] = '\0';
}
static int split_fields(char *line, char *out[], int max_out) {
    int count = 0; char *p = line;
    while (count < max_out) {
        out[count++] = p;
        char *bar = strchr(p, '|'); if (!bar) break;
        *bar = '\0'; p = bar + 1;
    }
    return count;
}
static void send_str(int fd, const char *s){ (void)send(fd, s, strlen(s), 0); }
static void send_fmt(int fd, const char *fmt, ...) {
    char buf[2048];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    send_str(fd, buf);
}

/* ----------------- Normalización/tokenización ------------------ */
static void norm_push(char **buf, size_t *len, size_t *cap, char ch){
    if(*len+1>=*cap){ *cap=(*cap?*cap*2:64); *buf=realloc(*buf,*cap); }
    (*buf)[(*len)++]=ch;
}
static char *normalize_utf8_basic(const char *s){
    char *out=NULL; size_t L=0,C=0;
    for(const unsigned char *p=(const unsigned char*)s; *p; ){
        if (*p < 0x80){ char c=(char)tolower(*p++); norm_push(&out,&L,&C,c); }
        else if (p[0]==0xC3 && p[1]){
            unsigned char c2=p[1]; char m=0;
            switch(c2){
                case 0xA1: case 0x81: m='a'; break;
                case 0xA9: case 0x89: m='e'; break;
                case 0xAD: case 0x8D: m='i'; break;
                case 0xB3: case 0x93: m='o'; break;
                case 0xBA: case 0x9A: m='u'; break;
                case 0xBC: case 0x9C: m='u'; break;
                case 0xB1: case 0x91: m='n'; break;
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

/* ----------------- Hash FNV-1a ------------------ */
static inline uint64_t fnv1a64(const char *s){
    const uint64_t OFF=1469598103934665603ULL, PR=1099511628211ULL;
    uint64_t h = OFF;
    for (const unsigned char *p=(const unsigned char*)s; *p; ++p) { h ^= *p; h *= PR; }
    if (h == 0) { h = 1; }
    return h;
}

/* ----------------- Delta incremental (ya existía para ADD) ------------------ */
static void ensure_dir(const char *path){
    struct stat st; if (stat(path,&st)==0 && S_ISDIR(st.st_mode)) return;
    mkdir(path, 0775);
}
static int append_nameidx_delta(const char *namedir, const char *token, uint64_t offset){
    if(!token || !*token) return 0;
    uint64_t h = fnv1a64(token);
    char updir[512];
    int nwu = snprintf(updir, sizeof updir, "%s/updates", namedir);
    if (nwu < 0 || (size_t)nwu >= sizeof updir) { errno = ENAMETOOLONG; return -1; }
    ensure_dir(namedir); ensure_dir(updir);
    int b = (int)(h & 0xFF);
    char path[512];
    int nw = snprintf(path, sizeof path, "%s/b%02x.log", updir, b & 0xFF);
    if (nw < 0 || (size_t)nw >= sizeof path) { errno = ENAMETOOLONG; return -1; }
    FILE *f = fopen(path, "ab"); if (!f) return -1;
    fprintf(f, "%016" PRIx64 " %" PRIu64 "\n", h, offset);
    fclose(f);
    return 0;
}
static void record_nameidx_updates(const char *namedir, const char *name, const char *artist, uint64_t offset){
    char *n1 = normalize_utf8_basic(name);
    char *n2 = normalize_utf8_basic(artist);
    char **t1=NULL, **t2=NULL; size_t k1=tokenize_simple(n1,&t1), k2=tokenize_simple(n2,&t2);
    for(size_t i=0;i<k1;i++){ append_nameidx_delta(namedir, t1[i], offset); free(t1[i]); }
    for(size_t i=0;i<k2;i++){ append_nameidx_delta(namedir, t2[i], offset); free(t2[i]); }
    free(t1); free(t2); free(n1); free(n2);
}

/* ----------------- CSV parse/print compacto ------------------ */
static size_t parse_csv_line(const char *line, char **out, size_t max_fields){
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
        } else { buf[bi++]=c; }
    }
    buf[bi]='\0'; if(n<max_fields) out[n++]=strdup(buf);
    free(buf); return n;
}
static void free_fields(char **f, size_t n){ for(size_t i=0;i<n;i++) free(f[i]); }
typedef struct { int track_id, track_name, artist, date, region; } ColIdx;
static ColIdx gcols = {10,1,4,3,6};
static int find_col(char **hdr, size_t n, const char *name){
    for(size_t i=0;i<n;i++) if(hdr[i] && strcasecmp(hdr[i],name)==0) return (int)i;
    return -1;
}
static void load_cols_once(const char *csv){
    FILE *fp=fopen(csv,"r"); if(!fp) return;
    char *line=NULL; size_t cap=0; ssize_t len=getline(&line,&cap,fp);
    if(len>0){
        char *hdr[256]={0}; size_t nf=parse_csv_line(line,hdr,256);
        int c;
        if ((c=find_col(hdr,nf,"track_id"))   >=0) gcols.track_id   = c;
        if ((c=find_col(hdr,nf,"track_name"))>=0) gcols.track_name = c;
        if ((c=find_col(hdr,nf,"artist"))    >=0) gcols.artist     = c;
        if ((c=find_col(hdr,nf,"date"))      >=0) gcols.date       = c;
        if ((c=find_col(hdr,nf,"region"))    >=0) gcols.region     = c;
        for(size_t i=0;i<nf;i++) free(hdr[i]);
    }
    free(line); fclose(fp);
}
static void print_compact_line_to_fd(int fd, const char *line){
    char *f[256]={0}; size_t nx=parse_csv_line(line,f,256);
    const char *id    = (gcols.track_id   < (int)nx && f[gcols.track_id])   ? f[gcols.track_id]   : "-";
    const char *name  = (gcols.track_name < (int)nx && f[gcols.track_name]) ? f[gcols.track_name] : "-";
    const char *art   = (gcols.artist     < (int)nx && f[gcols.artist])     ? f[gcols.artist]     : "-";
    const char *date  = (gcols.date       < (int)nx && f[gcols.date])       ? f[gcols.date]       : "-";
    const char *reg   = (gcols.region     < (int)nx && f[gcols.region])     ? f[gcols.region]     : "-";
    if (nx == 5) { if (!strcmp(id,"-")) id=f[0]; if (!strcmp(name,"-")) name=f[1]; if (!strcmp(art,"-")) art=f[2]; }
    send_fmt(fd, "%s | %s | %s | %s | %s\n", id, name, art, date, reg);
    free_fields(f,nx);
}

/* ----------------- Postings base + delta + merge + AND ------------------ */
static int cmp_u64(const void *a, const void *b){
    const uint64_t A = *(const uint64_t*)a;
    const uint64_t B = *(const uint64_t*)b;
    if (A < B) return -1;
    else if (A > B) return 1;
    else return 0;
}
static uint64_t *load_postings_base(const char *dir, uint64_t h, size_t *out_n){
    int b=(int)(h & (NBKT-1));
    char path[512]; snprintf(path,sizeof(path),"%s/b%02x.idx",dir,b);
    FILE *f=fopen(path,"rb"); if(!f){ *out_n=0; return NULL; }
    for(;;){
        uint64_t hh; uint32_t df, pad;
        if (fread(&hh,8,1,f)!=1) break;
        if (fread(&df,4,1,f)!=1) break;
        if (fread(&pad,4,1,f)!=1) break;
        if (hh==h){
            uint64_t *arr=malloc((size_t)df*sizeof(uint64_t));
            if (!arr){ fclose(f); *out_n=0; return NULL; }
            if (fread(arr,8,df,f)!=(size_t)df){ free(arr); fclose(f); *out_n=0; return NULL; }
            fclose(f); *out_n=df; return arr;
        } else {
            if (fseeko(f,(off_t)df*8,SEEK_CUR)!=0) break;
        }
    }
    fclose(f); *out_n=0; return NULL;
}
static uint64_t *load_postings_delta(const char *dir, uint64_t h, size_t *out_n){
    int b=(int)(h & (NBKT-1));
    char path[512]; snprintf(path,sizeof(path),"%s/updates/b%02x.log",dir,b);
    FILE *f=fopen(path,"rb"); if(!f){ *out_n=0; return NULL; }
    size_t cap=128,n=0; uint64_t *arr=malloc(cap*sizeof(uint64_t));
    char *line=NULL; size_t L=0; ssize_t len;
    while ((len=getline(&line,&L,f))>0){
        if (len<18) continue;
        char hex[17]={0}; memcpy(hex,line,16);
        uint64_t hh=0; if (sscanf(hex,"%16" SCNx64, &hh)!=1) continue;
        if (hh!=h) continue;
        char *sp=strchr(line,' '); if (!sp) continue;
        uint64_t off=0; if (sscanf(sp+1,"%" SCNu64, &off)!=1) continue;
        if (n==cap){ cap*=2; arr=realloc(arr,cap*sizeof(uint64_t)); }
        arr[n++]=off;
    }
    free(line); fclose(f);
    if (n>1){
        qsort(arr,n,sizeof(uint64_t),cmp_u64);
        size_t m=0; for(size_t i=0;i<n;i++){ if (m==0 || arr[i]!=arr[m-1]) arr[m++]=arr[i]; }
        n=m;
    }
    *out_n=n; return arr;
}
static uint64_t *merge_base_delta(const uint64_t *base,size_t nb,const uint64_t *del,size_t nd,size_t *nout){
    uint64_t *r=malloc(((nb+nd)?(nb+nd):1)*sizeof(uint64_t));
    size_t i=0,j=0,k=0;
    while(i<nb && j<nd){
        if (base[i] < del[j]) r[k++]=base[i++];
        else if (del[j] < base[i]) r[k++]=del[j++];
        else { r[k++]=base[i]; i++; j++; }
    }
    while(i<nb) r[k++]=base[i++];
    while(j<nd) r[k++]=del[j++];
    *nout=k; return r;
}
static uint64_t *intersect(const uint64_t *a,size_t na,const uint64_t *b,size_t nb,size_t *nc){
    size_t i=0,j=0,cap=(na<nb?na:nb),n=0; uint64_t *c=malloc((cap?cap:1)*sizeof(uint64_t));
    while(i<na && j<nb){ if(a[i]==b[j]){ c[n++]=a[i]; i++; j++; } else if(a[i]<b[j]) i++; else j++; }
    *nc=n; return c;
}

/* ----------------- Manejo de comandos ------------------ */
static void handle_ADD(int cfd, const char *csv_path, const char *idx_path, const char *namedir,
                       char *f[], int k){
    if (k < 6){ send_str(cfd, "ERR faltan campos\n"); return; }
    TrackRecord rec = { .track_id=f[1], .name=f[2], .artist=f[3], .album=f[4], .duration_ms=f[5] };
    long ofs=-1; char err[256];
    if (add_track_and_index(csv_path, idx_path, &rec, &ofs, err, sizeof err)) {
        record_nameidx_updates(namedir, rec.name, rec.artist, (uint64_t)ofs);
        send_fmt(cfd, "OK %ld\n", ofs);
    } else {
        if (errno==EEXIST) send_str(cfd, "ERR track_id ya existe\n");
        else send_fmt(cfd, "ERR %s\n", err);
    }
}
static void handle_SEARCH(int cfd, const char *csv_path, const char *namedir, char *f[], int k){
    if (k < 2){ send_str(cfd, "ERR uso: SEARCH|palabra1[|palabra2][|palabra3]\n"); return; }

    /* preparar lista de postings fusionados (base+delta) para cada palabra y hacer AND */
    uint64_t *post=NULL; size_t pn=0;

    for (int qi=1; qi<k && qi<=3; ++qi){
        char *norm = normalize_utf8_basic(f[qi]);
        char **toks=NULL; size_t ntok=tokenize_simple(norm,&toks); free(norm);
        if (ntok==0){ free(toks); continue; }

        uint64_t h = fnv1a64(toks[0]);               /* usamos el primer token */
        for(size_t t=0;t<ntok;t++) free(toks[t]);
        free(toks);

        size_t nb=0, nd=0, nn=0;
        uint64_t *base = load_postings_base(namedir, h, &nb);
        uint64_t *delt = load_postings_delta(namedir, h, &nd);
        uint64_t *tp = NULL;

        if (base && delt){ tp = merge_base_delta(base,nb,delt,nd,&nn); free(base); free(delt); }
        else if (base){ tp=base; nn=nb; }
        else if (delt){ tp=delt; nn=nd; }
        else { tp=NULL; nn=0; }

        if (qi==1){ post=tp; pn=nn; }
        else {
            size_t cn=0; uint64_t *cp=intersect(post,pn,tp,nn,&cn);
            free(post); free(tp); post=cp; pn=cn;
        }
        if (pn==0) break;
    }

    if (!post || pn==0){ send_str(cfd, "OK 0\nEND\n"); free(post); return; }

    /* abrir CSV y emitir últimos MAX_SHOW (recientes primero) */
    FILE *fp=fopen(csv_path,"r");
    if (!fp){ send_fmt(cfd,"ERR CSV: %s\n", strerror(errno)); free(post); return; }

    /* cargar header para columnas */
    load_cols_once(csv_path);

    size_t start = (pn>MAX_SHOW)?(pn-MAX_SHOW):0;
    size_t count = (pn-start);
    send_fmt(cfd, "OK %zu\n", count>MAX_SHOW?MAX_SHOW:count);

    size_t emitted=0;
    for (ssize_t idx=(ssize_t)pn-1; idx>=(ssize_t)start && emitted<MAX_SHOW; --idx){
        if (fseeko(fp,(off_t)post[idx],SEEK_SET)!=0) continue;
        char *line=NULL; size_t cap=0; ssize_t len=getline(&line,&cap,fp);
        if (len>0){ print_compact_line_to_fd(cfd, line); emitted++; }
        free(line);
    }
    send_str(cfd, "END\n");
    fclose(fp); free(post);
}

/* ----------------- Handler conexión ------------------ */
static void handle_client(int cfd, const char *csv_path, const char *idx_path, const char *namedir) {
    char buf[RECV_BUF];
    ssize_t n = recv(cfd, buf, sizeof(buf)-1, 0);
    if (n <= 0) return;
    buf[n] = '\0'; trim_crlf(buf);

    char *f[8]={0};
    int k = split_fields(buf, f, 8);
    if (k < 1 || !f[0]) { send_str(cfd, "ERR comando\n"); return; }

    if      (!strcasecmp(f[0],"ADD"))    handle_ADD(cfd, csv_path, idx_path, namedir, f, k);
    else if (!strcasecmp(f[0],"SEARCH")) handle_SEARCH(cfd, csv_path, namedir, f, k);
    else                                 send_str(cfd, "ERR comando no soportado\n");
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

    int yes = 1; setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

    struct sockaddr_in a; memset(&a,0,sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons(port); a.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(sfd, (struct sockaddr*)&a, sizeof a) < 0) { perror("bind"); close(sfd); return 1; }
    if (listen(sfd, 16) < 0) { perror("listen"); close(sfd); return 1; }

    fprintf(stderr,"track_server escuchando en puerto %d (CSV=%s IDX=%s NAMEIDX=%s)\n",
            port, csv_path, idx_path, namedir);

    for (;;) {
        int cfd = accept(sfd, NULL, NULL);
        if (cfd < 0) { if (errno == EINTR) continue; perror("accept"); break; }
        handle_client(cfd, csv_path, idx_path, namedir);
        close(cfd);
    }
    close(sfd);
    return 0;
}
