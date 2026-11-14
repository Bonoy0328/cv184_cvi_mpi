/* Synthetic attendance transactions only, in a private temporary directory. */
#include "access_store.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
static unsigned long long us(void) {struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (unsigned long long)t.tv_sec*1000000+t.tv_nsec/1000;}
int main(void) {
    char directory[]="/mnt/system/ky50-store-bench.XXXXXX",path[192];
    if(!mkdtemp(directory)){perror("mkdtemp");return 1;}
    const char *modes[]={"DELETE","WAL","WAL","DELETE"};
    for(int run=0;run<4;++run) {
        UserStore s;AppUser u;snprintf(path,sizeof(path),"%s/test%d.sqlite3",directory,run);
        if(store_open(&s,path)||store_create(&s,"900001","timing",&u))return 2;
        char sql[128];snprintf(sql,sizeof(sql),"PRAGMA journal_mode=%s; PRAGMA synchronous=FULL",modes[run]);
        if(sqlite3_exec(s.db,sql,NULL,NULL,NULL)!=SQLITE_OK)return 3;
        sqlite3_stmt *q=NULL;
        if(sqlite3_prepare_v2(s.db,"PRAGMA journal_mode",-1,&q,NULL)!=SQLITE_OK || sqlite3_step(q)!=SQLITE_ROW)return 8;
        const char *actual=(const char *)sqlite3_column_text(q,0);
        printf("STORE_BENCH config_run=%d journal=%s ",run,actual);
        int mode_ok=!strcasecmp(actual,modes[run]);sqlite3_finalize(q);
        if(!mode_ok)return 9;
        if(sqlite3_prepare_v2(s.db,"PRAGMA synchronous",-1,&q,NULL)!=SQLITE_OK || sqlite3_step(q)!=SQLITE_ROW)return 10;
        int level=sqlite3_column_int(q,0);sqlite3_finalize(q);
        printf("synchronous=%d\n",level);if(level!=2)return 11;
        /* Approximately the production row count, without copying any user data. */
        const char *seed="WITH RECURSIVE n(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM n WHERE x<320) "
            "INSERT INTO attendance_events(result,score,created_utc,time_valid,monotonic_ms,method) "
            "SELECT 'unmatched',0,0,0,0,'palm' FROM n";
        if(sqlite3_exec(s.db,seed,NULL,NULL,NULL)!=SQLITE_OK){fprintf(stderr,"seed: %s\n",sqlite3_errmsg(s.db));return 4;}
        for(int sample=0;sample<12;++sample) {
            unsigned long long start=us();int rc=store_record_match_kind(&s,&u,0.9f,start/1000,CREDENTIAL_PALM);
            unsigned long long elapsed=us()-start;
            if(rc)return 5;
            printf("STORE_BENCH mode=%s run=%d sample=%d warmup=%d elapsed_us=%llu\n",modes[run],run,sample,sample<2,elapsed);
        }
        store_close(&s);
        if(unlink(path))return 6;
    }
    if(rmdir(directory)){perror("cleanup");return 7;}
    puts("STORE_BENCH cleanup=ok");return 0;
}
