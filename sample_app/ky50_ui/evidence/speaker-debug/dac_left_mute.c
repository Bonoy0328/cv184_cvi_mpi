/* Diagnostic: use the shipped driver's public left-DAC mute ioctl. */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "codec_ioctl.h"
int main(int argc,char **argv)
{
    if(argc!=2||(strcmp(argv[1],"0")&&strcmp(argv[1],"1"))) {
        fprintf(stderr,"usage: %s 0|1 (left DAC mute)\n",argv[0]);return 2;
    }
    int fd=open("/dev/misc_dac_0",O_RDWR);
    if(fd<0) { perror("open codec");return 1; }
    struct cvi_vol_ctrl before={0},after={0};
    unsigned mute=(unsigned)(argv[1][0]-'0');
    if(ioctl(fd,ACODEC_GET_DACL_VOL,&before)||
       ioctl(fd,ACODEC_SET_DACL_MUTE,&mute)||
       ioctl(fd,ACODEC_GET_DACL_VOL,&after)) {
        perror("left DAC mute ioctl");close(fd);return 1;
    }
    printf("left mute %u -> %u, volume=%u\n",before.vol_ctrl_mute,
           after.vol_ctrl_mute,after.vol_ctrl);
    close(fd);return after.vol_ctrl_mute!=mute;
}
