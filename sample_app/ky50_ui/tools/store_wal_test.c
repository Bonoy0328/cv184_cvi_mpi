/* Isolated files only: exercise WAL durability, readers, backup and failure.
 * Optional directory argument permits the same test on the board's UBIFS. */
#include "access_store.h"
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static void sql(sqlite3 *db,const char *text)
{
    int rc=sqlite3_exec(db,text,NULL,NULL,NULL);
    if(rc!=SQLITE_OK)fprintf(stderr,"test SQL: %s: %s\n",text,sqlite3_errmsg(db));
    assert(rc==SQLITE_OK);
}
static int scalar(sqlite3 *db,const char *text)
{
    sqlite3_stmt *q=NULL;assert(sqlite3_prepare_v2(db,text,-1,&q,NULL)==SQLITE_OK);
    assert(sqlite3_step(q)==SQLITE_ROW);int result=sqlite3_column_int(q,0);
    sqlite3_finalize(q);return result;
}
static void integrity(sqlite3 *db)
{
    sqlite3_stmt *q=NULL;assert(sqlite3_prepare_v2(db,"PRAGMA integrity_check",-1,&q,NULL)==SQLITE_OK);
    assert(sqlite3_step(q)==SQLITE_ROW&&!strcmp((const char *)sqlite3_column_text(q,0),"ok"));sqlite3_finalize(q);
}
static int events(sqlite3 *db){return scalar(db,"SELECT count(*) FROM attendance_events");}
static void kill_writer(const char *path,int commit)
{
    pid_t child=fork();assert(child>=0);
    if(!child) {
        UserStore s;AppUser u;assert(!store_open(&s,path));assert(!store_get(&s,0,&u));
        sqlite3_wal_autocheckpoint(s.db,0);
        if(commit)assert(!store_record_match_kind(&s,&u,.9f,100,CREDENTIAL_FACE));
        else sql(s.db,"BEGIN IMMEDIATE; INSERT INTO device_meta VALUES('uncommitted','must disappear')");
        kill(getpid(),SIGKILL);_exit(99);
    }
    int status;assert(waitpid(child,&status,0)==child);assert(WIFSIGNALED(status)&&WTERMSIG(status)==SIGKILL);
}
int main(int argc,char **argv)
{
    assert(argc<=2);char dir[512],path[640],copy[640],sidecar[660];
    snprintf(dir,sizeof(dir),"%s/ky50-wal-test-XXXXXX",argc==2?argv[1]:"/tmp");assert(mkdtemp(dir));
    snprintf(path,sizeof(path),"%s/test.sqlite3",dir);snprintf(copy,sizeof(copy),"%s/backup.sqlite3",dir);
    UserStore s;AppUser u;assert(!store_open(&s,path));
    assert(scalar(s.db,"PRAGMA synchronous")==2);
    assert(scalar(s.db,"PRAGMA wal_autocheckpoint")==256);
    assert(!store_create(&s,"10001","Synthetic WAL test",&u));
    assert(!store_prepare_enroll_kind(&s,u.local_id,CREDENTIAL_FACE,0));
    assert(!store_finish_enroll_kind(&s,u.local_id,-1,CREDENTIAL_FACE));assert(!store_get(&s,u.local_id,&u));
    assert(!store_checkpoint(&s,1));
    sqlite3 *reader=NULL;assert(sqlite3_open_v2(path,&reader,SQLITE_OPEN_READWRITE,NULL)==SQLITE_OK);
    sql(reader,"BEGIN");assert(events(reader)==0);
    assert(!store_record_match_kind(&s,&u,.9f,1,CREDENTIAL_FACE));
    assert(events(reader)==0&&events(s.db)==1); /* WAL reader does not block commit. */
    assert(store_checkpoint(&s,1)==1); /* A pinned reader safely defers truncate. */
    sqlite3 *dest=NULL;assert(sqlite3_open(copy,&dest)==SQLITE_OK);
    sqlite3_backup *backup=sqlite3_backup_init(dest,"main",s.db,"main");assert(backup);
    assert(sqlite3_backup_step(backup,-1)==SQLITE_DONE);assert(sqlite3_backup_finish(backup)==SQLITE_OK);
    assert(events(dest)==1);integrity(dest);assert(sqlite3_close(dest)==SQLITE_OK);
    sql(reader,"COMMIT");assert(!store_checkpoint(&s,1));
    /* A competing writer and SQLITE_FULL must roll back, never half-update. */
    sql(reader,"BEGIN IMMEDIATE");assert(store_record_match_kind(&s,&u,.9f,2,CREDENTIAL_FACE)<0);
    sql(reader,"ROLLBACK");assert(events(s.db)==1&&scalar(s.db,"SELECT recognition_count FROM users")==1);
    int pages=scalar(s.db,"PRAGMA page_count");char limit[80];snprintf(limit,sizeof(limit),"PRAGMA max_page_count=%d",pages);sql(s.db,limit);
    assert(sqlite3_exec(s.db,"CREATE TABLE space_test(data BLOB); INSERT INTO space_test VALUES(zeroblob(2097152))",NULL,NULL,NULL)==SQLITE_FULL);
    assert(events(s.db)==1&&scalar(s.db,"SELECT recognition_count FROM users")==1);
    sql(s.db,"PRAGMA max_page_count=1073741823; DROP TABLE IF EXISTS space_test");
    sqlite3_close(reader);
    for(int i=0;i<80;i++)assert(!store_record_match_kind(&s,&u,.9f,(uint64_t)i+3,CREDENTIAL_FACE));
    snprintf(sidecar,sizeof(sidecar),"%s-wal",path);struct stat st;assert(!stat(sidecar,&st));
    assert(st.st_size<2*1024*1024); /* No pinned reader: auto-checkpoint bounds reuse. */
    assert(!store_checkpoint(&s,1));assert(!stat(sidecar,&st)&&st.st_size==0);
    integrity(s.db);int expected=events(s.db);store_close(&s);
    kill_writer(path,1);kill_writer(path,0);
    assert(!store_open(&s,path));assert(events(s.db)==expected+1);
    assert(scalar(s.db,"SELECT count(*) FROM device_meta WHERE key='uncommitted'")==0);
    assert(scalar(s.db,"SELECT recognition_count FROM users")==expected+1);integrity(s.db);store_close(&s);
    unlink(path);unlink(copy);
    const char *suffix[]={"-wal","-shm"};
    for(unsigned i=0;i<2;i++){
        snprintf(sidecar,sizeof(sidecar),"%s%s",path,suffix[i]);unlink(sidecar);
        snprintf(sidecar,sizeof(sidecar),"%s%s",copy,suffix[i]);unlink(sidecar);
    }
    assert(!rmdir(dir));
    puts("PASS WAL/FULL: concurrent reader, deferred checkpoint, online backup with WAL, busy writer, SQLITE_FULL, bounded WAL reuse, committed/uncommitted SIGKILL recovery, integrity and clean shutdown");
    return 0;
}
