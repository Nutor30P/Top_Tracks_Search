/*
  p1-dataProgram.c
  Menú interactivo:
    1) Ingresar primer criterio de búsqueda (track_id exacto)
    2) Ingresar segundo criterio (palabra para nombre/artista)
    3) Ingresar tercer criterio (opcional, palabra extra para AND)
    4) Realizar búsqueda
    5) Salir

  Requisitos previos:
    - Índice por ID: tracks.idx
    - Índice por texto: directorio nameidx/ (generado por build_name_index)

  Salida compacta: track_id | track_name | artist | date | region
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
#include <ctype.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>

/* ---------- Constantes ---------- */
#define NBKT 256
#define MAX_SHOW 20

/* ---------- Estructuras del índice por ID ---------- */
typedef struct {
    char     magic[8];
    uint64_t capacity;   // número de slots (potencia de 2)
    uint32_t key_col;    // columna de track_id en el CSV
    uint32_t version;
    uint64_t reserved[3];
} __attribute__((packed)) IdxHeader;

typedef struct { uint64_t hash, offset; } __attribute__((packed)) Slot;

/* ---------- CSV (parser que respeta comillas) ---------- */
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
            /* ignore */
        } else {
            buf[bi++]=c;
        }
    }
    buf[bi]='\0'; if(n<max_fields) out[n++]=strdup(buf);
    free(buf); return n;
}
static void free_fields(char **f, size_t n){ for(size_t i=0;i<n;i++) free(f[i]); }

/* ---------- Normalizador/Tokenizador (para búsquedas por texto) ---------- */
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
                case 0xA1: case 0x81: m='a'; break; // á/Á
                case 0xA9: case 0x89: m='e'; break; // é/É
                case 0xAD: case 0x8D: m='i'; break; // í/Í
                case 0xB3: case 0x93: m='o'; break; // ó/Ó
                case 0xBA: case 0x9A: m='u'; break; // ú/Ú
                case 0xBC: case 0x9C: m='u'; break; // ü/Ü
                case 0xB1: case 0x91: m='n'; break; // ñ/Ñ
                default: m=0; break;
            }
            if (m){ norm_push(&out,&L,&C,m); p+=2; } else { p+=2; }
        } else { p++; }
    }
    norm_push(&out,&L,&C,'\0'); return out?out:strdup("");
}
static int cmp_strptr(const void *a, const void *b){
    const char *const *pa=a, *const *pb=b; return strcmp(*pa,*pb);
}
static size_t tokenize_unique(const char *norm, char ***out_tokens){
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
    qsort(tok,n,sizeof(char*),cmp_strptr);
    size_t m=0;
    for(size_t k=0;k<n;k++){
        if (m==0 || strcmp(tok[k], tok[m-1])!=0) tok[m++]=tok[k];
        else free(tok[k]);
    }
    *out_tokens=tok; return m;
}

/* ---------- Hash FNV-1a 64 ---------- */
static uint64_t fnv1a64(const char *s){
    const uint64_t OFF=1469598103934665603ULL, PR=1099511628211ULL;
    uint64_t h=OFF;
    for(const unsigned char *p=(const unsigned char*)s; *p; ++p){ h^=(uint64_t)*p; h*=PR; }
    if (h==0) { h=1; }
    return h;
}

/* ---------- Lectura de postings en nameidx ---------- */
static uint64_t *load_postings(const char *dir, uint64_t h, size_t *out_n){
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
            if (fseeko(f, (off_t)df*8, SEEK_CUR)!=0) break;
        }
    }
    fclose(f); *out_n=0; return NULL;
}
static uint64_t *intersect(const uint64_t *a,size_t na,const uint64_t *b,size_t nb,size_t *nc){
    size_t i=0,j=0,cap=(na<nb?na:nb),n=0; uint64_t *c=malloc(cap?cap:1*sizeof(uint64_t));
    while(i<na && j<nb){ if(a[i]==b[j]){ c[n++]=a[i]; i++; j++; } else if(a[i]<b[j]) i++; else j++; }
    *nc=n; return c;
}

