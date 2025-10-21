#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

static int ensure_fifo(const char *path, mode_t mode){
    if (mkfifo(path, mode) != 0){ if (errno==EEXIST) return 0; perror("mkfifo"); return -1; }
    return 0;
}
static int write_full(int fd, const char *buf, size_t len){
    size_t off=0; while(off<len){
        ssize_t n=write(fd, buf+off, len-off);
        if(n<0){ if(errno==EINTR) continue; return -1; }
        if(n==0) break; off+=(size_t)n;
    } return (off==len)?0:-1;
}

int main(int argc, char **argv){
    if (argc < 3){ fprintf(stderr,"Uso: %s <fifo_req> <track_id>\n", argv[0]); return 1; }
    const char *req_fifo=argv[1], *track_id=argv[2];

    char resp_fifo[256];
    snprintf(resp_fifo,sizeof(resp_fifo),"/tmp/track.res.%ld",(long)getpid());
    if (ensure_fifo(resp_fifo,0600)!=0) return 1;

    int wfd=open(req_fifo,O_WRONLY);
    if (wfd<0){ fprintf(stderr,"No se puede abrir %s: %s\n", req_fifo, strerror(errno)); unlink(resp_fifo); return 1; }
    char line[512];
    snprintf(line,sizeof(line),"GET_ID %s %s\n", track_id, resp_fifo);
    (void)write_full(wfd,line,strlen(line));
    close(wfd);

    int rfd=open(resp_fifo,O_RDONLY);
    if (rfd<0){ fprintf(stderr,"No se puede abrir %s: %s\n", resp_fifo, strerror(errno)); unlink(resp_fifo); return 1; }
    char buf[4096]; ssize_t n;
    while ((n=read(rfd,buf,sizeof(buf)))>0){ (void)write_full(STDOUT_FILENO, buf, (size_t)n); }
    close(rfd);
    unlink(resp_fifo);
    return 0;
}
