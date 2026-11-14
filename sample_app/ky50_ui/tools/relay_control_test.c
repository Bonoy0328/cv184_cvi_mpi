/* Exercise the actual userspace controller without GPIO or sleeps. */
#define open fake_open
#define write fake_write
#define close fake_close
#include "../relay_control.c"
#include <assert.h>
#include <string.h>
static int opens,writes,closes,interrupt_once,fail_open,fail_write;
static char sent[32];
int fake_open(const char *path,int flags,...)
{
    assert(!strcmp(path,"/dev/ky50-relay")&&(flags&O_CLOEXEC));++opens;
    if(fail_open){errno=ENOENT;return -1;}return 17;
}
ssize_t fake_write(int fd,const void *buffer,size_t n)
{
    assert(fd==17&&n<sizeof(sent));++writes;
    if(interrupt_once){interrupt_once=0;errno=EINTR;return -1;}
    if(fail_write){errno=EIO;return -1;}
    memcpy(sent,buffer,n);sent[n]=0;return n;
}
int fake_close(int fd){assert(fd==17);++closes;return 0;}
int main(void)
{
    assert(relay_control_pulse(0)<0&&relay_control_pulse(61)<0&&!opens);
    fail_open=1;assert(relay_control_pulse(5)<0);fail_open=0;
    interrupt_once=1;assert(!relay_control_pulse(5)&&!strcmp(sent,"P 5\n")&&writes==2);
    assert(!relay_control_pulse(2)&&!strcmp(sent,"P 2\n")&&opens==2); /* Retrigger same fd. */
    assert(!relay_control_pulse(60)&&!strcmp(sent,"P 60\n"));
    fail_write=1;assert(relay_control_pulse(3)<0);fail_write=0;
    relay_control_stop();relay_control_stop();assert(closes==1);
    assert(!relay_control_pulse(1)&&opens==3);relay_control_stop();
    puts("PASS relay duration bounds, protocol, retrigger, EINTR, errors and shutdown");
}