/* ---------- Detección de columnas + formateo compacto ---------- */
typedef struct { int track_id, track_name, artist, date, region; } ColIdx;
static ColIdx gcols = {10, 1, 4, 3, 6};  // defaults por si el header no está

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
static void print_compact_line(const char *line){
    char *f[256]={0}; size_t nx=parse_csv_line(line,f,256);
    const char *id    = (gcols.track_id   < (int)nx && f[gcols.track_id])   ? f[gcols.track_id]   : "-";
    const char *name  = (gcols.track_name < (int)nx && f[gcols.track_name]) ? f[gcols.track_name] : "-";
    const char *art   = (gcols.artist     < (int)nx && f[gcols.artist])     ? f[gcols.artist]     : "-";
    const char *date  = (gcols.date       < (int)nx && f[gcols.date])       ? f[gcols.date]       : "-";
    const char *reg   = (gcols.region     < (int)nx && f[gcols.region])     ? f[gcols.region]     : "-";
    printf("%s | %s | %s | %s | %s\n", id, name, art, date, reg);
    free_fields(f,nx);
}

/* ---------- Lookup por track_id (usa tracks.idx) ---------- */
static int lookup_by_id(const char *csv, const char *idx, const char *key){
    int fd = open(idx, O_RDONLY);
    if (fd < 0){ fprintf(stderr,"Índice: %s\n", strerror(errno)); return -1; }
    off_t sz = lseek(fd, 0, SEEK_END);
    void *map = mmap(NULL, sz, PROT_READ, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED){ fprintf(stderr,"mmap: %s\n", strerror(errno)); close(fd); return -1; }

    IdxHeader *h = (IdxHeader*)map;
    if (strncmp(h->magic,"IDX1TRK",7)!=0){
        fprintf(stderr,"Índice inválido\n"); munmap(map,sz); close(fd); return -1;
    }
    uint64_t cap=h->capacity; uint32_t key_col=h->key_col;
    Slot *slots = (Slot*)((char*)map + sizeof(IdxHeader));

    FILE *fp=fopen(csv,"r");
    if(!fp){ fprintf(stderr,"CSV: %s\n", strerror(errno)); munmap(map,sz); close(fd); return -1; }
    setvbuf(fp,NULL,_IOFBF,4*1024*1024);

    uint64_t hv=fnv1a64(key), i=hv&(cap-1), start=i;
    char *line=NULL; size_t bufcap=0; ssize_t len;
    int found=0;

    for(;;){
        Slot s=slots[i];
        if(s.hash==0) break;
        if(s.hash==hv){
            if (fseeko(fp,(off_t)s.offset,SEEK_SET)==0){
                len=getline(&line,&bufcap,fp);
                if (len>0){
                    char *f[256]={0};
                    size_t nx=parse_csv_line(line,f,256);
                    if (nx>key_col && f[key_col] && strcmp(f[key_col],key)==0){
                        print_compact_line(line);
                        free_fields(f,nx); found=1; break;
                    }
                    free_fields(f,nx);
                }
            }
        }
        i=(i+1)&(cap-1);
        if (i==start) break;
    }

    free(line); fclose(fp); munmap(map,sz); close(fd);
    if(!found) printf("NOT_FOUND\n");
    return found?0:1;
}

