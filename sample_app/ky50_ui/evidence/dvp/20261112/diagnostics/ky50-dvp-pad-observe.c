/* Read-only GPIO input observation; sampled edges are NOT a MHz clock measurement. */
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
static uint64_t ns(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000ULL+t.tv_nsec;}
int main(void){
 int fd=open("/dev/mem",O_RDONLY|O_SYNC);if(fd<0){perror("mem");return 1;}
 volatile uint32_t *p=mmap(NULL,4096,PROT_READ,MAP_SHARED,fd,0x03021000);close(fd);
 if(p==MAP_FAILED){perror("map");return 1;}
 uint64_t hi[12]={0},edges[12]={0},count=0,start=ns(),end=start;uint32_t prev=p[0x50/4];
 do{for(unsigned n=0;n<1000;n++){uint32_t v=p[0x50/4],chg=v^prev;prev=v;count++;
 for(unsigned i=0;i<12;i++){hi[i]+=(v>>(11+i))&1;edges[i]+=(chg>>(11+i))&1;}}end=ns();}while(end-start<1000000000ULL);
 printf("samples=%llu duration_ns=%llu\n",(unsigned long long)count,(unsigned long long)(end-start));
 for(unsigned i=0;i<12;i++)printf("GPIOB%u high_pct=%.3f sampled_edges=%llu\n",i+11,100.0*hi[i]/count,(unsigned long long)edges[i]);
 munmap((void *)p,4096);return 0;
}
