#include "ui.h"
#include <stdio.h>
#include <string.h>
uint64_t ui_monotonic_ms(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000 + t.tv_nsec/1000000;
}
int ui_carrier(const char *path)
{
    FILE *f=fopen(path,"r");
    if (!f) return -1;
    int c=fgetc(f); fclose(f);
    return c=='1' ? 1 : c=='0' ? 0 : -1;
}
int ui_time_text(time_t now, char *out, unsigned size)
{
    /* UTC+8 without changing process/global timezone or relying on zoneinfo. */
    struct tm t; time_t local=now+8*3600;
    if (now<1704067200 || !gmtime_r(&local,&t)) {
        snprintf(out,size,"北京时间待校准"); return 0;
    }
    snprintf(out,size,"%04d年%02d月%02d日 %02d:%02d:%02d",
             t.tm_year+1900,t.tm_mon+1,t.tm_mday,t.tm_hour,t.tm_min,t.tm_sec);
    return 1;
}
