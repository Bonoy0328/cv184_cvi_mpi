#include "settings.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
const char *camera_source_name(enum camera_source source)
{
    static const char *names[]={"RGB","IR","DVP"};
    return (unsigned)source<CAMERA_SOURCE_COUNT?names[source]:"unknown";
}
int settings_load(const char *dir,enum camera_source *source)
{
    *source=CAMERA_RGB;
    int d=open(dir,O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    if(d<0) return errno==ENOENT?0:-1;
    int fd=openat(d,"display-source",O_RDONLY|O_CLOEXEC|O_NOFOLLOW);close(d);
    if(fd<0) return errno==ENOENT?0:-1;
    char data[16]={0};ssize_t n=read(fd,data,sizeof(data)-1);close(fd);
    if(n<=0) return -1;
    for(int i=0;i<CAMERA_SOURCE_COUNT;i++) {
        char expected[16];snprintf(expected,sizeof(expected),"%s\n",camera_source_name(i));
        if(n==(ssize_t)strlen(expected)&&!memcmp(data,expected,n)) { *source=i;return 0; }
    }
    errno=EINVAL;return -1;
}
/* Only call after receiving and rendering a frame from the selected sensor.
 * No write on every frame/click. File+directory sync and atomic replacement. */
int settings_save(const char *dir,enum camera_source source)
{
    if((unsigned)source>=CAMERA_SOURCE_COUNT) { errno=EINVAL;return -1; }
    if(mkdir(dir,0755)&&errno!=EEXIST) return -1;
    int d=open(dir,O_RDONLY|O_DIRECTORY|O_CLOEXEC);if(d<0) return -1;
    int fd=openat(d,"display-source.tmp",O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC|O_NOFOLLOW,0600);
    int rc=-1;
    if(fd>=0) {
        char data[16];int n=snprintf(data,sizeof(data),"%s\n",camera_source_name(source));
        int ok=write(fd,data,n)==n && !fsync(fd);
        if(close(fd)) ok=0;
        if(ok&&!renameat(d,"display-source.tmp",d,"display-source")&&!fsync(d)) rc=0;
    }
    if(rc) unlinkat(d,"display-source.tmp",0);
    close(d);return rc;
}
