#include <errno.h>
#include <fcntl.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

int main(void)
{
    const uint16_t regs[] = {0x03f0,0x03f1,0x003e,0x0d03,0x0d04,
        0x0dc1,0x00d0,0x00b8,0x00b9,0x00b1,0x00b2,0x0d41,0x0d42};
    int fd = open("/dev/i2c-3", O_RDWR | O_CLOEXEC);
    if (fd < 0) { perror("open i2c-3"); return 1; }
    int failed = 0;
    for (int i = 0; i < 2; ++i) {
        unsigned int addr = i ? 0x3f : 0x37;
        printf("%s addr=0x%02x\n", i ? "RGB" : "IR", addr);
        for (unsigned int j = 0; j < sizeof(regs)/sizeof(regs[0]); ++j) {
            uint8_t ptr[] = {regs[j] >> 8, regs[j] & 255}, value = 0xff;
            struct i2c_msg msg[] = {{.addr=addr,.len=2,.buf=ptr},
                {.addr=addr,.flags=I2C_M_RD,.len=1,.buf=&value}};
            struct i2c_rdwr_ioctl_data transfer = {.msgs=msg,.nmsgs=2};
            if (ioctl(fd, I2C_RDWR, &transfer) != 2) {
                fprintf(stderr,"reg %04x: %s\n", regs[j],strerror(errno));
                failed = 1;
            } else printf("  %04x=%02x\n", regs[j],value);
        }
    }
    close(fd);
    return failed;
}
