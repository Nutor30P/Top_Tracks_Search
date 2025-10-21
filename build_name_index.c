// build_name_index.c
// Índice invertido por tokens de track_name + artist  -> offsets (CSV)
// Salida: 256 buckets nameidx/b00.idx ... nameidx/bff.idx

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
#include <sys/stat.h>
#include <unistd.h>

#define MAXF 256
#define NBKT 256        // 256 buckets -> b00..bff

typedef struct { uint64_t h, off; } Pair;

/* ---------- Prototipos ---------- */
static size_t   parse_csv_line(const char *line, char **out, size_t max_fields);
static void     free_fields(char **f, size_t n);
static int      find_col(char **hdr, size_t n, const char *name);
static char    *normalize_utf8_basic(const char *s);
static size_t   tokenize_unique(const char *norm, char ***out_tokens);
static uint64_t fnv1a64(const char *s);
static int      ensure_dir(const char *path);
static int      cmp_strptr(const void *a, const void *b);
static int      cmp_pair(const void *a, const void *b);

/* ---------- CSV (respeta comillas) ---------- */
static size_t parse_csv_line(const char *line, char **out, size_t max_fields){
    size_t n=0, L=strlen(line), bi=0; int inq=0;
    char *buf=(char*)malloc(L+1); if(!buf) return 0;
    for(size_t i=0;i<L;i++){
        char c=line[i];
        if(c=='"'){
            if(inq && i+1<L && line[i+1]=='"'){ buf[bi++]='"'; i++; }
            else { inq=!inq; }
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
static int find_col(char **hdr, size_t n, const char *name){
    for(size_t i=0;i<n;i++) if(hdr[i] && strcasecmp(hdr[i],name)==0) return (int)i;
    return -1;
}

/* ---------- Normalización básica ---------- */
static void norm_push(char **buf, size_t *len, size_t *cap, char ch){
    if(*len+1>=*cap){ *cap=(*cap?*cap*2:64); *buf=realloc(*buf,*cap); }
    (*buf)[(*len)++]=ch;
}
static char *normalize_utf8_basic(const char *s){
    char *out=NULL; size_t L=0,C=0;
    for(const unsigned char *p=(const unsigned char*)s; *p; ){
        if (*p < 0x80){                      // ASCII
            char c=(char)tolower(*p++);
            norm_push(&out,&L,&C,c);
        } else if (p[0]==0xC3 && p[1]){      // áéíóúüñ (y mayúsculas)
            unsigned char c2=p[1]; char m=0;
            switch(c2){
                case 0xA1: case 0x81: m='a'; break; // á Á
                case 0xA9: case 0x89: m='e'; break; // é É
                case 0xAD: case 0x8D: m='i'; break; // í Í
                case 0xB3: case 0x93: m='o'; break; // ó Ó
                case 0xBA: case 0x9A: m='u'; break; // ú Ú
                case 0xBC: case 0x9C: m='u'; break; // ü Ü
                case 0xB1: case 0x91: m='n'; break; // ñ Ñ
                default: m=0; break;
            }
            if (m){ norm_push(&out,&L,&C,m); p+=2; }
            else   { p+=2; }                 // omite otros acentos
        } else {
            p++;                              // omite demás multibyte
        }
    }
    norm_push(&out,&L,&C,'\0');
    return out?out:strdup("");
}

/* ---------- Tokenización y unique por línea ---------- */
static int cmp_strptr(const void *a, const void *b){
    const char *const *pa=a, *const *pb=b;
    return strcmp(*pa, *pb);
}
static size_t tokenize_unique(const char *norm, char ***out_tokens){
    size_t cap=16,n=0; char **tok=malloc(cap*sizeof(char*));
    size_t i=0,L=strlen(norm);
    while(i<L){
        while(i<L && !isalnum((unsigned char)norm[i])) i++;
        if(i>=L) break;
        size_t j=i; while(j<L && isalnum((unsigned char)norm[j])) j++;
        if(n==cap){ cap*=2; tok=realloc(tok,cap*sizeof(char*)); }
        tok[n++]=strndup(norm+i,j-i);
        i=j;
    }
    qsort(tok,n,sizeof(char*),cmp_strptr);      // ordenar
    size_t m=0;                                 // unique
    for(size_t k=0;k<n;k++){
        if (m==0 || strcmp(tok[k], tok[m-1])!=0) tok[m++]=tok[k];
        else free(tok[k]);
    }
    *out_tokens=tok; return m;
}

/* ---------- Hash ---------- */
static uint64_t fnv1a64(const char *s){
    const uint64_t OFF=1469598103934665603ULL, PR=1099511628211ULL;
    uint64_t h=OFF;
    for(const unsigned char *p=(const unsigned char*)s; *p; ++p){
        h ^= (uint64_t)*p;
        h *= PR;
    }
    if(h==0){ h=1; }
    return h;
}

/* ---------- FS helpers ---------- */
static int ensure_dir(const char *path){
    struct stat st;
    if (stat(path,&st)==0){ if(S_ISDIR(st.st_mode)) return 0; errno=ENOTDIR; return -1; }
    return mkdir(path, 0775);
}

/* ---------- Comparador de Pair para qsort ---------- */
static int cmp_pair(const void *a, const void *b){
    const Pair *x = (const Pair*)a, *y = (const Pair*)b;
    if (x->h < y->h) return -1;
    if (x->h > y->h) return  1;
    if (x->off < y->off) return -1;
    if (x->off > y->off) return  1;
    return 0;
}

/* ---------- main ---------- */
int main(int argc, char **argv){
    if (argc<3){ fprintf(stderr,"Uso: %s <dataset.csv> <dir_idx>\n", argv[0]); return 1; }
    const char *csv=argv[1], *dir=argv[2];
    if (ensure_dir(dir)!=0 && errno!=EEXIST){ perror("mkdir dir_idx"); return 1; }

    // Abrir 256 archivos temporales
    FILE *bkt[NBKT]={0};
    char path[512];
    for(int b=0;b<NBKT;b++){
        snprintf(path,sizeof(path),"%s/b%02x.tmp",dir,b);
        bkt[b]=fopen(path,"wb");
        if(!bkt[b]){ fprintf(stderr,"No puedo crear %s: %s\n", path, strerror(errno)); return 1; }
    }

    FILE *fp=fopen(csv,"r");
    if(!fp){ fprintf(stderr,"CSV: %s\n", strerror(errno)); return 1; }
    setvbuf(fp,NULL,_IOFBF,4*1024*1024);

    // Leer encabezado y detectar columnas
    char *line=NULL; size_t bufcap=0; ssize_t len=getline(&line,&bufcap,fp);
    if(len<=0){ fprintf(stderr,"CSV vacío\n"); fclose(fp); free(line); return 1; }

    char *hdr[MAXF]={0}; size_t nf=parse_csv_line(line,hdr,MAXF);
    int col_name   = find_col(hdr,nf,"track_name");
    int col_artist = find_col(hdr,nf,"artist");
    if (col_name   < 0) col_name = 1; // por tu CSV
    if (col_artist < 0) col_artist = 4;
    fprintf(stderr,"Usando columnas: track_name=%d, artist=%d\n", col_name, col_artist);
    free_fields(hdr,nf);

    // Recorrer filas
    uint64_t rows=0;
    for(;;){
        off_t off=ftello(fp);
        len=getline(&line,&bufcap,fp);
        if(len<=0) break;

        char *f[MAXF]={0}; size_t nx=parse_csv_line(line,f,MAXF);
        if ((int)nx>col_name || (int)nx>col_artist){
            char *norm_name   = (col_name   < (int)nx && f[col_name])   ? normalize_utf8_basic(f[col_name])   : strdup("");
            char *norm_artist = (col_artist < (int)nx && f[col_artist]) ? normalize_utf8_basic(f[col_artist]) : strdup("");

            size_t combo_len = strlen(norm_name) + 1 + strlen(norm_artist) + 1;
            char *combo = (char*)malloc(combo_len);
            snprintf(combo, combo_len, "%s %s", norm_name, norm_artist);

            char **tokens=NULL; size_t ntok=tokenize_unique(combo,&tokens);
            for(size_t t=0;t<ntok;t++){
                uint64_t h=fnv1a64(tokens[t]);
                int b=(int)(h & (NBKT-1));
                fwrite(&h,   sizeof(uint64_t), 1, bkt[b]);
                fwrite(&off, sizeof(uint64_t), 1, bkt[b]);
            }

            for(size_t t=0;t<ntok;t++) free(tokens[t]);
            free(tokens);
            free(combo); free(norm_name); free(norm_artist);
        }
        free_fields(f,nx);

        rows++;
        if ((rows%1000000ULL)==0) fprintf(stderr,"Filas procesadas: %llu\n",(unsigned long long)rows);
    }
    free(line); fclose(fp);
    for(int b=0;b<NBKT;b++) fclose(bkt[b]);

    // Compactar cada bucket: ordenar y agrupar offsets
    for(int b=0;b<NBKT;b++){
        char tin[512], tout[512];
        snprintf(tin, sizeof(tin),  "%s/b%02x.tmp", dir, b);
        snprintf(tout,sizeof(tout), "%s/b%02x.idx", dir, b);

        FILE *fi=fopen(tin,"rb");
        if(!fi){ continue; } // bucket vacío
        if (fseeko(fi,0,SEEK_END)!=0){ fclose(fi); continue; }
        off_t sz=ftello(fi); fseeko(fi,0,SEEK_SET);
        size_t n=(size_t)(sz/sizeof(Pair));
        if (n==0){ fclose(fi); unlink(tin); continue; }

        Pair *arr=malloc(n*sizeof(Pair));
        if(!arr){ fprintf(stderr,"Memoria insuficiente en bucket %02x\n", b); fclose(fi); return 1; }
        for(size_t i=0;i<n;i++){
            if (fread(&arr[i].h,8,1,fi)!=1 || fread(&arr[i].off,8,1,fi)!=1){
                fprintf(stderr,"Lectura incompleta en %s\n", tin);
                free(arr); fclose(fi); return 1;
            }
        }
        fclose(fi);

        qsort(arr,n,sizeof(Pair),cmp_pair);

        FILE *fo=fopen(tout,"wb");
        if(!fo){ fprintf(stderr,"No puedo crear %s: %s\n", tout, strerror(errno)); free(arr); return 1; }

        size_t i=0;
        while(i<n){
            uint64_t h=arr[i].h;
            // compactar offsets duplicados
            size_t j=i; uint32_t df=0;
            size_t w=i; uint64_t last=~(uint64_t)0;
            while(j<n && arr[j].h==h){
                if (arr[j].off!=last){ arr[w++]=arr[j]; last=arr[j].off; df++; }
                j++;
            }
            // escribir bloque: [hash][df][pad][df * offsets]
            fwrite(&h, 8, 1, fo);
            fwrite(&df,4, 1, fo);
            uint32_t pad=0; fwrite(&pad,4,1,fo);
            for(size_t k=i;k<i+df;k++) fwrite(&arr[k].off,8,1,fo);

            i=j;
        }
        fclose(fo);
        free(arr);
        unlink(tin); // borrar tmp
        fprintf(stderr,"Bucket %02x listo -> %s\n", b, tout);
    }

    fprintf(stderr,"Índice de nombres/artistas listo en %s/\n", dir);
    return 0;
}
