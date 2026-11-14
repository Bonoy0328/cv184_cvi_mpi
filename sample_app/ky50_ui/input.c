#include "input.h"
#include "ui.h"
#include <linux/input.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/wait.h>
static int x,y,down,dropped,monotonic_events;
static uint64_t pending_ms;
uint64_t touch_pending_ms(void) { return pending_ms; }
void touch_presented(void) { pending_ms=0; }
int touch_prepare(void)
{
    if(!access("/sys/module/ky50_touch",F_OK)) return 0;
    pid_t pid=fork();
    if(pid<0) return -1;
    if(!pid) { execl("/sbin/insmod","insmod","/tmp/ky50-ui/ky50_touch.ko",(char *)NULL);_exit(127); }
    int status;
    while(waitpid(pid,&status,0)<0) if(errno!=EINTR) return -1;
    return WIFEXITED(status)&&WEXITSTATUS(status)==0?0:-1;
}
int touch_open(const char *path)
{
    for(int i=0;i<32;i++) {
        char p[64],name[128]={0};
        snprintf(p,sizeof(p),"/dev/input/event%d",i);
        int fd=open(path?path:p,O_RDONLY|O_NONBLOCK|O_CLOEXEC);
        if(fd>=0) {
            if(ioctl(fd,EVIOCGNAME(sizeof(name)),name)>=0&&!strcmp(name,"KY50 ST7102 Touchscreen")) {
                int clock_id=CLOCK_MONOTONIC;
                monotonic_events=ioctl(fd,EVIOCSCLOCKID,&clock_id)==0;
                printf("TOUCH %s: %s\n",path?path:p,name);return fd;
            }
            close(fd);
        }
        if(path) break;
    }
    fprintf(stderr,"KY50 touch input missing; load kernel/ky50_touch.ko first\n");return -1;
}
int touch_poll(int fd)
{
    struct input_event e;
    ssize_t n;
    while((n=read(fd,&e,sizeof(e)))==(ssize_t)sizeof(e)) {
        if(e.type==EV_SYN&&e.code==SYN_DROPPED) {
            dropped=1;down=0;ui_pointer(x,y,0);continue;
        }
        if(dropped&&!(e.type==EV_SYN&&e.code==SYN_REPORT)) continue;
        if(e.type==EV_ABS&&e.code==ABS_X) x=e.value;
        if(e.type==EV_ABS&&e.code==ABS_Y) y=e.value;
        if(e.type==EV_KEY&&e.code==BTN_TOUCH) down=!!e.value;
        if(dropped) {
            dropped=0;
            /* Discard through SYN_REPORT, then resynchronize current state. */
            struct input_absinfo a;
            unsigned long keys[(KEY_MAX+8*sizeof(long))/(8*sizeof(long))];
            memset(keys,0,sizeof(keys));
            if(!ioctl(fd,EVIOCGABS(ABS_X),&a)) x=a.value;
            if(!ioctl(fd,EVIOCGABS(ABS_Y),&a)) y=a.value;
            if(ioctl(fd,EVIOCGKEY(sizeof(keys)),keys)>=0)
                down=!!(keys[BTN_TOUCH/(8*sizeof(long))]&(1UL<<(BTN_TOUCH%(8*sizeof(long)))));
        }
        if(e.type==EV_SYN&&e.code==SYN_REPORT) {
            if(!pending_ms) pending_ms=monotonic_events?
                (uint64_t)e.input_event_sec*1000+e.input_event_usec/1000:ui_monotonic_ms();
            ui_pointer(x,y,down);
            /* Consume each complete report: rapid DOWN/UP cannot disappear. */
            ui_process();
        }
    }
    if(n<0&&(errno==EAGAIN||errno==EINTR)) return 0;
    return -1;
}
