#include "access_store.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

static int error(UserStore *s)
{
    snprintf(s->error,sizeof(s->error),"%s",s->db?sqlite3_errmsg(s->db):"database unavailable");
    fprintf(stderr,"USER_DB %s\n",s->error);return -1;
}
static int exec(UserStore *s,const char *sql)
{
    if(sqlite3_exec(s->db,sql,NULL,NULL,NULL)!=SQLITE_OK) return error(s);
    return 0;
}
static int prepare(UserStore *s,sqlite3_stmt **q,const char *sql)
{
    return sqlite3_prepare_v2(s->db,sql,-1,q,NULL)==SQLITE_OK?0:error(s);
}
static int done(UserStore *s,sqlite3_stmt *q)
{
    int rc=sqlite3_step(q)==SQLITE_DONE?0:error(s);sqlite3_finalize(q);return rc;
}
static int finish(UserStore *s,int rc)
{
    if(!rc && !exec(s,"COMMIT")) return 0;
    exec(s,"ROLLBACK");return -1;
}
static int audit(UserStore *s,uint16_t local_id,const char *action,const char *detail)
{
    sqlite3_stmt *q=NULL;
    if(prepare(s,&q,"INSERT INTO audit_events(user_id,local_id,employee_id,name,action,detail,created_utc,time_valid) "
        "SELECT id,local_id,employee_id,name,?,?,?,? FROM users WHERE local_id=?")) return -1;
    sqlite3_bind_text(q,1,action,-1,SQLITE_TRANSIENT);sqlite3_bind_text(q,2,detail,-1,SQLITE_TRANSIENT);
    time_t now=time(NULL);sqlite3_bind_int64(q,3,now);sqlite3_bind_int(q,4,now>=1704067200);
    sqlite3_bind_int(q,5,local_id);
    if(done(s,q)) return -1;
    return exec(s,"DELETE FROM audit_events WHERE id<=(SELECT max(id)-2000 FROM audit_events)");
}
int store_open(UserStore *s,const char *path)
{
    memset(s,0,sizeof(*s));
    if(sqlite3_open_v2(path,&s->db,SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE|SQLITE_OPEN_FULLMUTEX,NULL)!=SQLITE_OK) {
        error(s);store_close(s);return -1;
    }
    chmod(path,0600);sqlite3_busy_timeout(s->db,500);
    sqlite3_stmt *q=NULL;int version=-1;
    /* WAL keeps FULL durability and avoids the per-transaction rollback
     * journal writes on sync-mounted UBIFS. Verify the returned mode: a PRAGMA
     * can succeed while the requested mode is not actually available. */
    if(prepare(s,&q,"PRAGMA journal_mode=WAL"))goto fail;
    int wal=sqlite3_step(q)==SQLITE_ROW&&!sqlite3_stricmp((const char *)sqlite3_column_text(q,0),"wal");
    sqlite3_finalize(q);
    if(!wal){snprintf(s->error,sizeof(s->error),"WAL unavailable");fprintf(stderr,"USER_DB %s\n",s->error);goto fail;}
    if(exec(s,"PRAGMA foreign_keys=ON; PRAGMA synchronous=FULL; "
        "PRAGMA temp_store=MEMORY; PRAGMA cache_size=-256; PRAGMA secure_delete=ON; "
        "PRAGMA journal_size_limit=1048576")||sqlite3_wal_autocheckpoint(s->db,256)!=SQLITE_OK)goto fail;
    if(prepare(s,&q,"PRAGMA user_version")) goto fail;
    if(sqlite3_step(q)==SQLITE_ROW) version=sqlite3_column_int(q,0);
    sqlite3_finalize(q);
    if(version!=0&&version!=1&&version!=2&&version!=3&&version!=4) { snprintf(s->error,sizeof(s->error),"Unsupported schema %d",version);goto fail; }
    int reset=version==1||version==2;
    if(exec(s,"BEGIN IMMEDIATE"))goto fail;
    /* User explicitly requested discarding v1/v2 data. Keep device parameters,
     * but persist a reset marker until BOTH SDK databases have been emptied. */
    if(reset&&exec(s,"DROP TABLE IF EXISTS operations; DROP TABLE IF EXISTS credentials; "
        "DROP TABLE IF EXISTS attendance_events; DROP TABLE IF EXISTS audit_events; DROP TABLE IF EXISTS users;"))goto rollback;
    if(version==3&&exec(s,"DROP INDEX IF EXISTS credential_template; DROP TRIGGER IF EXISTS credential_changed; "
        "ALTER TABLE credentials RENAME TO credentials_v3; ALTER TABLE operations RENAME TO operations_v3;"))goto rollback;
    if(exec(s,
        "CREATE TABLE IF NOT EXISTS users("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,local_id INTEGER NOT NULL UNIQUE CHECK(local_id BETWEEN 0 AND 99),"
        "employee_id TEXT NOT NULL UNIQUE CHECK(length(employee_id) BETWEEN 1 AND 32 AND employee_id NOT GLOB '*[^0-9]*'),"
        "name TEXT NOT NULL DEFAULT '',is_admin INTEGER NOT NULL DEFAULT 0 CHECK(is_admin IN(0,1)),"
        "enabled INTEGER NOT NULL DEFAULT 1 CHECK(enabled IN(0,1)),state TEXT NOT NULL DEFAULT 'active' CHECK(state IN('active','deleting')),"
        "department TEXT NOT NULL DEFAULT '',job_title TEXT NOT NULL DEFAULT '',company TEXT NOT NULL DEFAULT '',"
        "phone TEXT NOT NULL DEFAULT '',email TEXT NOT NULL DEFAULT '',note TEXT NOT NULL DEFAULT '',"
        "avatar_path TEXT NOT NULL DEFAULT '',access_profile TEXT NOT NULL DEFAULT '',attendance_group TEXT NOT NULL DEFAULT '',"
        "valid_from_utc INTEGER,valid_until_utc INTEGER,created_utc INTEGER NOT NULL,updated_utc INTEGER NOT NULL,"
        "created_time_valid INTEGER NOT NULL DEFAULT 0,last_seen_utc INTEGER,last_score REAL,recognition_count INTEGER NOT NULL DEFAULT 0,"
        "source TEXT NOT NULL DEFAULT 'local',external_id TEXT NOT NULL DEFAULT '',sync_revision INTEGER NOT NULL DEFAULT 0);"
        "CREATE TABLE IF NOT EXISTS credentials("
        "user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,"
        "kind TEXT NOT NULL CHECK(kind IN('face','palm','fingerprint','card','password')),"
        "state INTEGER NOT NULL DEFAULT 0 CHECK(state BETWEEN 0 AND 3),template_uuid INTEGER,"
        "model_id TEXT NOT NULL DEFAULT '',hand_side INTEGER,registered_utc INTEGER,updated_utc INTEGER NOT NULL,"
        "last_error TEXT NOT NULL DEFAULT '',credential_ref TEXT NOT NULL DEFAULT '',face_direction INTEGER,"
        "details_json TEXT NOT NULL DEFAULT '{}',PRIMARY KEY(user_id,kind),"
        "CHECK(kind NOT IN('face','palm','fingerprint') OR (state IN(0,3) AND template_uuid IS NULL) OR "
        "(state IN(1,2) AND template_uuid IS NOT NULL AND template_uuid BETWEEN 0 AND 99)));"
        "CREATE UNIQUE INDEX IF NOT EXISTS credential_template ON credentials(kind,template_uuid) WHERE template_uuid IS NOT NULL;"
        "CREATE TRIGGER IF NOT EXISTS credential_changed AFTER UPDATE ON credentials BEGIN "
        "UPDATE users SET updated_utc=NEW.updated_utc WHERE id=NEW.user_id; END;"
        "CREATE TABLE IF NOT EXISTS operations(local_id INTEGER PRIMARY KEY REFERENCES users(local_id) ON DELETE CASCADE,"
        "kind TEXT NOT NULL CHECK(kind IN('enroll','delete')),created_utc INTEGER NOT NULL,"
        "modality TEXT NOT NULL CHECK(modality IN('palm','face','fingerprint')),template_uuid INTEGER,"
        "CHECK(kind='delete' OR (template_uuid IS NOT NULL AND template_uuid BETWEEN 0 AND 99)));"
        "CREATE TABLE IF NOT EXISTS attendance_events(id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "user_id INTEGER REFERENCES users(id) ON DELETE SET NULL,local_id INTEGER,employee_id TEXT NOT NULL DEFAULT '',"
        "name TEXT NOT NULL DEFAULT '',method TEXT NOT NULL DEFAULT 'palm',result TEXT NOT NULL,score REAL,template_uuid INTEGER,"
        "created_utc INTEGER NOT NULL,time_valid INTEGER NOT NULL,monotonic_ms INTEGER NOT NULL,"
        "device_id TEXT NOT NULL DEFAULT 'local',direction TEXT NOT NULL DEFAULT 'unspecified',exported INTEGER NOT NULL DEFAULT 0);"
        "CREATE INDEX IF NOT EXISTS attendance_user_time ON attendance_events(user_id,created_utc);"
        "CREATE TABLE IF NOT EXISTS audit_events(id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "user_id INTEGER REFERENCES users(id) ON DELETE SET NULL,local_id INTEGER,employee_id TEXT NOT NULL,name TEXT NOT NULL,"
        "action TEXT NOT NULL,detail TEXT NOT NULL,created_utc INTEGER NOT NULL,time_valid INTEGER NOT NULL);"
        "CREATE TABLE IF NOT EXISTS device_meta(key TEXT PRIMARY KEY,value TEXT NOT NULL);"
        "INSERT OR REPLACE INTO device_meta VALUES('schema','4');"
        "PRAGMA application_id=1264137520; PRAGMA user_version=4;"))goto rollback;
    if(version==3&&exec(s,"INSERT INTO credentials SELECT * FROM credentials_v3; "
        "INSERT INTO operations SELECT * FROM operations_v3; DROP TABLE operations_v3; DROP TABLE credentials_v3;"))goto rollback;
    if(reset&&exec(s,"INSERT OR REPLACE INTO device_meta VALUES('templates_reset_pending','1')"))goto rollback;
    if(exec(s,"COMMIT"))goto rollback;
    printf("USER_DB journal=WAL synchronous=FULL autocheckpoint_pages=256\n");
    return 0;
rollback:
    exec(s,"ROLLBACK");
fail:
    store_close(s);return -1;
}
void store_close(UserStore *s)
{
    if(s->db){store_checkpoint(s,1);sqlite3_close(s->db);}
    s->db=NULL;
}
int store_checkpoint(UserStore *s,int truncate)
{
    if(!s->db)return -1;
    int frames=-1,done=-1;
    struct timespec a,b;clock_gettime(CLOCK_MONOTONIC,&a);
    int rc=sqlite3_wal_checkpoint_v2(s->db,NULL,truncate?SQLITE_CHECKPOINT_TRUNCATE:SQLITE_CHECKPOINT_PASSIVE,&frames,&done);
    clock_gettime(CLOCK_MONOTONIC,&b);
    if(getenv("KY50_TRACE_STALL")&&(frames>0||truncate||rc!=SQLITE_OK))
        printf("STORE_CHECKPOINT mode=%s rc=%d frames=%d done=%d elapsed_us=%lld\n",
            truncate?"truncate":"passive",rc,frames,done,
            (long long)(b.tv_sec-a.tv_sec)*1000000+(b.tv_nsec-a.tv_nsec)/1000);
    if(rc!=SQLITE_OK&&rc!=SQLITE_BUSY)return error(s);
    return rc==SQLITE_OK?0:1;
}
int palm_parameters_valid(const PalmParameters *p)
{
    return p&&p->hand_threshold>=0&&p->hand_threshold<=100&&
        p->auth_threshold>=0&&p->auth_threshold<=100&&p->dynamic_level>=0&&p->dynamic_level<=4;
}
int store_parameters_load(UserStore *s,PalmParameters *p)
{
    sqlite3_stmt *q=NULL;PalmParameters loaded;char extra;
    if(prepare(s,&q,"SELECT value FROM device_meta WHERE key='pva.parameters.v1'"))return -1;
    int step=sqlite3_step(q),rc=-1;
    if(step==SQLITE_DONE)rc=1;
    else if(step==SQLITE_ROW) {
        const char *text=(const char*)sqlite3_column_text(q,0);
        if(text&&sscanf(text,"%d %d %d %c",&loaded.hand_threshold,&loaded.auth_threshold,&loaded.dynamic_level,&extra)==3&&
           palm_parameters_valid(&loaded)){*p=loaded;rc=0;}
    }
    sqlite3_finalize(q);return rc;
}
int store_parameters_save(UserStore *s,const PalmParameters *p)
{
    if(!palm_parameters_valid(p))return -1;
    sqlite3_stmt *q=NULL;char text[48];
    snprintf(text,sizeof(text),"%d %d %d",p->hand_threshold,p->auth_threshold,p->dynamic_level);
    /* One statement is an atomic transaction; synchronous=FULL is set at open. */
    if(prepare(s,&q,"INSERT INTO device_meta(key,value) VALUES('pva.parameters.v1',?) "
        "ON CONFLICT(key) DO UPDATE SET value=excluded.value"))return -1;
    sqlite3_bind_text(q,1,text,-1,SQLITE_TRANSIENT);return done(s,q);
}
int face_parameters_valid(const FaceParameters *p)
{
    return p&&p->detection_threshold>=1&&p->detection_threshold<=100&&
        p->quality_threshold>=0&&p->quality_threshold<=100&&
        p->liveness_threshold>=0&&p->liveness_threshold<=100&&
        (p->quality_enable==0||p->quality_enable==1)&&
        (p->liveness_enable==0||p->liveness_enable==1)&&
        p->auth_threshold>=-100&&p->auth_threshold<=100&&
        p->filter_threshold>=-100&&p->filter_threshold<=100;
}
int store_face_parameters_load(UserStore *s,FaceParameters *p)
{
    sqlite3_stmt *q=NULL;FaceParameters loaded;char extra;
    if(prepare(s,&q,"SELECT value FROM device_meta WHERE key='pva.face.parameters.v1'"))return -1;
    int step=sqlite3_step(q),rc=-1;
    if(step==SQLITE_DONE)rc=1;
    else if(step==SQLITE_ROW) {
        const char *text=(const char*)sqlite3_column_text(q,0);
        if(text&&sscanf(text,"%d %d %d %d %d %d %d %c",&loaded.detection_threshold,
            &loaded.quality_threshold,&loaded.liveness_threshold,&loaded.quality_enable,
            &loaded.liveness_enable,&loaded.auth_threshold,&loaded.filter_threshold,&extra)==7&&
            face_parameters_valid(&loaded)){*p=loaded;rc=0;}
    }
    sqlite3_finalize(q);return rc;
}
int store_face_parameters_save(UserStore *s,const FaceParameters *p)
{
    if(!face_parameters_valid(p))return -1;
    sqlite3_stmt *q=NULL;char text[96];
    snprintf(text,sizeof(text),"%d %d %d %d %d %d %d",p->detection_threshold,
        p->quality_threshold,p->liveness_threshold,p->quality_enable,p->liveness_enable,
        p->auth_threshold,p->filter_threshold);
    if(prepare(s,&q,"INSERT INTO device_meta(key,value) VALUES('pva.face.parameters.v1',?) "
        "ON CONFLICT(key) DO UPDATE SET value=excluded.value"))return -1;
    sqlite3_bind_text(q,1,text,-1,SQLITE_TRANSIENT);return done(s,q);
}
int store_ir_target_load(UserStore *s,int *target)
{
    sqlite3_stmt *q=NULL;int value;char extra;
    if(prepare(s,&q,"SELECT value FROM device_meta WHERE key='pva.ir_ae_target.v1'"))return -1;
    int step=sqlite3_step(q),rc=-1;
    if(step==SQLITE_DONE)rc=1;
    else if(step==SQLITE_ROW) {
        const char *text=(const char*)sqlite3_column_text(q,0);
        if(text&&sscanf(text,"%d %c",&value,&extra)==1&&value>=1&&value<=255){*target=value;rc=0;}
    }
    sqlite3_finalize(q);return rc;
}
int store_ir_target_save(UserStore *s,int target)
{
    if(target<1||target>255)return -1;
    sqlite3_stmt *q=NULL;char text[16];snprintf(text,sizeof(text),"%d",target);
    if(prepare(s,&q,"INSERT INTO device_meta(key,value) VALUES('pva.ir_ae_target.v1',?) "
        "ON CONFLICT(key) DO UPDATE SET value=excluded.value"))return -1;
    sqlite3_bind_text(q,1,text,-1,SQLITE_TRANSIENT);return done(s,q);
}
void app_utf8_copy(char *out,size_t capacity,const char *in)
{
    if(!capacity) return;
    size_t n=strlen(in);if(n>=capacity) { n=capacity-1;while(n&&((unsigned char)in[n]&0xc0)==0x80)--n; }
    memcpy(out,in,n);out[n]=0;
}
int store_employee_valid(const char *text)
{
    size_t n=strlen(text);if(!n||n>=APP_EMPLOYEE_BYTES)return 0;
    for(size_t i=0;i<n;i++) if(text[i]<'0'||text[i]>'9')return 0;
    return 1;
}
int store_name_valid(const char *text)
{
    size_t bytes=strlen(text);unsigned chars=0;
    if(bytes>=APP_NAME_BYTES) return 0;
    for(size_t i=0;i<bytes;) {
        const unsigned char *p=(const unsigned char*)text+i;uint32_t cp;unsigned n;
        if(p[0]<0x80) { cp=p[0];n=1; }
        else if(p[0]>=0xc2&&p[0]<=0xdf) {cp=p[0]&31;n=2;}
        else if(p[0]>=0xe0&&p[0]<=0xef) {cp=p[0]&15;n=3;}
        else if(p[0]>=0xf0&&p[0]<=0xf4) {cp=p[0]&7;n=4;}
        else return 0;
        if(i+n>bytes) return 0;
        for(unsigned j=1;j<n;j++) {if((p[j]&0xc0)!=0x80)return 0;cp=(cp<<6)|(p[j]&63);}
        if((n==2&&cp<0x80)||(n==3&&cp<0x800)||(n==4&&cp<0x10000)||cp>0x10ffff||
           (cp>=0xd800&&cp<=0xdfff)||cp<32||cp==127) return 0;
        if(++chars>32)return 0;
        i+=n;
    }
    return 1;
}
static void row(sqlite3_stmt *q,AppUser *u)
{
    memset(u,0,sizeof(*u));u->record_id=sqlite3_column_int64(q,0);u->local_id=sqlite3_column_int(q,1);
    app_utf8_copy(u->employee_id,sizeof(u->employee_id),(const char*)sqlite3_column_text(q,2));
    app_utf8_copy(u->name,sizeof(u->name),(const char*)sqlite3_column_text(q,3));
    app_utf8_copy(u->avatar,sizeof(u->avatar),(const char*)sqlite3_column_text(q,4));
    u->enabled=sqlite3_column_int(q,5);u->is_admin=sqlite3_column_int(q,6);u->deleting=sqlite3_column_int(q,7);
    u->registered=sqlite3_column_int(q,8);u->palm_state=sqlite3_column_int(q,9);u->face_state=sqlite3_column_int(q,10);
    u->palm_uuid=sqlite3_column_int(q,11);u->face_uuid=sqlite3_column_int(q,12);
    u->finger_state=sqlite3_column_int(q,13);u->finger_uuid=sqlite3_column_int(q,14);
}
#define USER_SELECT "SELECT u.id,u.local_id,u.employee_id,u.name,u.avatar_path,u.enabled,u.is_admin,u.state='deleting'," \
    "COALESCE((SELECT sum(CASE c.kind WHEN 'face' THEN 1 WHEN 'palm' THEN 2 WHEN 'fingerprint' THEN 4 WHEN 'card' THEN 8 ELSE 16 END) " \
    "FROM credentials c WHERE c.user_id=u.id AND c.state=2),0)," \
    "COALESCE((SELECT state FROM credentials WHERE user_id=u.id AND kind='palm'),0)," \
    "COALESCE((SELECT state FROM credentials WHERE user_id=u.id AND kind='face'),0)," \
    "COALESCE((SELECT template_uuid FROM credentials WHERE user_id=u.id AND kind='palm'),-1)," \
    "COALESCE((SELECT template_uuid FROM credentials WHERE user_id=u.id AND kind='face'),-1),COALESCE((SELECT state FROM credentials WHERE user_id=u.id AND kind='fingerprint'),0),COALESCE((SELECT template_uuid FROM credentials WHERE user_id=u.id AND kind='fingerprint'),-1) FROM users u "
