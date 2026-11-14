#define _DEFAULT_SOURCE
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
int main(void) {
 int fd=open("/dev/mem",O_RDONLY|O_SYNC);
 if(fd<0){perror("open");return 1;}
 void *map=mmap(NULL,4096,PROT_READ,MAP_SHARED,fd,0x0300a000);
 if(map==MAP_FAILED){perror("mmap");close(fd);return 1;}
 volatile const uint32_t *regs=map;
 unsigned hist[2][256]={{0}};uint32_t first=regs[11],last=first;
 struct timespec start,end;clock_gettime(CLOCK_MONOTONIC,&start);
 for(unsigned i=0;i<10000;i++) {
  last=regs[11];hist[0][(last>>16)&255]++;hist[1][last>>24]++;usleep(97);
 }
 clock_gettime(CLOCK_MONOTONIC,&end);
 printf("ctrl0=0x%08x ctrl1=0x%08x status=0x%08x ana3_first=0x%08x ana3_last=0x%08x elapsed_s=%.3f\n",regs[0],regs[1],regs[2],first,last,(end.tv_sec-start.tv_sec)+(end.tv_nsec-start.tv_nsec)/1e9);
 for(unsigned ch=0;ch<2;ch++) {
  unsigned pop[9]={0};printf("channel%u DEM_values:",ch);
  for(unsigned v=0;v<256;v++)if(hist[ch][v]){printf(" %02x:%u",v,hist[ch][v]);pop[__builtin_popcount(v)]+=hist[ch][v];}
  printf("\nchannel%u popcount_hist:",ch);for(unsigned i=0;i<9;i++)printf(" %u:%u",i,pop[i]);putchar('\n');
 }
 munmap(map,4096);close(fd);return 0;
}
