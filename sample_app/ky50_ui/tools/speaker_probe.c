/* Run from /tmp/ky50-ui with the UI idle. Does not touch SYS/VI/VO. */
#include "factory.h"
#include <stdio.h>
#include <unistd.h>
int main(void)
{
    if(factory_speaker()) return 1;
    const char *result=NULL;
    for(int i=0;i<1000&&!result;i++) { usleep(10000);result=factory_poll(); }
    factory_stop();puts(result?result:"timeout");return result?0:1;
}
