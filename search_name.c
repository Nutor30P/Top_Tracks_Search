// search_name.c
// Consulta el índice "nameidx" (track_name + artist) por 1–3 palabras (AND)
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

#define NBKT 256
#define MAX_SHOW 20

/* ---- normalización básica (minúsculas + quita acentos comunes) ---- */
static void norm_push(char **buf, size_t *len, size_t *cap, char ch){
    if(*len+1>=*cap){ *cap=(*cap?*cap*2:64); *buf=realloc(*buf,*cap); }
    (*buf)[(*len)++]=ch;
}
static char *normalize_utf8_basic(const char *s){
    char *out=NULL; size_t L=0,C=0;
    for(const unsigned char *p=(const unsigned char*)s; *p; ){
        if (*p < 0x80){
            char c=(char)tolower(*p++); norm_push(&out,&L,&C,c);
        } else if (p[0]==0xC3 && p[1]){
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
            else   { p+=2; }
        } else { p++; }
    }
    norm_push(&out,&L,&C,'\0');
    return out?out:strdup("");
}

/* ---- tokenizar y unique ---- */
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

/* ---- hash FNV-1a 64 ---- */
static uint64_t fnv1a64(const char *s){
    const uint64_t OFF=1469598103934665603ULL, PR=1099511628211ULL;
    uint64_t h=OFF;
    for(const unsigned char *p=(const unsigned char*)s; *p; ++p){ h^=(uint64_t)*p; h*=PR; }
    if (h==0) { h=1; }
    return h;
}

/* ---- cargar postings de un token ---- */
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

/* ---- intersección AND de dos listas ordenadas ---- */
static uint64_t *intersect(const uint64_t *a,size_t na,const uint64_t *b,size_t nb,size_t *nc){
    size_t i=0,j=0; size_t cap=(na<nb?na:nb), n=0;
    uint64_t *c=malloc(cap?cap:1*sizeof(uint64_t));
    while(i<na && j<nb){
        if (a[i]==b[j]){ c[n++]=a[i]; i++; j++; }
        else if (a[i]<b[j]) i++; else j++;
    }
    *nc=n; return c;
}

int main(int argc, char **argv){
    if (argc < 4){
        fprintf(stderr,"Uso: %s <dataset.csv> <dir_idx> <pal1> [pal2] [pal3]\n", argv[0]);
        return 1;
    }
    const char *csv=argv[1], *dir=argv[2];

    /* normalizar/ tokenizar argumentos de consulta */
    char **qargv=&argv[3]; int qn=argc-3; if(qn>3) qn=3;

    uint64_t *post=NULL; size_t pn=0;
    for(int qi=0; qi<qn; ++qi){
        char *norm=normalize_utf8_basic(qargv[qi]);
        char **toks=NULL; size_t ntok=tokenize_unique(norm,&toks);
        free(norm);
        if (ntok==0){ free(toks); continue; }

        /* Usamos el primer token del argumento */
        uint64_t h = fnv1a64(toks[0]);
        for(size_t k=0;k<ntok;k++){ free(toks[k]); }
        free(toks);

        size_t tn=0; uint64_t *tp=load_postings(dir,h,&tn);
        if (qi==0){ post=tp; pn=tn; }
        else {
            size_t cn=0; uint64_t *cp=intersect(post,pn,tp,tn,&cn);
            free(post); free(tp); post=cp; pn=cn;
        }
        if (pn==0) break;
    }

    if (pn==0 || !post){ printf("NOT_FOUND\n"); return 0; }

    /* abrir CSV y devolver hasta MAX_SHOW líneas */
    FILE *fp=fopen(csv,"r");
    if (!fp){ fprintf(stderr,"CSV: %s\n", strerror(errno)); free(post); return 1; }
    setvbuf(fp,NULL,_IOFBF,4*1024*1024);

    size_t shown=0;
    for(size_t i=0;i<pn && shown<MAX_SHOW;i++){
        if (fseeko(fp,(off_t)post[i],SEEK_SET)!=0) continue;
        char *line=NULL; size_t cap=0; ssize_t len=getline(&line,&cap,fp);
        if (len>0){ fwrite(line,1,(size_t)len,stdout); shown++; }
        free(line);
    }
    if (shown==0) printf("NOT_FOUND\n");

    fclose(fp);
    free(post);
    return 0;
}
