#include "access.h"
#include "ime.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
static unsigned char templates[APP_MAX_USERS];static int remove_fails,quality_status;
static int feature_result,feature_calls,enroll_calls,recognize_calls;
static int ids(uint16_t *out,int max){int n=0;for(int i=0;i<APP_MAX_USERS;i++)if(templates[i]){assert(n<max);out[n++]=i;}return n;}
static int exists(uint16_t id){return templates[id]>0;}
static int complete(uint16_t id){return templates[id]==2;}
static int remove_template(uint16_t id){if(remove_fails)return -1;templates[id]=0;return 0;}
static int clear(void){return 0;}
static int capacity(void){return 3;}
static int features(uint8_t *y,uint16_t w,uint16_t h,HandPoseRes_t *pose,uint8_t *feat)
{
    assert(y[0]==37&&w==540&&h==960&&pose->keypoints[0]==101);
    ++feature_calls;pose->status=quality_status;
    memset(feat,7,APP_FEATURE_BYTES);return feature_result;
}
static int enroll(User_For_C_t *u,uint8_t *feat,ResgisRes_t *p)
{
    ++enroll_calls;assert(feat[0]==7);p->total=3;templates[u->uuid]=1;
    p->status=++p->progress==3?STATUS_REGIS_SUCCESS:STATUS_IN_REGISING;
    if(p->status==STATUS_REGIS_SUCCESS)templates[u->uuid]=2;
    return 0;
}
static int recognize(uint8_t *feat,User_For_C_t *u)
{
    ++recognize_calls;assert(feat[0]==7);for(int i=0;i<APP_MAX_USERS;i++)if(templates[i]==2){u->uuid=i;u->targetDist=.91f;return 1;}return 0;
}
static PalmParameters fake_parameters={76,52,2};
static int parameter_calls,parameter_fail;
static int parameters_get(PalmParameters *p){*p=fake_parameters;return 0;}
static int parameters_set(const PalmParameters *p)
{
    ++parameter_calls;fake_parameters=*p;
    if(parameter_fail){--parameter_fail;return -1;}
    return 0;
}
static int pick_free(void){for(int i=0;i<APP_TEMPLATE_SLOTS;i++)if(!templates[i])return i;return -1;}
static const PalmBackend backend={ids,exists,complete,remove_template,clear,capacity,features,enroll,recognize,parameters_get,parameters_set,pick_free};
static int ir_gets,ir_sets;
static int ir_get(IrExposureReading *r){++ir_gets;*r=(IrExposureReading){.target=56};return 0;}
static int ir_set(int target){(void)target;++ir_sets;return 0;}
static const IrExposureBackend ir_backend={ir_get,ir_set};
/* Synchronous tests still pass real image metadata when AE is attached. */
static int sample_frame(uint8_t *image,uint16_t w,uint16_t h,HandPoseRes_t *pose,int found,int rgb,uint64_t ms)
{
    static uint64_t sequence;
    IrExposureSample sample={.sequence=++sequence,.captured_ms=ms,.target=56,.stable=1,.valid=1};
    access_set_ir_sample(&sample);return access_frame(image,w,h,pose,found,rgb,ms);
}
#define access_frame sample_frame

