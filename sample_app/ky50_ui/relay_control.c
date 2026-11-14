#include "relay_control.h"
#include "app_config.h"
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
static int relay_fd=-1;
int relay_control_pulse(int seconds)
{
    if(seconds<1||seconds>APP_MAX_RELAY_SECONDS){errno=EINVAL;return -1;}
    if(relay_fd<0)relay_fd=open("/dev/ky50-relay",O_WRONLY|O_CLOEXEC);
    if(relay_fd<0){perror("relay open");return -1;}
    char command[16];int length=snprintf(command,sizeof(command),"P %d\n",seconds);
    ssize_t n;
    do {n=write(relay_fd,command,length);}while(n<0&&errno==EINTR);
    if(n!=length){perror("relay pulse");return -1;}
    return 0;
}
void relay_control_stop(void)
{
    if(relay_fd>=0){close(relay_fd);relay_fd=-1;}
}
