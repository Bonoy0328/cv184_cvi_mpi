/* Read-only report; explicit --backup writes a consistent SQLite copy. */
#include "sqlite3.h"
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
static int backup(sqlite3 *source,const char *path)
{
    int fd=open(path,O_CREAT|O_EXCL|O_RDWR|O_CLOEXEC,0600);
    if(fd<0){perror("create backup");return 1;}
    close(fd);sqlite3 *dest=NULL;sqlite3_backup *copy=NULL;int rc=SQLITE_ERROR;
    if(sqlite3_open_v2(path,&dest,SQLITE_OPEN_READWRITE,NULL)==SQLITE_OK) {
        copy=sqlite3_backup_init(dest,"main",source,"main");
        if(copy) {
            for(int i=0;i<50;i++) {
                rc=sqlite3_backup_step(copy,128);
                if(rc==SQLITE_DONE)break;
                if(rc!=SQLITE_OK&&rc!=SQLITE_BUSY&&rc!=SQLITE_LOCKED)break;
                sqlite3_sleep(20);
            }
            int ended=sqlite3_backup_finish(copy);if(ended!=SQLITE_OK)rc=ended;
        }
    }
    sqlite3_close(dest);
    if(rc!=SQLITE_DONE){unlink(path);fprintf(stderr,"SQLite backup failed: %d\n",rc);return 1;}
    puts("consistent_backup=ok");return 0;
}
int main(int argc,char **argv)
{
    if(argc!=2&&(argc!=4||strcmp(argv[2],"--backup"))){fprintf(stderr,"Usage: access_inspect /path/users.sqlite3 [--backup NEW_FILE]\n");return 2;}
    sqlite3 *db=NULL;int result=1;
    if(sqlite3_open_v2(argv[1],&db,SQLITE_OPEN_READONLY,NULL)!=SQLITE_OK)goto done;
    sqlite3_busy_timeout(db,1000);
    const char *sql[]={
        "PRAGMA integrity_check",
        "SELECT count(*) FROM users",
        "SELECT count(*) FROM credentials WHERE kind='palm' AND state=2",
        "SELECT count(*) FROM operations",
        "SELECT count(*) FROM credentials WHERE kind IN('face','palm','fingerprint') AND state IN(1,2) AND (template_uuid IS NULL OR template_uuid NOT BETWEEN 0 AND 99)",
        "SELECT count(*) FROM credentials WHERE kind='face' AND state=2",
        "SELECT count(*) FROM (SELECT kind,template_uuid FROM credentials WHERE template_uuid IS NOT NULL GROUP BY kind,template_uuid HAVING count(*)>1)",
        "SELECT count(*) FROM attendance_events WHERE result='matched'",
        "SELECT count(*) FROM attendance_events WHERE result='unmatched'",
        "SELECT count(*) FROM attendance_events WHERE result='matched' AND user_id IS NULL",
        "SELECT count(*) FROM users WHERE state='deleting'",
        "PRAGMA user_version",
        "PRAGMA journal_mode",
        "SELECT count(*) FROM credentials WHERE kind='fingerprint' AND state=2"};
    const char *label[]={"integrity","users","registered_palms","pending_operations","invalid_template_bindings","registered_faces","duplicate_template_bindings","matched_events","unmatched_events","retained_deleted_user_events","pending_deletions","schema","journal_mode","registered_fingerprints"};
    if(sqlite3_exec(db,"BEGIN",NULL,NULL,NULL)!=SQLITE_OK)goto done;
    for(unsigned i=0;i<sizeof(sql)/sizeof(sql[0]);i++) {
        sqlite3_stmt *q=NULL;
        if(sqlite3_prepare_v2(db,sql[i],-1,&q,NULL)!=SQLITE_OK)goto done;
        if(sqlite3_step(q)!=SQLITE_ROW){sqlite3_finalize(q);goto done;}
        printf("%s=%s\n",label[i],sqlite3_column_text(q,0));sqlite3_finalize(q);
    }
    result=sqlite3_exec(db,"COMMIT",NULL,NULL,NULL)==SQLITE_OK?0:1;
    if(!result&&argc==4)result=backup(db,argv[3]);
done:
    if(result)fprintf(stderr,"Inspection failed: %s\n",db?sqlite3_errmsg(db):"open error");
    sqlite3_close(db);return result;
}
