#include "app_config.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int app_parameters_valid(const AppParameters *p)
{
    return p&&p->palm_leave_frames>=1&&p->palm_leave_frames<=APP_MAX_LEAVE_FRAMES&&
        p->face_leave_frames>=1&&p->face_leave_frames<=APP_MAX_LEAVE_FRAMES&&
        p->relay_seconds>=1&&p->relay_seconds<=APP_MAX_RELAY_SECONDS&&
        p->match_confirmations>=1&&p->match_confirmations<=APP_MAX_MATCH_CONFIRMATIONS&&
        p->success_cooldown_seconds>=0&&p->success_cooldown_seconds<=APP_MAX_SUCCESS_COOLDOWN_SECONDS&&
        (p->strong_liveness==0||p->strong_liveness==1);
}
int app_config_load(const char *directory,AppParameters *p)
{
    *p=(AppParameters){APP_DEFAULT_LEAVE_FRAMES,APP_DEFAULT_LEAVE_FRAMES,APP_DEFAULT_RELAY_SECONDS,
        APP_DEFAULT_MATCH_CONFIRMATIONS,APP_DEFAULT_SUCCESS_COOLDOWN_SECONDS,APP_DEFAULT_STRONG_LIVENESS};
    int d=open(directory,O_RDONLY|O_DIRECTORY|O_CLOEXEC);if(d<0)return -1;
    int fd=openat(d,"app.conf",O_RDONLY|O_CLOEXEC|O_NOFOLLOW);int error=errno;close(d);
    if(fd<0)return error==ENOENT?1:-1;
    char data[256]={0},extra;struct stat st;AppParameters loaded=*p;
    int ok=!fstat(fd,&st)&&S_ISREG(st.st_mode)&&st.st_size>0&&st.st_size<(off_t)sizeof(data);
    ssize_t n=ok?read(fd,data,sizeof(data)-1):-1;close(fd);
    if(n<=0||n!=st.st_size||memchr(data,0,(size_t)n))return -1;
    int v1=!strncmp(data,"version=1\n",10),v2=!strncmp(data,"version=2\n",10),v3=!strncmp(data,"version=3\n",10);
    int fields=v1?sscanf(data,"version=1\npalm_leave_frames=%d\nface_leave_frames=%d %c",
        &loaded.palm_leave_frames,&loaded.face_leave_frames,&extra):
        v2?sscanf(data,"version=2\npalm_leave_frames=%d\nface_leave_frames=%d\nrelay_seconds=%d %c",
        &loaded.palm_leave_frames,&loaded.face_leave_frames,&loaded.relay_seconds,&extra):
        v3?sscanf(data,"version=3\npalm_leave_frames=%d\nface_leave_frames=%d\nrelay_seconds=%d\nmatch_confirmations=%d\nsuccess_cooldown_seconds=%d %c",
        &loaded.palm_leave_frames,&loaded.face_leave_frames,&loaded.relay_seconds,&loaded.match_confirmations,&loaded.success_cooldown_seconds,&extra):
        sscanf(data,"version=4\npalm_leave_frames=%d\nface_leave_frames=%d\nrelay_seconds=%d\nmatch_confirmations=%d\nsuccess_cooldown_seconds=%d\nstrong_liveness=%d %c",
        &loaded.palm_leave_frames,&loaded.face_leave_frames,&loaded.relay_seconds,&loaded.match_confirmations,&loaded.success_cooldown_seconds,&loaded.strong_liveness,&extra);
    if(fields!=(v1?2:v2?3:v3?5:6)||!app_parameters_valid(&loaded))return -1;
    *p=loaded;return v1||v2||v3;
}
int app_config_save(const char *directory,const AppParameters *p)
{
    if(!app_parameters_valid(p)){errno=EINVAL;return -1;}
    int d=open(directory,O_RDONLY|O_DIRECTORY|O_CLOEXEC);if(d<0)return -1;
    int fd=openat(d,"app.conf.tmp",O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC|O_NOFOLLOW,0600),rc=-1;
    if(fd>=0) {
        char data[256];int n=snprintf(data,sizeof(data),"version=4\npalm_leave_frames=%d\nface_leave_frames=%d\nrelay_seconds=%d\nmatch_confirmations=%d\nsuccess_cooldown_seconds=%d\nstrong_liveness=%d\n",
            p->palm_leave_frames,p->face_leave_frames,p->relay_seconds,p->match_confirmations,p->success_cooldown_seconds,p->strong_liveness);
        int ok=write(fd,data,n)==n&&!fsync(fd);
        if(close(fd))ok=0;
        if(ok&&!renameat(d,"app.conf.tmp",d,"app.conf")&&!fsync(d))rc=0;
    }
    if(rc)unlinkat(d,"app.conf.tmp",0);
    close(d);return rc;
}
