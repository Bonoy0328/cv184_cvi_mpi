#define _DEFAULT_SOURCE
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
static uint64_t times[10000];
static uint32_t values[10000];
int main(void) {
 int fd=open("/dev/mem",O_RDONLY|O_SYNC);
 if(fd<0){perror("open");return 1;}
 void *map=mmap(NULL,4096,PROT_READ,MAP_SHARED,fd,0x0300a000);
 if(map==MAP_FAILED){perror("mmap");close(fd);return 1;}
 volatile const uint32_t *regs=map;
 for(unsigned i=0;i<10000;i++) {
  struct timespec t;values[i]=regs[11];clock_gettime(CLOCK_MONOTONIC,&t);
  times[i]=(uint64_t)t.tv_sec*1000000000ULL+t.tv_nsec;usleep(97);
 }
 munmap(map,4096);close(fd);
 puts("time_ns,ana3,left_popcount,right_popcount");
 for(unsigned i=0;i<10000;i++)printf("%llu,0x%08x,%u,%u\n",(unsigned long long)times[i],values[i],__builtin_popcount((values[i]>>16)&255),__builtin_popcount(values[i]>>24));
 return 0;
}