static AccessSnapshot snapshot;
static void command(AccessCommand *c,uint64_t ms){assert(!access_submit(c));access_poll(ms);access_snapshot(&snapshot,0);}
static int scalar(sqlite3 *db,const char *sql)
{
    sqlite3_stmt *q=NULL;assert(sqlite3_prepare_v2(db,sql,-1,&q,NULL)==SQLITE_OK);assert(sqlite3_step(q)==SQLITE_ROW);
    int v=sqlite3_column_int(q,0);sqlite3_finalize(q);return v;
}
static void parameters_check(void)
{
    char dir[]="/tmp/ky50-parameters-XXXXXX",path[256];assert(mkdtemp(dir));
    snprintf(path,sizeof(path),"%s/users.sqlite3",dir);assert(!access_open(dir,&backend));
    access_snapshot(&snapshot,0);assert(snapshot.parameters_ready&&snapshot.parameters.hand_threshold==76);
    AccessCommand c={.type=ACCESS_PARAMETERS,.parameters={80,60,4}};
    command(&c,1);assert(!snapshot.parameters_result&&fake_parameters.auth_threshold==60);
    access_close();fake_parameters=(PalmParameters){76,52,2};
    assert(!access_open(dir,&backend));access_snapshot(&snapshot,0);
    assert(fake_parameters.hand_threshold==80&&snapshot.parameters.dynamic_level==4);
    int calls=parameter_calls;
    c.parameters.hand_threshold=101;command(&c,2);assert(snapshot.parameters_result&&parameter_calls==calls);
    c.parameters=(PalmParameters){80,-1,2};command(&c,3);assert(snapshot.parameters_result&&parameter_calls==calls);
    c.parameters=(PalmParameters){80,60,5};command(&c,4);assert(snapshot.parameters_result&&parameter_calls==calls);
    UserStore view;assert(!store_open(&view,path));
    assert(!sqlite3_exec(view.db,"CREATE TRIGGER fail_parameters BEFORE UPDATE ON device_meta "
        "BEGIN SELECT RAISE(ABORT,'test settings write failure'); END",NULL,NULL,NULL));
    c.parameters=(PalmParameters){81,61,1};command(&c,5);
    assert(snapshot.parameters_result&&snapshot.parameters.hand_threshold==80&&fake_parameters.hand_threshold==80);
    PalmParameters saved;assert(!store_parameters_load(&view,&saved)&&saved.auth_threshold==60);
    assert(!sqlite3_exec(view.db,"DROP TRIGGER fail_parameters",NULL,NULL,NULL));
    parameter_fail=1;command(&c,6);assert(snapshot.parameters_result&&fake_parameters.auth_threshold==60);
    assert(!store_parameters_load(&view,&saved)&&saved.auth_threshold==60);
    c.parameters=(PalmParameters){0,100,0};command(&c,7);assert(!snapshot.parameters_result);
    c.parameters=(PalmParameters){100,0,4};command(&c,8);assert(!snapshot.parameters_result);
    store_close(&view);access_close();unlink(path);snprintf(path,sizeof(path),"%s/app.conf",dir);unlink(path);snprintf(path,sizeof(path),"%s/avatars",dir);rmdir(path);rmdir(dir);
    fake_parameters=(PalmParameters){76,52,2};
    puts("PASS parameters: defaults, all three setters, persistent restart, boundaries, invalid input, storage/apply failure rollback");
}
int main(void)
{
    parameters_check();
    char dir[]="/tmp/ky50-access-test-XXXXXX",path[256];assert(mkdtemp(dir));snprintf(path,sizeof(path),"%s/users.sqlite3",dir);
    assert(store_employee_valid("00001234")&&!store_employee_valid("12A")&&!store_employee_valid(""));
    assert(store_name_valid("")&&store_name_valid("张三 Ab9")&&!store_name_valid("\xff")&&!store_name_valid("x\ny"));
    char shortname[5];app_utf8_copy(shortname,sizeof(shortname),"张三");assert(!strcmp(shortname,"张"));
    assert(!ime_load("assets/ime/pinyin.tsv"));char c[5];assert(!ime_candidate(ime_lookup("zhang"),0,c)&&!strcmp(c,"张"));
    assert(ime_count(ime_lookup("li"))>5);ime_close();
    assert(!access_open(dir,&backend));
    AccessCommand policy={.type=ACCESS_APP_PARAMETERS,.app_parameters={5,5,5,2,0,0}};command(&policy,900);
    AccessCommand request={.type=ACCESS_CREATE};strcpy(request.employee_id,"00001234");strcpy(request.name,"张三 Ab9");
    command(&request,1000);assert(snapshot.event==ACCESS_EVENT_CREATED&&snapshot.count==1);uint16_t id=snapshot.selected_id;
    assert(!strcmp(snapshot.users[0].employee_id,"00001234")&&!strcmp(snapshot.users[0].name,"张三 Ab9"));
    int64_t first_record=snapshot.users[0].record_id;
    command(&request,1100);assert(snapshot.event==ACCESS_EVENT_ERROR&&snapshot.count==1);
    request=(AccessCommand){.type=ACCESS_ENROLL,.local_id=id};command(&request,1200);assert(snapshot.registering&&snapshot.total==3);
    AccessCommand change={.type=ACCESS_PARAMETERS,.parameters={80,60,4}};int parameter_before=parameter_calls;
    command(&change,1210);assert(snapshot.parameters_result&&snapshot.registering&&parameter_calls==parameter_before);
    uint8_t image[]={37};HandPoseRes_t pose={0};pose.keypoints[0]=101;
    assert(!access_frame(image,540,960,&pose,1,1,1950)&&feature_calls==0);
    assert(!access_frame(image,540,960,&pose,0,1,1951));
    assert(!access_frame(image,540,960,&pose,1,1,1952)&&feature_calls==0);
    assert(!access_frame(image,540,960,&pose,-1,1,1953));
    assert(!access_frame(image,540,960,&pose,1,1,1954)&&feature_calls==0);
    access_detection_gap();
    assert(!access_frame(image,540,960,&pose,1,1,1960)&&feature_calls==0);
    quality_status=STATUS_TOO_DARK;access_frame(image,540,960,&pose,1,1,2000);access_snapshot(&snapshot,0);assert(snapshot.progress==0);
    assert(pose.status==STATUS_TOO_DARK&&enroll_calls==0&&recognize_calls==0);
    for(int status=1;status<=22;status++) {
        quality_status=status;access_frame(image,540,960,&pose,1,1,2010+status);
        assert(pose.status==status&&enroll_calls==0&&recognize_calls==0);
    }
    int before=feature_calls;access_frame(image,540,960,&pose,0,1,2050);assert(feature_calls==before);
    quality_status=STATUS_OK;feature_result=STATUS_FAILED;
    access_frame(image,540,960,&pose,1,1,2060);assert(pose.status==STATUS_FAILED&&enroll_calls==0);
    feature_result=STATUS_OK;
    quality_status=STATUS_OK;
    for(int i=0;i<3;i++) {
        access_frame(image,540,960,&pose,1,1,2200+i*200);
        if(i==0) {
            quality_status=STATUS_TOO_DARK;access_frame(image,540,960,&pose,1,1,2210);
            access_snapshot(&snapshot,0);assert(snapshot.progress==1&&enroll_calls==1&&pose.status==STATUS_TOO_DARK);
            quality_status=STATUS_OK;access_frame(image,540,960,&pose,1,1,2220);
            assert(enroll_calls==1&&pose.status==STATUS_OK); /* status refreshes inside business throttle */
        }
    }
    access_snapshot(&snapshot,0);assert(snapshot.event==ACCESS_EVENT_ENROLLED&&!snapshot.registering&&snapshot.progress==3);
    assert(snapshot.users[0].registered&CREDENTIAL_PALM);assert(templates[id]==2);
    access_set_exposure_backend(&ir_backend);assert(!ir_gets&&!ir_sets);
    access_enable_recognition(1);
    quality_status=STATUS_TOO_BLUR;access_frame(image,540,960,&pose,1,0,5000);assert(recognize_calls==0);
    quality_status=STATUS_OK;
    for(int i=0;i<15;i++)access_frame(image,540,960,&pose,1,0,6000+i*200);
    before=feature_calls;quality_status=STATUS_TOO_CLOSE;
    assert(access_frame(image,540,960,&pose,1,0,8901)==1);
    assert(feature_calls==before+1&&pose.status==STATUS_TOO_CLOSE&&recognize_calls==2);
    access_enable_recognition(0);before=feature_calls;
    access_poll(8902); /* Leaving home must not release a successful hand. */
    quality_status=STATUS_TOO_RIGHT;access_frame(image,540,960,&pose,1,0,8902);
    assert(feature_calls==before+1&&pose.status==STATUS_TOO_RIGHT&&recognize_calls==2);
    change.parameters=(PalmParameters){76,52,2};command(&change,8903);
    assert(!snapshot.parameters_result);
    access_enable_recognition(1);quality_status=STATUS_OK;
    before=feature_calls;
    UserStore view;assert(!store_open(&view,path));assert(scalar(view.db,"SELECT count(*) FROM attendance_events WHERE result='matched'")==1);
    int saved_target=-1;assert(!store_ir_target_load(&view,&saved_target)&&saved_target==56);
    assert(ir_gets==2&&!ir_sets); /* Initial target read + one real match saved. */
    assert(!access_frame(image,540,960,&pose,0,0,10000));
    assert(access_frame(image,540,960,&pose,1,0,10001)==1&&feature_calls==++before&&recognize_calls==2); /* time alone never unlocks auth */
    for(int i=0;i<APP_DEFAULT_LEAVE_FRAMES-1;i++)assert(!access_frame(image,540,960,&pose,0,0,10100+i*5));
    assert(access_frame(image,540,960,&pose,1,0,10250)==1&&feature_calls==++before&&recognize_calls==2); /* no entry filter within session */
    for(int i=0;i<APP_DEFAULT_LEAVE_FRAMES-1;i++)assert(!access_frame(image,540,960,&pose,0,0,10400+i*5));
    assert(!access_frame(image,540,960,&pose,-1,0,10550)); /* error is not the final false */
    for(int i=0;i<APP_DEFAULT_LEAVE_FRAMES-1;i++)assert(!access_frame(image,540,960,&pose,0,0,10600+i*5));
    assert(access_frame(image,540,960,&pose,1,0,10730)==1&&feature_calls==++before&&recognize_calls==2);
    for(int i=0;i<APP_DEFAULT_LEAVE_FRAMES;i++)assert(!access_frame(image,540,960,&pose,0,0,11000+i*10));
    assert(ir_gets==2&&!ir_sets);
    assert(!access_frame(image,540,960,&pose,1,0,11500)&&feature_calls==before);
    assert(access_frame(image,540,960,&pose,1,0,11700)==1&&feature_calls==before+1&&recognize_calls==3);
    access_frame(image,540,960,&pose,1,0,11900);assert(recognize_calls==4);
    assert(scalar(view.db,"SELECT count(*) FROM attendance_events WHERE result='matched'")==2);store_close(&view);
    remove_fails=1;request=(AccessCommand){.type=ACCESS_DELETE,.count=1};request.ids[0]=id;command(&request,12000);
    assert(snapshot.count==1&&snapshot.users[0].deleting&&templates[id]);access_close();
    remove_fails=0;assert(!access_open(dir,&backend));access_snapshot(&snapshot,0);assert(!snapshot.count&&!templates[id]);access_close();
    assert(!store_open(&view,path));assert(scalar(view.db,"SELECT count(*) FROM attendance_events WHERE user_id IS NULL AND employee_id='00001234'")==2);
    AppUser user;
    assert(!store_create(&view,"222","",&user));assert(user.local_id==id&&user.record_id!=first_record);
    assert(!store_prepare_enroll(&view,id,id));templates[id]=1;store_close(&view);
    assert(!access_open(dir,&backend));access_snapshot(&snapshot,0);assert(!templates[id]&&snapshot.users[0].palm_state==CRED_NONE);access_close();
    assert(!store_open(&view,path));assert(!store_prepare_enroll(&view,id,id));templates[id]=2;store_close(&view);
    assert(!access_open(dir,&backend));access_snapshot(&snapshot,0);assert(snapshot.users[0].palm_state==CRED_REGISTERED);access_close();
    assert(!store_open(&view,path));assert(!scalar(view.db,"SELECT count(*) FROM operations"));
    assert(scalar(view.db,"SELECT count(*) FROM credentials")==5);
    for(int i=1;i<APP_MAX_USERS;i++){char employee[32];snprintf(employee,sizeof(employee),"%d",1000+i);assert(!store_create(&view,employee,"",&user));}
    assert(store_create(&view,"333","",&user)==-3);
    assert(!sqlite3_exec(view.db,"DELETE FROM users WHERE local_id!=0",NULL,NULL,NULL));
    assert(sqlite3_exec(view.db,"PRAGMA query_only=ON",NULL,NULL,NULL)==SQLITE_OK);
    assert(store_create(&view,"333","",&user)==-1);assert(scalar(view.db,"SELECT count(*) FROM users")==1);
    assert(sqlite3_exec(view.db,"PRAGMA query_only=OFF",NULL,NULL,NULL)==SQLITE_OK);
    assert(sqlite3_exec(view.db,"WITH RECURSIVE n(i) AS(VALUES(1) UNION ALL SELECT i+1 FROM n WHERE i<9998) "
        "INSERT INTO attendance_events(result,created_utc,time_valid,monotonic_ms) SELECT 'matched',0,0,i FROM n",NULL,NULL,NULL)==SQLITE_OK);
    assert(scalar(view.db,"SELECT count(*) FROM attendance_events")==10000);
    assert(store_record_unknown(&view,999999)==-1&&scalar(view.db,"SELECT count(*) FROM attendance_events")==10000);
    assert(sqlite3_exec(view.db,"UPDATE attendance_events SET exported=1 WHERE id=(SELECT min(id) FROM attendance_events)",NULL,NULL,NULL)==SQLITE_OK);
    assert(!store_record_unknown(&view,1000000)&&scalar(view.db,"SELECT count(*) FROM attendance_events")==10000);
    assert(sqlite3_exec(view.db,"PRAGMA integrity_check",NULL,NULL,NULL)==SQLITE_OK);store_close(&view);
    templates[9]=2;templates[id]=0;
    assert(!access_open(dir,&backend));access_snapshot(&snapshot,0);
    assert(snapshot.orphan_count==1&&snapshot.users[0].palm_state==CRED_MISSING&&templates[9]);access_close();
    unlink(path);snprintf(path,sizeof(path),"%s/app.conf",dir);unlink(path);snprintf(path,sizeof(path),"%s/avatars",dir);rmdir(path);rmdir(dir);
    printf("PASS %d-true entry, %d false interruptions keep success lock, %d-false exit and next-round filter; errors break streaks, success stops auth across time/navigation/settings while live features/status continue; quality gates, enrollment, parameters, persistence, recovery, attendance and pinyin\n",PALM_ENTER_FRAMES,APP_DEFAULT_LEAVE_FRAMES-1,APP_DEFAULT_LEAVE_FRAMES);
    return 0;
}
