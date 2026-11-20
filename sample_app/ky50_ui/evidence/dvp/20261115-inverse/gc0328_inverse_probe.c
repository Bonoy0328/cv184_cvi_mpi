/* Temporary board diagnostic: only Page0 0x43 bit0 is changed. */
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
static int rd(int fd,unsigned char addr) {
    unsigned char v=0;
    struct i2c_msg m[]={{.addr=0x21,.len=1,.buf=&addr},
        {.addr=0x21,.flags=I2C_M_RD,.len=1,.buf=&v}};
    struct i2c_rdwr_ioctl_data t={.msgs=m,.nmsgs=2};
    return ioctl(fd,I2C_RDWR,&t)==2?v:-1;
}
static int wr(int fd,unsigned char addr,unsigned char v) {
    unsigned char b[]={addr,v};return write(fd,b,2)==2?0:-1;
}
int main(int argc,char **argv) {
    if(argc!=2||(strcmp(argv[1],"status")&&strcmp(argv[1],"on")&&strcmp(argv[1],"off")))return 2;
    int fd=open("/dev/i2c-2",O_RDWR|O_CLOEXEC),rc=1;
    if(fd<0){perror("open");return 1;}
    if(ioctl(fd,I2C_SLAVE,0x21)){perror("slave");goto done;}
    int id=rd(fd,0xf0),page=rd(fd,0xfe);
    printf("GC0328 chip_id=0x%02x page=0x%02x\n",id,page);
    if(id!=0x9d||page!=0){fprintf(stderr,"Unexpected sensor/page; no write performed\n");goto done;}
    int old=rd(fd,0x43);if(old<0)goto done;
    printf("special_effect_before=0x%02x\n",old);
    if(strcmp(argv[1],"status")) {
        int v=!strcmp(argv[1],"on")?(old|1):(old&~1);
        if(wr(fd,0x43,(unsigned char)v))goto done;
        usleep(150000); /* Allow three frame boundaries for sensor register latch. */
        int after=rd(fd,0x43);printf("special_effect_after=0x%02x inverse=%d\n",after,after&1);
        if(after!=v){wr(fd,0x43,(unsigned char)old);usleep(150000);goto done;}
    }
    rc=0;
done:close(fd);return rc;
}
