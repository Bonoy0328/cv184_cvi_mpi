/* Read only a documented candidate's info registers; never scan/write config. */
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
int main(int argc, char **argv)
{
    unsigned r = argc > 2 ? strtoul(argv[2], NULL, 0) : 0;
    unsigned char reg[2] = {r >> 8, r & 255};
    unsigned char data[80] = {0};
    unsigned n = argc > 3 ? strtoul(argv[3], NULL, 0) : 16;
    if (!n || n > sizeof(data)) return 2;
    int fd = open(argc > 1 ? argv[1] : "/dev/i2c-1", O_RDWR);
    if (fd < 0) { perror("open"); return 1; }
    struct i2c_msg msgs[] = {{.addr=0x55,.len=2,.buf=reg},
                            {.addr=0x55,.flags=I2C_M_RD,.len=n,.buf=data}};
    struct i2c_rdwr_ioctl_data x = {.msgs=msgs,.nmsgs=2};
    if (ioctl(fd,I2C_RDWR,&x) != 2) { perror("I2C_RDWR"); close(fd); return 1; }
    for (unsigned i=0;i<n;i++) printf("%02x%s",data[i], (i+1)%16 ? " " : "\n");
    puts(""); close(fd); return 0;
}
