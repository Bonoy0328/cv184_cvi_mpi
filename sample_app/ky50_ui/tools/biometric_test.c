#include "access.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static uint8_t hands[APP_MAX_USERS],faces[APP_MAX_USERS];
static int hand_feats,face_feats,hand_auth,face_auth,hand_enrolls,face_enrolls;
static int hand_quality,face_quality_status,face_remove_error;
static int h_picks,f_picks,h_match_id,f_match_id;
static int forced_hand_pick=-2;
static FaceParameters face_settings={50,30,50,1,1,24,24};
static int settings_calls,settings_fail;
static int rgb_live_fail,rgb_live_checks;
static int f_settings_get(FaceParameters *p){*p=face_settings;return 0;}
static int f_settings_set(const FaceParameters *p)
{
    ++settings_calls;face_settings=*p;
    if(settings_fail){--settings_fail;return -1;}return 0;
}
static int h_pick(void){++h_picks;if(forced_hand_pick!=-2)return forced_hand_pick;for(int i=0;i<APP_TEMPLATE_SLOTS;i++)if(!hands[i])return i;return UINT16_MAX;}
static int f_pick(void){++f_picks;for(int i=0;i<APP_TEMPLATE_SLOTS;i++)if(!faces[i])return i;return UINT16_MAX;}
static int f_complete(uint16_t id){return faces[id]==1;}
static int h_ids(uint16_t *out,int cap){int n=0;for(int i=0;i<APP_MAX_USERS;i++)if(hands[i]){assert(n<cap);out[n++]=i;}return n;}
static int f_ids(uint16_t *out,int cap){int n=0;for(int i=0;i<APP_MAX_USERS;i++)if(faces[i]){assert(n<cap);out[n++]=i;}return n;}
static int h_exists(uint16_t id){return hands[id]!=0;}
static int f_exists(uint16_t id){return faces[id]!=0;}
static int h_remove(uint16_t id){hands[id]=0;return 0;}
static int f_remove(uint16_t id){if(face_remove_error)return -1;faces[id]=0;return 0;}
static int clear(void){return 0;}
static int h_capacity(void){return 3;}
static int f_capacity(void){return 1;}
static int h_features(uint8_t *y,uint16_t w,uint16_t h,HandPoseRes_t *p,uint8_t *feat)
{
    assert(y[0]==37&&w==540&&h==960);++hand_feats;
    memset(feat,7,APP_FEATURE_BYTES);p->status=hand_quality;return hand_quality;
}
static int f_features(uint8_t *nv12,uint16_t w,uint16_t h,FacePoseRes_t *p,uint8_t *feat)
{
    assert(nv12[0]==89&&nv12[w*h]==51&&nv12[w*h+1]==203&&w==540&&h==960);
    ++face_feats;memset(feat,9,APP_FEATURE_BYTES);p->status=face_quality_status;p->sharpness=.8f;
    if(p->status==STATUS_OK&&face_settings.liveness_enable) {
        ++rgb_live_checks;if(rgb_live_fail)p->status=STATUS_FACE_LIVENESS_FAILED;
    }
    return p->status;
}
static int h_enroll(User_For_C_t *u,uint8_t *feat,ResgisRes_t *p)
{
    assert(feat[0]==7);++hand_enrolls;p->total=3;
    p->status=++p->progress==3?STATUS_REGIS_SUCCESS:STATUS_IN_REGISING;
    if(p->status==STATUS_REGIS_SUCCESS)hands[u->uuid]=1;
    return 0;
}
static int f_enroll(User_For_C_t *u,uint8_t *feat,float quality,ResgisRes_t *p)
{
    assert(feat[0]==9&&quality==.8f);++face_enrolls;faces[u->uuid]=1;
    *p=(ResgisRes_t){.status=STATUS_REGIS_SUCCESS,.progress=1,.total=1};return 0;
}
static int h_auth(uint8_t *feat,User_For_C_t *u){assert(feat[0]==7);++hand_auth;u->uuid=h_match_id;u->targetDist=.9f;return hands[h_match_id];}
static int f_auth(uint8_t *feat,User_For_C_t *u){assert(feat[0]==9);++face_auth;u->uuid=f_match_id;u->targetDist=.9f;return faces[f_match_id];}
static const PalmBackend palm={.ids=h_ids,.exists=h_exists,.complete=h_exists,.remove=h_remove,.clear=clear,
    .capacity=h_capacity,.features=h_features,.enroll=h_enroll,.recognize=h_auth,.pick_free=h_pick};
