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
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

typedef struct {
    char     magic[8];
    uint64_t capacity;
    uint32_t key_col;
    uint32_t version;
    uint64_t reserved[3];
} __attribute__((packed)) IdxHeader;

typedef struct { uint64_t hash, offset; } __attribute__((packed)) Slot;

/* --- helpers --- */
#define MAXF 256
static size_t parse_csv_line(const char *line, char **out, size_t max_fields){
    size_t n=0, L=strlen(line), bi=0; int inq=0;
    char *buf=(char*)malloc(L+1); if(!buf) return 0;
    for(size_t i=0;i<L;i++){
        char c=line[i];
        if(c=='"'){ if(inq && i+1<L && line[i+1]=='"'){ buf[bi++]='"'; i++; } else inq=!inq; }
        else if(c==',' && !inq){ buf[bi]='\0'; if(n<max_fields) out[n++]=strdup(buf); bi=0; }
        else if(c=='\r' || c=='\n'){ /* ignore */ }
        else buf[bi++]=c;
    }
    buf[bi]='\0'; if(n<max_fields) out[n++]=strdup(buf);
    free(buf); return n;
}
static void free_fields(char **f, size_t n){ for(size_t i=0;i<n;i++) free(f[i]); }

static uint64_t fnv1a64(const char *s){
    const uint64_t OFF=1469598103934665603ULL, PR=1099511628211ULL;
    uint64_t h=OFF; for(const unsigned char *p=(const unsigned char*)s; *p; ++p){ h^=(uint64_t)*p; h*=PR; }
    if(h==0){ h=1; } return h;
}
static int ensure_fifo(const char *path, mode_t mode){
    struct stat st;
    if (lstat(path, &st) == 0) { if (!S_ISFIFO(st.st_mode)) { fprintf(stderr,"No FIFO: %s\n", path); return -1; } return 0; }
    if (mkfifo(path, mode) != 0){ perror("mkfifo"); return -1; }
    return 0;
}
static int write_full(int fd, const char *buf, size_t len){
    size_t off=0; while(off<len){
        ssize_t n = write(fd, buf+off, len-off);
        if (n < 0){ if (errno==EINTR) continue; return -1; }
        if (n == 0) break;
        off += (size_t)n;
    }
    return (off==len)?0:-1;
}

/* --- main --- */
int main(int argc, char **argv){
    if (argc < 4){ fprintf(stderr,"Uso: %s <dataset.csv> <tracks.idx> <fifo_req>\n", argv[0]); return 1; }
    const char *csv_path=argv[1], *idx_path=argv[2], *req_fifo=argv[3];

    int ifd = open(idx_path, O_RDONLY);
    if (ifd < 0){ fprintf(stderr,"Índice: %s\n", strerror(errno)); return 1; }
    off_t isz = lseek(ifd, 0, SEEK_END);
    void *imap = mmap(NULL, isz, PROT_READ, MAP_SHARED, ifd, 0);
    if (imap == MAP_FAILED){ fprintf(stderr,"mmap índice: %s\n", strerror(errno)); close(ifd); return 1; }

    IdxHeader *ih = (IdxHeader*)imap;
    if (strncmp(ih->magic,"IDX1TRK",7)!=0){ fprintf(stderr,"Índice inválido.\n"); munmap(imap,isz); close(ifd); return 1; }
    uint64_t cap = ih->capacity; uint32_t key_col = ih->key_col;
    Slot *slots = (Slot*)((char*)imap + sizeof(IdxHeader));

    FILE *fp = fopen(csv_path,"r");
    if (!fp){ fprintf(stderr,"CSV: %s\n", strerror(errno)); munmap(imap,isz); close(ifd); return 1; }
    setvbuf(fp,NULL,_IOFBF,4*1024*1024);

    if (ensure_fifo(req_fifo,0666)!=0){ fclose(fp); munmap(imap,isz); close(ifd); return 1; }
    fprintf(stderr,"Servidor listo en %s\n", req_fifo);

    char line[4096];
    for(;;){
        int rfd = open(req_fifo, O_RDONLY);
        if (rfd < 0){ perror("open req_fifo"); break; }
        FILE *r = fdopen(rfd, "r"); if(!r){ perror("fdopen"); close(rfd); break; }

        while (fgets(line,sizeof(line),r)){
            char *cmd=strtok(line," \t\r\n");
            if(!cmd) continue;
            if (strcasecmp(cmd,"QUIT")==0){ fclose(r); close(rfd); goto out; }
            if (strcasecmp(cmd,"GET_ID")!=0){ fprintf(stderr,"Comando inválido\n"); continue; }
            char *key=strtok(NULL," \t\r\n");
            char *resp_fifo=strtok(NULL," \t\r\n");
            if(!key || !resp_fifo){ fprintf(stderr,"Solicitud incompleta\n"); continue; }

            uint64_t hv = fnv1a64(key);
            uint64_t i = hv & (cap - 1), start=i;
            int found=0;

            for(;;){
                Slot s = slots[i];
                if (s.hash==0) break;
                if (s.hash==hv){
                    if (fseeko(fp,(off_t)s.offset,SEEK_SET)==0){
                        char *csvline=NULL; size_t bufc=0; ssize_t len=getline(&csvline,&bufc,fp);
                        if (len>0){
                            char *f[MAXF]={0};
                            size_t nx=parse_csv_line(csvline,f,MAXF);
                            if (nx>key_col && f[key_col] && strcmp(f[key_col],key)==0){
                                int wfd=open(resp_fifo,O_WRONLY);
                                if (wfd>=0){ (void)write_full(wfd,csvline,(size_t)len); close(wfd); }
                                found=1;
                            }
                            free_fields(f,nx);
                        }
                        free(csvline);
                    }
                    if (found) break;
                }
                i=(i+1)&(cap-1);
                if (i==start) break;
            }
            if (!found){
                int wfd=open(resp_fifo,O_WRONLY);
                if (wfd>=0){ (void)write_full(wfd,"NOT_FOUND\n",10); close(wfd); }
            }
        }
        fclose(r); close(rfd);
    }

out:
    fclose(fp);
    munmap(imap,isz);
    close(ifd);
    return 0;
}