int store_list(UserStore *s,AppUser *users,int capacity)
{
    sqlite3_stmt *q=NULL;if(prepare(s,&q,USER_SELECT "ORDER BY u.id DESC"))return -1;
    int n=0,rc;while((rc=sqlite3_step(q))==SQLITE_ROW) {
        if(n==capacity) {sqlite3_finalize(q);return -1;}row(q,&users[n++]);
    }
    if(rc!=SQLITE_DONE) {error(s);n=-1;}sqlite3_finalize(q);return n;
}
int store_get(UserStore *s,uint16_t local_id,AppUser *u)
{
    sqlite3_stmt *q=NULL;if(prepare(s,&q,USER_SELECT "WHERE u.local_id=?"))return -1;
    sqlite3_bind_int(q,1,local_id);int rc=sqlite3_step(q);if(rc==SQLITE_ROW)row(q,u);
    sqlite3_finalize(q);return rc==SQLITE_ROW?0:-1;
}
int store_create(UserStore *s,const char *employee,const char *name,AppUser *out)
{
    if(!store_employee_valid(employee)||!store_name_valid(name))return -1;
    AppUser existing[APP_MAX_USERS];int n=store_list(s,existing,APP_MAX_USERS);
    if(n<0)return -1;
    uint8_t used[APP_MAX_USERS]={0};
    for(int i=0;i<n;i++) {
        if(!strcmp(existing[i].employee_id,employee))return -2;
        if(existing[i].local_id<APP_MAX_USERS)used[existing[i].local_id]=1;
    }
    int local_id;for(local_id=0;local_id<APP_MAX_USERS&&used[local_id];local_id++);
    if(local_id==APP_MAX_USERS)return -3;
    if(exec(s,"BEGIN IMMEDIATE"))return -1;
    sqlite3_stmt *q=NULL;int rc=-1;
    if(!prepare(s,&q,"INSERT INTO users(local_id,employee_id,name,created_utc,updated_utc,created_time_valid) VALUES(?,?,?,?,?,?)")) {
        time_t now=time(NULL);sqlite3_bind_int(q,1,local_id);sqlite3_bind_text(q,2,employee,-1,SQLITE_TRANSIENT);
        sqlite3_bind_text(q,3,name,-1,SQLITE_TRANSIENT);sqlite3_bind_int64(q,4,now);sqlite3_bind_int64(q,5,now);
        sqlite3_bind_int(q,6,now>=1704067200);rc=done(s,q);
    }
    static const char *kinds[]={"face","palm","fingerprint","card","password"};
    for(int i=0;!rc&&i<5;i++) {
        if(prepare(s,&q,"INSERT INTO credentials(user_id,kind,updated_utc) SELECT id,?,? FROM users WHERE local_id=?")){rc=-1;break;}
        sqlite3_bind_text(q,1,kinds[i],-1,SQLITE_STATIC);sqlite3_bind_int64(q,2,time(NULL));sqlite3_bind_int(q,3,local_id);rc=done(s,q);
    }
    if(!rc)rc=audit(s,local_id,"create","local touch UI");
    if(finish(s,rc))return -1;
    return store_get(s,local_id,out);
}
static const char *modality_name(unsigned kind){return kind==CREDENTIAL_FINGER?"fingerprint":kind==CREDENTIAL_FACE?"face":"palm";}
int store_get_by_template(UserStore *s,unsigned kind,uint16_t template_uuid,AppUser *u)
{
    if((kind!=CREDENTIAL_FACE&&kind!=CREDENTIAL_PALM&&kind!=CREDENTIAL_FINGER)||template_uuid>=APP_TEMPLATE_SLOTS)return -1;
    sqlite3_stmt *q=NULL;
    if(prepare(s,&q,USER_SELECT "WHERE u.id=(SELECT user_id FROM credentials WHERE kind=? AND template_uuid=?)"))return -1;
    sqlite3_bind_text(q,1,modality_name(kind),-1,SQLITE_STATIC);sqlite3_bind_int(q,2,template_uuid);
    int rc=sqlite3_step(q);if(rc==SQLITE_ROW)row(q,u);sqlite3_finalize(q);return rc==SQLITE_ROW?0:-1;
}
int store_templates_reset_pending(UserStore *s)
{
    sqlite3_stmt *q=NULL;if(prepare(s,&q,"SELECT 1 FROM device_meta WHERE key='templates_reset_pending' AND value='1'"))return -1;
    int rc=sqlite3_step(q);sqlite3_finalize(q);return rc==SQLITE_ROW?1:rc==SQLITE_DONE?0:-1;
}
int store_finish_templates_reset(UserStore *s)
{
    return exec(s,"DELETE FROM device_meta WHERE key='templates_reset_pending'");
}
int store_fingerprint_reset_pending(UserStore *s)
{
    sqlite3_stmt *q=NULL;
    if(prepare(s,&q,"SELECT value FROM device_meta WHERE key='fingerprint.input_contract'"))return -1;
    int rc=sqlite3_step(q),result=-1;
    if(rc==SQLITE_DONE)result=1;
    else if(rc==SQLITE_ROW)result=strcmp((const char*)sqlite3_column_text(q,0),APP_FINGER_MODEL_ID)!=0;
    sqlite3_finalize(q);return result;
}
int store_finish_fingerprint_reset(UserStore *s)
{
    if(exec(s,"BEGIN IMMEDIATE"))return -1;
    if(exec(s,"DELETE FROM operations WHERE kind='enroll' AND modality='fingerprint';"
        "UPDATE credentials SET state=0,template_uuid=NULL,model_id='" APP_FINGER_MODEL_ID "',"
        "registered_utc=NULL,updated_utc=strftime('%s','now'),last_error='ROI upgrade: re-enroll fingerprint' WHERE kind='fingerprint';"
        "INSERT OR REPLACE INTO device_meta(key,value) VALUES('fingerprint.input_contract','" APP_FINGER_MODEL_ID "');")||exec(s,"COMMIT")) {
        exec(s,"ROLLBACK");return -1;
    }
    return 0;
}
static int credential_update(UserStore *s,uint16_t local_id,int state,const char *detail,int side,unsigned kind,int template_uuid)
{
    sqlite3_stmt *q=NULL;
    if(prepare(s,&q,"UPDATE credentials SET state=?,template_uuid=?,model_id=?10,last_error=?3,"
        "updated_utc=?4,hand_side=?5,registered_utc=CASE WHEN ?6=2 THEN ?7 ELSE registered_utc END "
        "WHERE kind=?9 AND user_id=(SELECT id FROM users WHERE local_id=?8)"))return -1;
    sqlite3_bind_int(q,1,state);
    if(template_uuid<0)sqlite3_bind_null(q,2);else sqlite3_bind_int(q,2,template_uuid);
    sqlite3_bind_text(q,3,detail,-1,SQLITE_TRANSIENT);
    time_t now=time(NULL);sqlite3_bind_int64(q,4,now);sqlite3_bind_int(q,5,side);sqlite3_bind_int(q,6,state);
    sqlite3_bind_int64(q,7,now);sqlite3_bind_int(q,8,local_id);
    sqlite3_bind_text(q,9,modality_name(kind),-1,SQLITE_STATIC);
    sqlite3_bind_text(q,10,kind==CREDENTIAL_FINGER?APP_FINGER_MODEL_ID:kind==CREDENTIAL_FACE?"cv184x-face-1000-20261113":"cv184x-fused-20261111",-1,SQLITE_STATIC);
    return done(s,q);
}
static int operation(UserStore *s,uint16_t local_id,const char *kind,unsigned modality,int template_uuid)
{
    sqlite3_stmt *q=NULL;if(prepare(s,&q,"INSERT INTO operations(local_id,kind,created_utc,modality,template_uuid) VALUES(?,?,?,?,?)"))return -1;
    sqlite3_bind_int(q,1,local_id);sqlite3_bind_text(q,2,kind,-1,SQLITE_STATIC);sqlite3_bind_int64(q,3,time(NULL));
    sqlite3_bind_text(q,4,modality_name(modality),-1,SQLITE_STATIC);
    if(template_uuid<0)sqlite3_bind_null(q,5);else sqlite3_bind_int(q,5,template_uuid);
    return done(s,q);
}
static int clear_operation(UserStore *s,uint16_t local_id)
{
    sqlite3_stmt *q=NULL;if(prepare(s,&q,"DELETE FROM operations WHERE local_id=?"))return -1;
    sqlite3_bind_int(q,1,local_id);return done(s,q);
}
int store_prepare_enroll_kind(UserStore *s,uint16_t local_id,unsigned kind,uint16_t template_uuid)
{
    if((kind!=CREDENTIAL_FACE&&kind!=CREDENTIAL_PALM&&kind!=CREDENTIAL_FINGER)||template_uuid>=APP_TEMPLATE_SLOTS)return -1;
    AppUser u;if(store_get(s,local_id,&u)||u.deleting)return -1;
    int state=kind==CREDENTIAL_FINGER?u.finger_state:kind==CREDENTIAL_FACE?u.face_state:u.palm_state;
    if(state!=CRED_NONE&&state!=CRED_MISSING)return -1;
    if(exec(s,"BEGIN IMMEDIATE"))return -1;
    int rc=operation(s,local_id,"enroll",kind,template_uuid);
    if(!rc)rc=credential_update(s,local_id,CRED_PENDING,"",-1,kind,template_uuid);
    if(!rc)rc=audit(s,local_id,"enroll_begin",modality_name(kind));
    return finish(s,rc);
}
int store_finish_enroll_kind(UserStore *s,uint16_t local_id,int hand_side,unsigned kind)
{
    AppUser u;if(store_get(s,local_id,&u))return -1;
    int template_uuid=kind==CREDENTIAL_FINGER?u.finger_uuid:kind==CREDENTIAL_FACE?u.face_uuid:u.palm_uuid;
    if(template_uuid<0||(kind==CREDENTIAL_FINGER?u.finger_state:kind==CREDENTIAL_FACE?u.face_state:u.palm_state)!=CRED_PENDING)return -1;
    if(exec(s,"BEGIN IMMEDIATE"))return -1;
    int rc=credential_update(s,local_id,CRED_REGISTERED,"",hand_side,kind,template_uuid);
    if(!rc)rc=clear_operation(s,local_id);
    if(!rc)rc=audit(s,local_id,"enroll_success",modality_name(kind));
    return finish(s,rc);
}
int store_cancel_enroll_kind(UserStore *s,uint16_t local_id,const char *reason,unsigned kind)
{
    if(exec(s,"BEGIN IMMEDIATE"))return -1;
    int rc=credential_update(s,local_id,CRED_NONE,reason,-1,kind,-1);
    if(!rc)rc=clear_operation(s,local_id);
    if(!rc)rc=audit(s,local_id,"enroll_cancel",reason);
    return finish(s,rc);
}
int store_prepare_delete(UserStore *s,uint16_t local_id)
{
    AppUser u;if(store_get(s,local_id,&u))return -1;if(u.deleting)return 0;
    if(exec(s,"BEGIN IMMEDIATE"))return -1;
    sqlite3_stmt *q=NULL;int rc=operation(s,local_id,"delete",CREDENTIAL_PALM,-1);
    if(!rc) {
        rc=prepare(s,&q,"UPDATE users SET state='deleting',updated_utc=? WHERE local_id=?");
        if(!rc){sqlite3_bind_int64(q,1,time(NULL));sqlite3_bind_int(q,2,local_id);rc=done(s,q);}
    }
    if(!rc)rc=audit(s,local_id,"delete_begin","all credentials and profile");
    return finish(s,rc);
}
int store_finish_delete(UserStore *s,uint16_t local_id)
{
    if(exec(s,"BEGIN IMMEDIATE"))return -1;
    int rc=audit(s,local_id,"delete_success","all templates absent verified");sqlite3_stmt *q=NULL;
    if(!rc){rc=prepare(s,&q,"DELETE FROM users WHERE local_id=? AND state='deleting'");if(!rc){sqlite3_bind_int(q,1,local_id);rc=done(s,q);}}
    return finish(s,rc);
}
int store_pending(UserStore *s,PendingOperation *ops,int capacity)
{
    sqlite3_stmt *q=NULL;if(prepare(s,&q,"SELECT local_id,kind='delete',CASE modality WHEN 'fingerprint' THEN 4 WHEN 'face' THEN 1 ELSE 2 END,COALESCE(template_uuid,-1) FROM operations ORDER BY created_utc"))return -1;
    int n=0,rc;while((rc=sqlite3_step(q))==SQLITE_ROW){if(n==capacity){sqlite3_finalize(q);return -1;}
        ops[n].local_id=sqlite3_column_int(q,0);ops[n].deleting=sqlite3_column_int(q,1);ops[n].modality=sqlite3_column_int(q,2);ops[n++].template_uuid=sqlite3_column_int(q,3);}
    sqlite3_finalize(q);return rc==SQLITE_DONE?n:-1;
}
int store_mark_missing_kind(UserStore *s,uint16_t local_id,unsigned kind)
{
    if(exec(s,"BEGIN IMMEDIATE"))return -1;
    int rc=credential_update(s,local_id,CRED_MISSING,"SDK template missing",-1,kind,-1);
    if(!rc)rc=audit(s,local_id,"template_missing","re-enrollment required");
    return finish(s,rc);
}
static int record(UserStore *s,const AppUser *u,float score,uint64_t ms,unsigned kind)
{
    const int trace=getenv("KY50_TRACE_STALL")!=NULL;
    struct timespec stamp;uint64_t ticks[7]={0};
#define RECORD_TICK(n) do { if(trace) { clock_gettime(CLOCK_MONOTONIC,&stamp); \
    ticks[n]=(uint64_t)stamp.tv_sec*1000000+stamp.tv_nsec/1000; } } while(0)
    RECORD_TICK(0);
    if(exec(s,"BEGIN IMMEDIATE"))return -1;
    RECORD_TICK(1);
    /* Keep unexported attendance intact. Only acknowledged/exported rows may
     * make room; a full backlog produces a visible save failure. */
    if(exec(s,"DELETE FROM attendance_events WHERE id IN(SELECT id FROM attendance_events WHERE exported=1 "
        "ORDER BY id LIMIT max(0,(SELECT count(*) FROM attendance_events)-9999))"))return finish(s,-1);
    RECORD_TICK(2);
    sqlite3_stmt *q=NULL;
    if(prepare(s,&q,"SELECT count(*) FROM attendance_events"))return finish(s,-1);
    int rows=sqlite3_step(q)==SQLITE_ROW?sqlite3_column_int(q,0):-1;sqlite3_finalize(q);
    if(rows<0||rows>=10000){snprintf(s->error,sizeof(s->error),"attendance backlog full or unavailable");fprintf(stderr,"USER_DB %s\n",s->error);return finish(s,-1);}
    RECORD_TICK(3);
    int rc=prepare(s,&q,"INSERT INTO attendance_events(user_id,local_id,employee_id,name,result,score,created_utc,time_valid,monotonic_ms,method,template_uuid) VALUES(?,?,?,?,?,?,?,?,?,?,?)");
    time_t now=time(NULL);
    if(!rc) {
        if(u) {sqlite3_bind_int64(q,1,u->record_id);sqlite3_bind_int(q,2,u->local_id);}else{sqlite3_bind_null(q,1);sqlite3_bind_null(q,2);}
        sqlite3_bind_text(q,3,u?u->employee_id:"",-1,SQLITE_TRANSIENT);sqlite3_bind_text(q,4,u?u->name:"",-1,SQLITE_TRANSIENT);
        sqlite3_bind_text(q,5,u?"matched":"unmatched",-1,SQLITE_STATIC);sqlite3_bind_double(q,6,score);
        sqlite3_bind_int64(q,7,now);sqlite3_bind_int(q,8,now>=1704067200);sqlite3_bind_int64(q,9,ms);sqlite3_bind_text(q,10,modality_name(kind),-1,SQLITE_STATIC);
        int template_uuid=u?(kind==CREDENTIAL_FINGER?u->finger_uuid:kind==CREDENTIAL_FACE?u->face_uuid:u->palm_uuid):-1;
        if(template_uuid>=0)sqlite3_bind_int(q,11,template_uuid);else sqlite3_bind_null(q,11);
        rc=done(s,q);
    }
    RECORD_TICK(4);
    if(!rc&&u) {
        rc=prepare(s,&q,"UPDATE users SET last_seen_utc=?,last_score=?,recognition_count=recognition_count+1 WHERE id=?");
        if(!rc){sqlite3_bind_int64(q,1,now);sqlite3_bind_double(q,2,score);sqlite3_bind_int64(q,3,u->record_id);rc=done(s,q);}
    }
    RECORD_TICK(5);
    rc=finish(s,rc);
    RECORD_TICK(6);