static int ir_live_pass=1,ir_live_calls;
static int f_live_ir(uint8_t *image,uint16_t w,uint16_t h)
{
    assert(w==540&&h==960&&image[0]==37);
    for(size_t i=(size_t)w*h;i<(size_t)w*h*3/2;i++)assert(image[i]==128);
    ++ir_live_calls;return ir_live_pass;
}
static const FaceBackend face={f_ids,f_exists,f_complete,f_remove,clear,f_capacity,f_features,f_enroll,f_auth,f_pick,f_settings_get,f_settings_set,NULL,NULL,NULL,f_live_ir,NULL};
static AccessSnapshot snap;
static uint64_t ms=10000;
static uint8_t ir[540*960],nv12[540*960*3/2];
static void frame(int hand,int face_found)
{
    HandPoseRes_t h={0};FacePoseRes_t f={0};ms+=200;
    access_biometrics_frame(ir,nv12,540,960,&h,hand,&f,face_found,1,ms);
    access_snapshot(&snap,0);
}
static void command(AccessCommand c){assert(!access_submit(&c));access_poll(ms+=200);access_snapshot(&snap,0);}
static int scalar(sqlite3 *db,const char *sql)
{
    sqlite3_stmt *q=NULL;assert(sqlite3_prepare_v2(db,sql,-1,&q,NULL)==SQLITE_OK);
    assert(sqlite3_step(q)==SQLITE_ROW);int n=sqlite3_column_int(q,0);sqlite3_finalize(q);return n;
}
static AppUser get_user(UserStore *view,int id)
{
    AppUser u;assert(!store_get(view,id,&u));return u;
}
static uint16_t create_user(const char *employee)
{
    AccessCommand c={.type=ACCESS_CREATE};snprintf(c.employee_id,sizeof(c.employee_id),"%s",employee);
    command(c);assert(snap.event==ACCESS_EVENT_CREATED);return snap.selected_id;
}
static void enroll_face(uint16_t local_id)
{
    int picks=f_picks;
    command((AccessCommand){.type=ACCESS_ENROLL,.local_id=local_id,.modality=CREDENTIAL_FACE});
    assert(snap.registering&&f_picks==picks+1);frame(0,1);
    assert(!snap.registering&&snap.event==ACCESS_EVENT_ENROLLED&&f_picks==picks+1);
}
static void enroll_palm(uint16_t local_id)
{
    int picks=h_picks;
    command((AccessCommand){.type=ACCESS_ENROLL,.local_id=local_id,.modality=CREDENTIAL_PALM});
    assert(snap.registering&&h_picks==picks+1);
    for(int i=0;i<4;i++)frame(1,1);
    assert(!snap.registering&&snap.event==ACCESS_EVENT_ENROLLED&&h_picks==picks+1);
}
static void clean_dir(const char *dir)
{
    char path[256];snprintf(path,sizeof(path),"%s/users.sqlite3",dir);assert(!unlink(path));
    snprintf(path,sizeof(path),"%s/app.conf",dir);assert(!unlink(path));
    snprintf(path,sizeof(path),"%s/avatars",dir);assert(!rmdir(path));assert(!rmdir(dir));
}
static void discard_old_data(void)
{
    char dir[]="/tmp/ky50-ids-reset-XXXXXX",path[256];assert(mkdtemp(dir));
    snprintf(path,sizeof(path),"%s/users.sqlite3",dir);sqlite3 *db=NULL;
    assert(sqlite3_open(path,&db)==SQLITE_OK);
    assert(!sqlite3_exec(db,"CREATE TABLE users(id INTEGER PRIMARY KEY,uuid INTEGER); INSERT INTO users VALUES(1,55);"
        "CREATE TABLE credentials(user_id INTEGER,kind TEXT,template_uuid INTEGER); INSERT INTO credentials VALUES(1,'palm',55);"
        "CREATE TABLE operations(uuid INTEGER,kind TEXT); INSERT INTO operations VALUES(55,'enroll');"
        "CREATE TABLE attendance_events(user_id INTEGER); INSERT INTO attendance_events VALUES(1);"
        "CREATE TABLE audit_events(user_id INTEGER); INSERT INTO audit_events VALUES(1);"
        "CREATE TABLE device_meta(key TEXT PRIMARY KEY,value TEXT); INSERT INTO device_meta VALUES('schema','2');"
        "INSERT INTO device_meta VALUES('pva.ir_ae_target.v1','56'); PRAGMA user_version=2;",NULL,NULL,NULL));
    sqlite3_close(db);memset(hands,0,sizeof(hands));memset(faces,0,sizeof(faces));hands[55]=1;faces[77]=1;
    face_remove_error=1;
    assert(!access_open_biometrics(dir,&palm,&face));access_snapshot(&snap,0);
    assert(!snap.ready&&!snap.count&&!hands[55]&&faces[77]);access_close();
    UserStore view;assert(!store_open(&view,path));
    assert(scalar(view.db,"PRAGMA user_version")==4&&store_templates_reset_pending(&view)==1);
    assert(!scalar(view.db,"SELECT count(*) FROM users")&&!scalar(view.db,"SELECT count(*) FROM credentials"));
    assert(!scalar(view.db,"SELECT count(*) FROM attendance_events")&&!scalar(view.db,"SELECT count(*) FROM audit_events"));
    int target=0;assert(!store_ir_target_load(&view,&target)&&target==56);store_close(&view);
    assert(!access_open_biometrics(dir,NULL,NULL));access_snapshot(&snap,0);assert(!snap.ready);access_close();
    face_remove_error=0;
    assert(!access_open_biometrics(dir,&palm,&face));access_snapshot(&snap,0);
    assert(snap.ready&&!snap.count&&!faces[77]&&!hands[55]);
    assert(create_user("900")==0);access_enable_recognition(0);enroll_face(0);access_close();
    assert(!access_open_biometrics(dir,&palm,&face));access_snapshot(&snap,0);
    assert(snap.count==1&&faces[0]&&snap.users[0].face_uuid==0);access_close();
    assert(!store_open(&view,path));assert(!store_templates_reset_pending(&view));store_close(&view);
    clean_dir(dir);puts("PASS old-data discard: legacy profiles/events and both SDK databases cleared, partial failure marker survives restart, no reset of new registrations or device parameters");
}
static void parameter_checks(void)
{
    char dir[]="/tmp/ky50-face-settings-XXXXXX",path[256];assert(mkdtemp(dir));
    assert(!access_open_biometrics(dir,&palm,&face));access_snapshot(&snap,0);
    assert(snap.face_parameters_ready&&snap.face_parameters.detection_threshold==50);
    assert(snap.app_parameters.match_confirmations==1&&snap.app_parameters.success_cooldown_seconds==0);
    assert(snap.app_parameters_ready&&snap.app_parameters.palm_leave_frames==5&&snap.app_parameters.face_leave_frames==5&&snap.app_parameters.relay_seconds==5);
    assert(snap.app_parameters.strong_liveness==1);
    command((AccessCommand){.type=ACCESS_APP_PARAMETERS,.app_parameters={5,5,5,1,0,0}});
    AccessCommand c={.type=ACCESS_FACE_PARAMETERS,.face_parameters={57,41,65,0,0,-12,-3}};
    command(c);assert(!snap.face_parameters_result&&!memcmp(&face_settings,&c.face_parameters,sizeof(face_settings)));
    snprintf(path,sizeof(path),"%s/users.sqlite3",dir);UserStore view;assert(!store_open(&view,path));
    FaceParameters saved;assert(!store_face_parameters_load(&view,&saved)&&saved.auth_threshold==-12);
    PalmParameters palm_saved;assert(store_parameters_load(&view,&palm_saved)==1);
    AppParameters app;assert(!app_config_load(dir,&app)&&app.palm_leave_frames==5&&app.face_leave_frames==5);
    AccessCommand invalid[]={
        {.face_parameters={0,41,65,0,0,-12,-3}}, {.face_parameters={101,41,65,0,0,-12,-3}},
        {.face_parameters={57,-1,65,0,0,-12,-3}}, {.face_parameters={57,41,101,0,0,-12,-3}},
        {.face_parameters={57,41,65,2,0,-12,-3}}, {.face_parameters={57,41,65,0,-1,-12,-3}},
        {.face_parameters={57,41,65,0,0,-101,-3}}, {.face_parameters={57,41,65,0,0,-12,101}}
    };
    int calls=settings_calls;
    for(unsigned i=0;i<sizeof(invalid)/sizeof(*invalid);i++) {
        invalid[i].type=ACCESS_FACE_PARAMETERS;command(invalid[i]);assert(snap.face_parameters_result&&settings_calls==calls);
    }
    assert(!sqlite3_exec(view.db,"CREATE TRIGGER fail_face BEFORE UPDATE ON device_meta "
        "WHEN NEW.key='pva.face.parameters.v1' BEGIN SELECT RAISE(ABORT,'test failure'); END;",NULL,NULL,NULL));
    c.face_parameters.detection_threshold=58;command(c);
    assert(snap.face_parameters_result&&face_settings.detection_threshold==57);
    assert(!store_face_parameters_load(&view,&saved)&&saved.detection_threshold==57);
    assert(!sqlite3_exec(view.db,"DROP TRIGGER fail_face",NULL,NULL,NULL));
    settings_fail=1;command(c);assert(snap.face_parameters_result&&face_settings.detection_threshold==57);
    assert(!store_face_parameters_load(&view,&saved)&&saved.detection_threshold==57);
    c.face_parameters=(FaceParameters){1,0,100,1,0,-100,100};command(c);assert(!snap.face_parameters_result);
    c.face_parameters=(FaceParameters){100,100,0,0,1,100,-100};command(c);assert(!snap.face_parameters_result);
    AccessCommand change={.type=ACCESS_APP_PARAMETERS,.app_parameters={3,7,8,2,0,0}};
    command(change);assert(!snap.app_parameters_result&&snap.app_parameters.palm_leave_frames==3&&snap.app_parameters.face_leave_frames==7);
    assert(!app_config_load(dir,&app)&&app.palm_leave_frames==3&&app.face_leave_frames==7);
    assert(!store_face_parameters_load(&view,&saved)&&saved.detection_threshold==100);
    assert(snap.app_parameters.relay_seconds==8&&app.relay_seconds==8);
    change.app_parameters.success_cooldown_seconds=-1;command(change);assert(snap.app_parameters_result&&snap.app_parameters.success_cooldown_seconds==0);
    change.app_parameters.success_cooldown_seconds=61;command(change);assert(snap.app_parameters_result&&snap.app_parameters.success_cooldown_seconds==0);
    change.app_parameters.success_cooldown_seconds=60;command(change);assert(!snap.app_parameters_result&&snap.app_parameters.success_cooldown_seconds==60);
    change.app_parameters.success_cooldown_seconds=0;command(change);
    change.app_parameters.match_confirmations=0;command(change);assert(snap.app_parameters_result&&snap.app_parameters.match_confirmations==2);
    change.app_parameters.match_confirmations=11;command(change);assert(snap.app_parameters_result&&snap.app_parameters.match_confirmations==2);
    change.app_parameters.match_confirmations=10;command(change);assert(!snap.app_parameters_result&&snap.app_parameters.match_confirmations==10);
    change.app_parameters.match_confirmations=2;command(change);
    change.app_parameters.relay_seconds=0;command(change);assert(snap.app_parameters_result&&snap.app_parameters.relay_seconds==8);
    change.app_parameters.relay_seconds=61;command(change);assert(snap.app_parameters_result&&snap.app_parameters.relay_seconds==8);
    change.app_parameters.relay_seconds=5;
    change.app_parameters.palm_leave_frames=0;command(change);assert(snap.app_parameters_result&&snap.app_parameters.palm_leave_frames==3);
    change.app_parameters=(AppParameters){1,101,5,2,0,0};command(change);assert(snap.app_parameters_result);
    snprintf(path,sizeof(path),"%s/app.conf.tmp",dir);assert(!symlink("app.conf",path));
    change.app_parameters=(AppParameters){1,100,5,2,0,0};command(change);assert(snap.app_parameters_result&&snap.app_parameters.face_leave_frames==7);
    assert(!app_config_load(dir,&app)&&app.palm_leave_frames==3&&app.face_leave_frames==7);
    command(change);assert(!snap.app_parameters_result);
    store_close(&view);access_close();face_settings=(FaceParameters){50,30,50,1,1,24,24};
    assert(!access_open_biometrics(dir,&palm,&face));access_snapshot(&snap,0);
    assert(snap.face_parameters_ready&&face_settings.filter_threshold==-100&&face_settings.liveness_enable==1);
    assert(snap.app_parameters.palm_leave_frames==1&&snap.app_parameters.face_leave_frames==100);
    access_close();
    snprintf(path,sizeof(path),"%s/app.conf",dir);FILE *f=fopen(path,"w");assert(f);
    fputs("version=1\npalm_leave_frames=3\nface_leave_frames=3\n",f);fclose(f);
    assert(app_config_load(dir,&app)==1&&app.palm_leave_frames==3&&app.face_leave_frames==3&&app.relay_seconds==5);
    assert(!access_open_biometrics(dir,&palm,&face));access_snapshot(&snap,0);
    assert(snap.app_parameters_ready&&snap.app_parameters.palm_leave_frames==3&&snap.app_parameters.relay_seconds==5);
    assert(!app_config_load(dir,&app));access_close();
    f=fopen(path,"w");assert(f);
    fputs("version=2\npalm_leave_frames=3\nface_leave_frames=3\nrelay_seconds=1\n",f);fclose(f);
    assert(app_config_load(dir,&app)==1&&app.match_confirmations==1&&app.relay_seconds==1);
    assert(!app_config_save(dir,&app)&&!app_config_load(dir,&app)&&app.match_confirmations==1);
    f=fopen(path,"w");assert(f);
    fputs("version=3\npalm_leave_frames=3\nface_leave_frames=3\nrelay_seconds=1\n",f);fclose(f);
    assert(app_config_load(dir,&app)<0); /* Missing confirmation count is malformed v3. */
    f=fopen(path,"w");assert(f);
    fputs("version=2\npalm_leave_frames=3\nface_leave_frames=3\n",f);fclose(f);
    assert(app_config_load(dir,&app)<0); /* A truncated v2 must not guess a duration. */
    f=fopen(path,"w");assert(f);
    fputs("version=1\npalm_leave_frames=0\nface_leave_frames=5\n",f);fclose(f);
    assert(!access_open_biometrics(dir,&palm,&face));access_snapshot(&snap,0);assert(!snap.ready&&!snap.app_parameters_ready);
    access_close();clean_dir(dir);face_settings=(FaceParameters){50,30,50,1,1,24,24};
    puts("PASS face/application settings: immediate update, independent persistence, all bounds, restart, invalid config and apply/storage failure rollback");
}
static void rapid_reentry_checks(void)
{
    char dir[]="/tmp/ky50-reentry-XXXXXX";assert(mkdtemp(dir));
    memset(hands,0,sizeof(hands));memset(faces,0,sizeof(faces));
    h_match_id=f_match_id=0;hand_quality=face_quality_status=STATUS_OK;
    assert(!access_open_biometrics(dir,&palm,&face));
    uint16_t id=create_user("777");enroll_face(id);enroll_palm(id);
    command((AccessCommand){.type=ACCESS_APP_PARAMETERS,.app_parameters={3,3,1,2,0,0}});
    for(int i=0;i<3;i++)frame(0,0);
    access_enable_recognition(1);
    frame(0,1);assert(snap.success_sequence==0); /* one sample never succeeds */
    frame(0,1);assert(snap.success_sequence==1);uint64_t last_success=snap.success_ms;
    frame(0,1);frame(0,1);assert(snap.success_sequence==1);
    for(int i=0;i<3;i++)frame(0,0);
    frame(0,1);assert(snap.success_sequence==1);
    frame(0,1);assert(snap.success_sequence==2&&snap.success_ms-last_success<3000);
    last_success=snap.success_ms;
    /* A face success must not impose a global delay on a new palm session. */
    frame(1,1);frame(1,1);assert(snap.success_sequence==2);
    frame(1,1);assert(snap.success_sequence==3&&snap.success_modality==CREDENTIAL_PALM&&snap.success_ms-last_success<3000);
    last_success=snap.success_ms;
    frame(1,1);frame(1,1);assert(snap.success_sequence==3);
    for(int i=0;i<3;i++)frame(0,0);
    frame(1,0);frame(1,0);assert(snap.success_sequence==3);
    frame(1,0);assert(snap.success_sequence==4&&snap.success_ms-last_success<3000);
    /* A live change to one match applies to both modalities, but does not
     * release an already successful target still in view. */
    command((AccessCommand){.type=ACCESS_APP_PARAMETERS,.app_parameters={3,3,1,1,0,0}});
    frame(1,0);assert(snap.success_sequence==4);
    for(int i=0;i<3;i++)frame(0,0);
    frame(0,1);assert(snap.success_sequence==5&&snap.success_modality==CREDENTIAL_FACE);
    frame(0,1);assert(snap.success_sequence==5);
    frame(1,1);assert(snap.success_sequence==5);
    frame(1,1);assert(snap.success_sequence==6&&snap.success_modality==CREDENTIAL_PALM);
    /* Raising the shared count discards partial confirmations immediately. */
    for(int i=0;i<3;i++)frame(0,0);
    command((AccessCommand){.type=ACCESS_APP_PARAMETERS,.app_parameters={3,3,1,3,0,0}});
    frame(0,1);assert(snap.success_sequence==6);
    command((AccessCommand){.type=ACCESS_APP_PARAMETERS,.app_parameters={3,3,1,2,0,0}});
    frame(0,1);assert(snap.success_sequence==6);
    frame(0,1);assert(snap.success_sequence==7);
    /* Shared success cooldown: face success delays palm, live zero releases
     * the wait on the next eligible sample without releasing success holds. */
    last_success=snap.success_ms;
    command((AccessCommand){.type=ACCESS_APP_PARAMETERS,.app_parameters={3,3,1,1,3,0}});
    frame(1,1);frame(1,1);assert(snap.success_sequence==7&&ms-last_success<3000);
    command((AccessCommand){.type=ACCESS_APP_PARAMETERS,.app_parameters={3,3,1,1,0,0}});
    frame(1,1);assert(snap.success_sequence==8&&snap.success_ms-last_success<3000);
    last_success=snap.success_ms;
    command((AccessCommand){.type=ACCESS_APP_PARAMETERS,.app_parameters={3,3,1,1,3,0}});
    for(int i=0;i<3;i++)frame(0,0);
    while(ms+200-last_success<3000){frame(0,1);assert(snap.success_sequence==8);}
    frame(0,1);assert(snap.success_sequence==9&&snap.success_ms-last_success>=3000);
    command((AccessCommand){.type=ACCESS_APP_PARAMETERS,.app_parameters={3,3,1,1,0,0}});
    frame(0,1);assert(snap.success_sequence==9);
    access_close();clean_dir(dir);
    puts("PASS rapid reentry: face and palm repeat below 3s after departure, cross-modality has no cooldown, held sessions stay latched, shared confirmation count and cooldown apply live, held sessions never repeat");
}
static void strong_live_checks(void)
{
    char dir[]="/tmp/ky50-strong-test-XXXXXX",path[256];assert(mkdtemp(dir));
    memset(hands,0,sizeof(hands));memset(faces,0,sizeof(faces));
    h_match_id=f_match_id=0;hand_quality=face_quality_status=STATUS_OK;ir_live_calls=0;ir_live_pass=0;
    face_settings=(FaceParameters){50,30,50,1,1,24,24};
    assert(!access_open_biometrics(dir,&palm,&face));access_snapshot(&snap,0);
    assert(snap.app_parameters.strong_liveness==1);
    uint16_t id=create_user("888");enroll_face(id);enroll_palm(id);assert(ir_live_calls==0);
    command((AccessCommand){.type=ACCESS_APP_PARAMETERS,.app_parameters={3,3,1,1,0,1}});
    FaceParameters disabled=face_settings;disabled.liveness_enable=0;
    command((AccessCommand){.type=ACCESS_FACE_PARAMETERS,.face_parameters=disabled});assert(!snap.face_parameters_result);
    assert(snap.app_parameters.strong_liveness); /* RGB off does not change IR */
    for(int i=0;i<3;i++)frame(0,0);
    int rgb_before=rgb_live_checks;rgb_live_fail=1;
    access_enable_recognition(1);frame(0,1);frame(0,1);
    assert(ir_live_calls==2&&snap.success_sequence==0&&rgb_live_checks==rgb_before);
    command((AccessCommand){.type=ACCESS_APP_PARAMETERS,.app_parameters={3,3,1,1,0,0}});
    int calls=ir_live_calls;frame(0,1);
    assert(snap.success_sequence==1&&ir_live_calls==calls&&rgb_live_checks==rgb_before); /* both off */
    disabled.liveness_enable=1;command((AccessCommand){.type=ACCESS_FACE_PARAMETERS,.face_parameters=disabled});
    assert(!snap.face_parameters_result&&!snap.app_parameters.strong_liveness);
    for(int i=0;i<3;i++)frame(0,0);
    frame(0,1);assert(snap.success_sequence==1&&rgb_live_checks==rgb_before+1&&ir_live_calls==calls);
    rgb_live_fail=0;frame(0,1);assert(snap.success_sequence==2&&ir_live_calls==calls); /* RGB only */
    command((AccessCommand){.type=ACCESS_APP_PARAMETERS,.app_parameters={3,3,1,1,0,1}});
    assert(!snap.app_parameters_result&&snap.face_parameters.liveness_enable);ir_live_pass=1;
    frame(0,1);assert(snap.success_sequence==2); /* enabling cannot release held success */
    for(int i=0;i<3;i++)frame(0,0);
    rgb_live_fail=1;frame(0,1);assert(snap.success_sequence==2&&ir_live_calls==calls);
    rgb_live_fail=0;frame(0,1);assert(snap.success_sequence==3&&ir_live_calls==calls+1); /* both on */
    disabled.liveness_enable=0;command((AccessCommand){.type=ACCESS_FACE_PARAMETERS,.face_parameters=disabled});
    assert(!snap.face_parameters_result&&snap.app_parameters.strong_liveness);
    for(int i=0;i<3;i++)frame(0,0);
    rgb_before=rgb_live_checks;rgb_live_fail=1;frame(0,1);
    assert(snap.success_sequence==4&&rgb_live_checks==rgb_before); /* IR only */
    calls=ir_live_calls;ir_live_pass=0;frame(1,1);frame(1,1);
    assert(snap.success_sequence==5&&snap.success_modality==CREDENTIAL_PALM&&ir_live_calls==calls);
    for(int i=0;i<3;i++)frame(0,0);
    f_match_id=1;frame(0,1);assert(ir_live_calls==calls);f_match_id=0;
    AppParameters cfg=snap.app_parameters;cfg.strong_liveness=2;
    command((AccessCommand){.type=ACCESS_APP_PARAMETERS,.app_parameters=cfg});assert(snap.app_parameters_result);
    access_close();assert(!access_open_biometrics(dir,&palm,&face));access_snapshot(&snap,0);
    assert(snap.app_parameters.strong_liveness==1&&!snap.face_parameters.liveness_enable);access_close();
    snprintf(path,sizeof(path),"%s/app.conf",dir);FILE *f=fopen(path,"w");assert(f);
    fputs("version=3\npalm_leave_frames=3\nface_leave_frames=7\nrelay_seconds=2\nmatch_confirmations=2\nsuccess_cooldown_seconds=3\n",f);fclose(f);
    AppParameters old;assert(app_config_load(dir,&old)==1&&old.strong_liveness==1&&old.face_leave_frames==7&&old.relay_seconds==2&&old.match_confirmations==2&&old.success_cooldown_seconds==3);
    f=fopen(path,"w");assert(f);
    fputs("version=4\npalm_leave_frames=3\nface_leave_frames=3\nrelay_seconds=1\nmatch_confirmations=1\nsuccess_cooldown_seconds=0\n",f);fclose(f);
    assert(app_config_load(dir,&old)<0);clean_dir(dir);ir_live_pass=1;rgb_live_fail=0;
    puts("PASS strong live: default on, full neutral-UV NV12, IR false blocks success, independent RGB/IR four combinations, restart IR-only, palm/enrollment unaffected, held success, v3 upgrade and v4 validation");
}
int main(void)
{
    parameter_checks();
    char dir[]="/tmp/ky50-biometric-XXXXXX",path[256];assert(mkdtemp(dir));
    snprintf(path,sizeof(path),"%s/users.sqlite3",dir);
    memset(ir,37,sizeof(ir));memset(nv12,89,sizeof(nv12));nv12[540*960]=51;nv12[540*960+1]=203;
    assert(!access_open_biometrics(dir,&palm,&face));
    command((AccessCommand){.type=ACCESS_APP_PARAMETERS,.app_parameters={5,5,5,2,0,0}});
    assert(create_user("001")==0&&create_user("002")==1&&create_user("003")==2);
    assert(!h_picks&&!f_picks); /* Creating a profile never reserves a biometric ID. */
    command((AccessCommand){.type=ACCESS_ENROLL,.local_id=1,.modality=CREDENTIAL_FACE});
    assert(snap.registering&&snap.total==1&&f_picks==1);
    int settings_before=settings_calls;
    command((AccessCommand){.type=ACCESS_FACE_PARAMETERS,.face_parameters={60,40,60,0,0,30,30}});
    assert(snap.face_parameters_result&&settings_calls==settings_before&&snap.registering);
    command((AccessCommand){.type=ACCESS_APP_PARAMETERS,.app_parameters={2,9,5,2,0,0}});
    assert(snap.app_parameters_result&&snap.app_parameters.palm_leave_frames==5);
    frame(1,1);frame(1,1);assert(!face_feats&&!face_enrolls&&!hand_enrolls);
    frame(-1,1);assert(!face_feats);
    face_quality_status=STATUS_FACE_LIVENESS_FAILED;frame(0,1);
    assert(face_feats==1&&!face_enrolls&&snap.status==STATUS_FACE_LIVENESS_FAILED);
    face_quality_status=STATUS_FACE_LOW_QUALITY;frame(0,1);assert(!face_enrolls&&snap.progress==0);
    face_quality_status=STATUS_OK;frame(0,1);
    assert(face_enrolls==1&&!snap.registering&&faces[0]&&!faces[1]&&f_picks==1);
    command((AccessCommand){.type=ACCESS_ENROLL,.local_id=0});assert(snap.total==3&&h_picks==1);
    int before=face_feats;frame(0,1);assert(face_feats==before);
    frame(1,1);frame(1,1);frame(1,1);frame(1,1);
    assert(!snap.registering&&hand_enrolls==3&&faces[0]&&hands[0]&&h_picks==1);
    UserStore view;assert(!store_open(&view,path));AppUser a=get_user(&view,0),b=get_user(&view,1),mapped;
    assert(a.palm_uuid==0&&a.face_uuid==-1&&b.face_uuid==0&&b.palm_uuid==-1);
    assert(!store_get_by_template(&view,CREDENTIAL_FACE,0,&mapped)&&mapped.local_id==1);
    assert(!store_get_by_template(&view,CREDENTIAL_PALM,0,&mapped)&&mapped.local_id==0);
    assert(!snap.success_sequence&&!snap.verified_modalities); /* Enrollment cannot unlock. */
    access_enable_recognition(1);
    int hf=hand_feats,ff=face_feats,ha=hand_auth,fa=face_auth;
    frame(0,0);assert(hand_feats==hf&&face_feats==ff&&hand_auth==ha&&face_auth==fa);
    frame(1,1);assert(hand_auth==ha+1&&face_auth==fa&&face_feats==ff);
    hand_quality=STATUS_TOO_DARK;frame(1,1);assert(face_auth==fa&&face_feats==ff&&hand_auth==ha+1);
    frame(-1,1);assert(face_feats==ff);hand_quality=STATUS_OK;
    frame(0,1);assert(!snap.success_sequence&&!snap.verified_modalities);
    frame(0,1);assert(face_auth==fa+2&&snap.matched==1);
    assert(snap.success_sequence==1&&snap.success_modality==CREDENTIAL_FACE&&snap.success_relay_seconds==5);
    assert(snap.verified_modalities==CREDENTIAL_FACE);
    assert(scalar(view.db,"SELECT count(*) FROM attendance_events WHERE method='face' AND local_id=1 AND template_uuid=0 AND employee_id='002'")==1);
    ff=face_feats;frame(0,1);assert(face_feats==ff&&snap.success_sequence==1);
    for(int i=0;i<4;i++)frame(0,0);
    frame(0,1);assert(face_feats==ff); /* Four misses must retain successful face. */
    for(int i=0;i<4;i++)frame(0,0);
    frame(0,-1); /* Error breaks the streak, not a fifth miss. */
    for(int i=0;i<4;i++)frame(0,0);
    frame(0,1);assert(face_feats==ff);
    for(int i=0;i<APP_DEFAULT_LEAVE_FRAMES;i++)frame(0,0);
    assert(!(snap.verified_modalities&CREDENTIAL_FACE));
    fa=face_auth;frame(0,1);frame(0,1);assert(face_auth==fa+2); /* Fifth miss unlocks without restart. */
    fa=face_auth;ff=face_feats;
    command((AccessCommand){.type=ACCESS_APP_PARAMETERS,.app_parameters={3,7,5,2,0,0}});
    assert(!snap.app_parameters_result);
    for(int i=0;i<6;i++)frame(0,0);
    frame(0,1);assert(face_auth==fa&&face_feats==ff);
    for(int i=0;i<7;i++)frame(0,0);
    frame(0,1);frame(0,1);assert(face_auth==fa+2); /* Independent face value is seven. */
    unsigned sequence=snap.success_sequence;
    command((AccessCommand){.type=ACCESS_APP_PARAMETERS,.app_parameters={3,7,2,2,0,0}});
    assert(snap.success_sequence==sequence); /* Saving must not repeat a pulse. */
    ms+=3500; /* Existing event rate limit is independent of departure. */
    fa=face_auth;frame(1,1);frame(1,1);frame(1,1);assert(face_auth==fa);
    assert(snap.success_sequence==sequence+1&&snap.success_modality==CREDENTIAL_PALM&&snap.success_relay_seconds==2);
    assert(snap.verified_modalities&CREDENTIAL_PALM);
    assert(scalar(view.db,"SELECT count(*) FROM attendance_events WHERE method='palm' AND local_id=0 AND template_uuid=0 AND employee_id='001'")==1);
    ha=hand_auth;frame(0,1);frame(0,1);frame(1,1);assert(hand_auth==ha);
    frame(0,1);frame(0,1);frame(0,1);frame(1,1);frame(1,1);assert(hand_auth==ha+1);
    puts("PASS live independent departure: default five, four-miss/error retention, face seven and palm three after saving");
    access_enable_recognition(0);enroll_face(0);a=get_user(&view,0);
    assert(a.palm_uuid==0&&a.face_uuid==1&&faces[0]&&faces[1]);
    /* Same numeric SDK UUID can belong to different users. The same user can
     * also have different UUIDs for palm and face. Deletion must follow both. */
    face_remove_error=1;command((AccessCommand){.type=ACCESS_DELETE,.count=1,.ids={0}});
    assert(!hands[0]&&faces[0]&&faces[1]&&get_user(&view,0).deleting);store_close(&view);access_close();
    face_remove_error=0;assert(!access_open_biometrics(dir,&palm,&face));access_snapshot(&snap,0);
    assert(snap.count==2&&!faces[1]&&faces[0]);assert(create_user("004")==0);
    assert(!store_open(&view,path));
    command((AccessCommand){.type=ACCESS_ENROLL,.local_id=0,.modality=CREDENTIAL_FACE});
    assert(snap.registering&&get_user(&view,0).face_uuid==1&&!faces[1]);
    assert(f_pick()==1&&f_pick()==1); /* Query itself does not occupy the slot. */
    command((AccessCommand){.type=ACCESS_CANCEL});
    assert(get_user(&view,0).face_uuid==-1&&get_user(&view,1).face_uuid==0&&faces[0]);
    store_close(&view);access_close();
    /* A crash after SDK write but before metadata commit uses the persisted
     * template ID (7), not the local ID (0), and never allocates again. */
    assert(!store_open(&view,path));assert(!store_prepare_enroll_kind(&view,0,CREDENTIAL_FACE,7));faces[7]=1;
    store_close(&view);int picks=f_picks;
    assert(!access_open_biometrics(dir,&palm,&face));assert(f_picks==picks);access_close();
    assert(!store_open(&view,path));assert(get_user(&view,0).face_uuid==7&&get_user(&view,0).face_state==CRED_REGISTERED);store_close(&view);
    faces[7]=0;
    assert(!access_open_biometrics(dir,&palm,&face));access_close();
    assert(!store_open(&view,path));assert(get_user(&view,0).face_state==CRED_MISSING&&get_user(&view,0).face_uuid==-1);
    assert(!store_prepare_enroll_kind(&view,0,CREDENTIAL_FACE,8));faces[8]=2;store_close(&view);
    assert(!access_open_biometrics(dir,&palm,&face));assert(!faces[8]&&faces[0]);
    /* Full palm DB does not block face registration or creation of profiles. */
    memset(hands,1,sizeof(hands));command((AccessCommand){.type=ACCESS_ENROLL,.local_id=1});
    assert(!snap.registering&&snap.event==ACCESS_EVENT_ERROR);enroll_face(0);assert(faces[1]);
    assert(create_user("005")==3);
    /* Full face DB does not block palm registration. */
    memset(hands,0,sizeof(hands));memset(faces,1,sizeof(faces));enroll_palm(2);assert(hands[0]);
    command((AccessCommand){.type=ACCESS_ENROLL,.local_id=2,.modality=CREDENTIAL_FACE});
    assert(!snap.registering&&snap.event==ACCESS_EVENT_ERROR);
    forced_hand_pick=APP_TEMPLATE_SLOTS;
    command((AccessCommand){.type=ACCESS_ENROLL,.local_id=3});assert(!snap.registering&&snap.event==ACCESS_EVENT_ERROR);
    forced_hand_pick=-2;
    /* SDK slot reuse cannot overwrite another registered SQLite binding. */
    faces[0]=0;command((AccessCommand){.type=ACCESS_ENROLL,.local_id=2,.modality=CREDENTIAL_FACE});
    assert(!snap.registering&&snap.event==ACCESS_EVENT_ERROR);
    assert(!store_open(&view,path));assert(get_user(&view,1).face_uuid==0&&get_user(&view,2).face_uuid==-1);
    store_close(&view);access_close();
    assert(!access_open_biometrics(dir,&palm,&face));enroll_face(2);
    assert(!store_open(&view,path));assert(get_user(&view,1).face_state==CRED_MISSING&&get_user(&view,2).face_uuid==0);
    assert(!store_get_by_template(&view,CREDENTIAL_FACE,0,&mapped)&&mapped.local_id==2);
    assert(scalar(view.db,"PRAGMA user_version")==4);store_close(&view);access_close();
    clean_dir(dir);
    discard_old_data();
    rapid_reentry_checks();
    strong_live_checks();
    puts("PASS independent IDs: SDK allocation once per session, same ID/different users, different IDs/same user, modality-aware auth/events/delete/cancel/recovery, per-modality full/invalid IDs, safe reuse and raw palm priority");
    return 0;
}