/* ---------- Búsqueda por palabras usando nameidx ---------- */
static int search_by_words(const char *csv, const char *dir, const char **words, int nwords){
    uint64_t *post=NULL; size_t pn=0;
    for(int qi=0; qi<nwords; ++qi){
        char *norm=normalize_utf8_basic(words[qi]);
        char **toks=NULL; size_t ntok=tokenize_unique(norm,&toks);
        free(norm);
        if (ntok==0){ free(toks); continue; }

        uint64_t h = fnv1a64(toks[0]);        /* usamos el 1er token del argumento */
        for (size_t k=0;k<ntok;k++){ free(toks[k]); }
        free(toks);

        size_t tn=0; uint64_t *tp=load_postings(dir,h,&tn);
        if (qi==0){ post=tp; pn=tn; }
        else {
            size_t cn=0; uint64_t *cp=intersect(post,pn,tp,tn,&cn);
            free(post); free(tp); post=cp; pn=cn;
        }
        if (pn==0) break;
    }
    if (pn==0 || !post){ printf("NOT_FOUND\n"); return 1; }

    FILE *fp=fopen(csv,"r");
    if(!fp){ fprintf(stderr,"CSV: %s\n", strerror(errno)); free(post); return -1; }
    setvbuf(fp,NULL,_IOFBF,4*1024*1024);

    size_t shown=0;
    for(size_t i=0;i<pn && shown<MAX_SHOW;i++){
        if (fseeko(fp,(off_t)post[i],SEEK_SET)!=0) continue;
        char *line=NULL; size_t cap=0; ssize_t len=getline(&line,&cap,fp);
        if (len>0){ print_compact_line(line); shown++; }
        free(line);
    }
    if (shown==0) printf("NOT_FOUND\n");
    fclose(fp); free(post);
    return shown?0:1;
}

/* ---------- UI ---------- */
static void chomp(char *s){ size_t n=strlen(s); while(n && (s[n-1]=='\n'||s[n-1]=='\r')) s[--n]=0; }

int main(void){
    char csv[512]="merged_data.csv";
    char idx[512]="tracks.idx";
    char namedir[512]="nameidx";

    printf("Ruta CSV [merged_data.csv]: "); fflush(stdout);
    if (fgets(csv,sizeof(csv),stdin)) { chomp(csv); if(!csv[0]) strcpy(csv,"merged_data.csv"); }
    printf("Ruta índice ID (tracks.idx) [tracks.idx]: "); fflush(stdout);
    if (fgets(idx,sizeof(idx),stdin)) { chomp(idx); if(!idx[0]) strcpy(idx,"tracks.idx"); }
    printf("Directorio índice texto (nameidx) [nameidx]: "); fflush(stdout);
    if (fgets(namedir,sizeof(namedir),stdin)) { chomp(namedir); if(!namedir[0]) strcpy(namedir,"nameidx"); }

    /* Detectar columnas del CSV una sola vez para la salida compacta */
    load_cols_once(csv);

    char id[256]="";
    char w1[128]="", w2[128]="", w3[128]="";

    for(;;){
        printf("\nBienvenido\n");
        printf("1. Ingresar primer criterio de búsqueda (track_id)\n");
        printf("2. Ingresar segundo criterio de búsqueda (palabra nombre/artista)\n");
        printf("3. Ingresar tercer criterio de búsqueda (si aplica)\n");
        printf("4. Realizar búsqueda\n");
        printf("5. Salir\n> ");
        fflush(stdout);

        char opt[16]; if(!fgets(opt,sizeof(opt),stdin)) break;
        int o=atoi(opt);

        if (o==1){
            printf("track_id: "); fflush(stdout);
            if (fgets(id,sizeof(id),stdin)) chomp(id);
        } else if (o==2){
            printf("Palabra #1: "); fflush(stdout);
            if (fgets(w1,sizeof(w1),stdin)) chomp(w1);
        } else if (o==3){
            printf("Palabra #2 (opcional): "); fflush(stdout);
            if (fgets(w2,sizeof(w2),stdin)) chomp(w2);
            printf("Palabra #3 (opcional): "); fflush(stdout);
            if (fgets(w3,sizeof(w3),stdin)) chomp(w3);
        } else if (o==4){
            printf("\n=== Resultados ===\n");
            if (id[0]){                     // criterio 1: por ID
                (void)lookup_by_id(csv, idx, id);
            } else {                         // criterio 2/3: por palabras (AND)
                const char *words[3]; int n=0;
                if (w1[0]) words[n++]=w1;
                if (w2[0]) words[n++]=w2;
                if (w3[0]) words[n++]=w3;
                if (n==0) printf("NOT_FOUND\n");
                else (void)search_by_words(csv, namedir, words, n);
            }
            printf("=== Fin ===\n");
        } else if (o==5){
            break;
        } else {
            printf("Opción inválida.\n");
        }
    }
    return 0;
}