#undef RECORD_TICK
    if(trace)printf("STORE_STAGE modality=%s begin_us=%llu prune_us=%llu count_us=%llu insert_us=%llu update_us=%llu commit_us=%llu rows=%d result=%d\n",
        modality_name(kind),(unsigned long long)(ticks[1]-ticks[0]),(unsigned long long)(ticks[2]-ticks[1]),
        (unsigned long long)(ticks[3]-ticks[2]),(unsigned long long)(ticks[4]-ticks[3]),
        (unsigned long long)(ticks[5]-ticks[4]),(unsigned long long)(ticks[6]-ticks[5]),rows,rc);
    return rc;
}
int store_record_match(UserStore *s,const AppUser *user,float score,uint64_t ms){return record(s,user,score,ms,CREDENTIAL_PALM);}
int store_record_unknown(UserStore *s,uint64_t ms){return record(s,NULL,0,ms,CREDENTIAL_PALM);}

int store_prepare_enroll(UserStore *s,uint16_t id,uint16_t template_uuid){return store_prepare_enroll_kind(s,id,CREDENTIAL_PALM,template_uuid);}
int store_finish_enroll(UserStore *s,uint16_t id,int side){return store_finish_enroll_kind(s,id,side,CREDENTIAL_PALM);}
int store_cancel_enroll(UserStore *s,uint16_t id,const char *why){return store_cancel_enroll_kind(s,id,why,CREDENTIAL_PALM);}
int store_mark_missing(UserStore *s,uint16_t id){return store_mark_missing_kind(s,id,CREDENTIAL_PALM);}
int store_record_match_kind(UserStore *s,const AppUser *u,float score,uint64_t ms,unsigned kind){return record(s,u,score,ms,kind);}
int store_record_unknown_kind(UserStore *s,uint64_t ms,unsigned kind){return record(s,NULL,0,ms,kind);}
