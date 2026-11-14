/* Host-only backend. All UI interactions below use the LVGL pointer driver. */
static unsigned char ui_templates[APP_MAX_USERS];
static int test_ids(uint16_t *ids,int n){int k=0;for(int i=0;i<APP_MAX_USERS;i++)if(ui_templates[i]){assert(k<n);ids[k++]=i;}return k;}
static int test_exists(uint16_t id){return ui_templates[id]!=0;}
static int test_remove(uint16_t id){ui_templates[id]=0;return 0;}
static int test_clear(void){return 0;}
static int test_capacity(void){return 3;}
static int test_features(uint8_t *y,uint16_t w,uint16_t h,HandPoseRes_t *p,uint8_t *f)
{(void)y;(void)w;(void)h;p->status=STATUS_OK;memset(f,1,APP_FEATURE_BYTES);return STATUS_OK;}
static int test_enroll(User_For_C_t *u,uint8_t *f,ResgisRes_t *p)
{(void)f;p->total=3;p->status=++p->progress==3?STATUS_REGIS_SUCCESS:STATUS_IN_REGISING;if(p->progress==3)ui_templates[u->uuid]=1;return 0;}
static int test_recognize(uint8_t *f,User_For_C_t *u){(void)f;(void)u;return 0;}
static PalmParameters ui_parameters={76,52,2};
static int test_parameters_get(PalmParameters *p){*p=ui_parameters;return 0;}
static int test_parameters_set(const PalmParameters *p){ui_parameters=*p;return 0;}
static int test_pick_free(void){for(int i=0;i<APP_TEMPLATE_SLOTS;i++)if(!ui_templates[i])return i;return -1;}
static const PalmBackend test_backend={test_ids,test_exists,test_exists,test_remove,test_clear,test_capacity,test_features,test_enroll,test_recognize,test_parameters_get,test_parameters_set,test_pick_free};
static unsigned char ui_faces[APP_MAX_USERS];
static int ui_face_ids(uint16_t *ids,int cap){int n=0;for(int i=0;i<APP_MAX_USERS;i++)if(ui_faces[i]){assert(n<cap);ids[n++]=i;}return n;}
static int ui_face_exists(uint16_t id){return ui_faces[id]!=0;}
static int ui_face_remove(uint16_t id){ui_faces[id]=0;return 0;}
static int ui_face_capacity(void){return 1;}
static int ui_face_features(uint8_t *y,uint16_t w,uint16_t h,FacePoseRes_t *p,uint8_t *f)
{(void)y;(void)w;(void)h;p->status=STATUS_OK;p->sharpness=.8f;memset(f,1,APP_FEATURE_BYTES);return STATUS_OK;}
static int ui_face_enroll(User_For_C_t *u,uint8_t *f,float quality,ResgisRes_t *p)
{(void)f;(void)quality;ui_faces[u->uuid]=1;*p=(ResgisRes_t){.status=STATUS_REGIS_SUCCESS,.progress=1,.total=1};return 0;}
static int ui_face_pick_free(void){for(int i=0;i<APP_TEMPLATE_SLOTS;i++)if(!ui_faces[i])return i;return -1;}
static FaceParameters ui_face_parameters={50,30,50,1,1,24,24};
static int ui_face_parameters_get(FaceParameters *p){*p=ui_face_parameters;return 0;}
static int ui_face_parameters_set(const FaceParameters *p){ui_face_parameters=*p;return 0;}
static int ui_live_ir(uint8_t *p,uint16_t w,uint16_t h){(void)p;(void)w;(void)h;return 1;}
static const FaceBackend ui_face_backend={ui_face_ids,ui_face_exists,ui_face_exists,ui_face_remove,test_clear,
    ui_face_capacity,ui_face_features,ui_face_enroll,test_recognize,ui_face_pick_free,ui_face_parameters_get,ui_face_parameters_set,NULL,NULL,NULL,ui_live_ir,NULL};
static lv_obj_t *find_matrix(lv_obj_t *o)
{
    if(lv_obj_check_type(o,&lv_buttonmatrix_class))return o;
    for(unsigned i=0;i<lv_obj_get_child_count(o);i++){lv_obj_t *r=find_matrix(lv_obj_get_child(o,i));if(r)return r;}
    return NULL;
}
static void tap_name_key(const char *label)
{
    step();lv_obj_t *o=find_matrix(lv_screen_active());assert(o);
    lv_buttonmatrix_t *m=(lv_buttonmatrix_t*)o;lv_area_t origin;lv_obj_get_coords(o,&origin);
    for(unsigned i=0;i<m->btn_cnt;i++)if(!strcmp(lv_buttonmatrix_get_button_text(o,i),label)) {
        lv_area_t a=m->button_areas[i];tap(origin.x1+(a.x1+a.x2)/2,origin.y1+(a.y1+a.y2)/2);return;
    }
    fprintf(stderr,"Missing name key %s\n",label);abort();
}
static void business_check(const char *images)
{
    char dir[]="/tmp/ky50-user-ui-XXXXXX",path[256];assert(mkdtemp(dir));assert(!access_open_biometrics(dir,&test_backend,&ui_face_backend));
    AccessCommand policy={.type=ACCESS_APP_PARAMETERS,.app_parameters={5,5,5,1,0,0}};assert(!access_submit(&policy));access_poll(1);
    business_active=1;step();ui_show(UI_ADVANCED,0);step();tap(240,368);
    assert(ui_current_page()==UI_PARAMETERS);save(images,"parameters-default");
    tap(390,250);tap(90,416);tap(390,582);save(images,"parameters-edited");
    assert(ui_parameters.hand_threshold==76); /* Editing does not apply until Save. */
    tap(120,806);assert(ui_parameters.hand_threshold==77&&ui_parameters.auth_threshold==51&&ui_parameters.dynamic_level==3);
    save(images,"parameters-saved");tap(350,806);assert(ui_current_page()==UI_ADVANCED);
    tap(240,368);tap(390,250);tap(50,100);tap(240,368);save(images,"parameters-reentered");
    assert(ui_parameters.hand_threshold==77);tap(120,806);assert(ui_parameters.hand_threshold==77); /* Back discards draft. */
    tap(390,582);tap(390,582);tap(120,806);assert(ui_parameters.dynamic_level==4);
    for(int i=0;i<6;i++)tap(90,582);
    tap(120,806);assert(ui_parameters.dynamic_level==0);
    access_close();ui_parameters=(PalmParameters){76,52,2};assert(!access_open_biometrics(dir,&test_backend,&ui_face_backend));step();
    ui_show(UI_PARAMETERS,0);step();assert(ui_parameters.hand_threshold==77&&ui_parameters.dynamic_level==0);save(images,"parameters-restarted");
    puts("PASS parameter touch: advanced route, +/- controls, save/apply, discard, limits, restart restore");
    tap(392,739);save(images,"face-parameters-detection-quality");
    tap(390,250);tap(90,416);tap(390,582);
    tap(392,739);save(images,"face-parameters-liveness-auth");
    tap(90,250);tap(390,416);tap(90,582);
    tap(392,739);tap(90,250);save(images,"face-parameters-filter");
    assert(ui_face_parameters.detection_threshold==50&&ui_face_parameters.quality_enable==1);
    tap(120,806);
    assert(ui_face_parameters.detection_threshold==51&&ui_face_parameters.quality_threshold==31&&ui_face_parameters.quality_enable==0);
    assert(ui_face_parameters.liveness_enable==0&&ui_face_parameters.liveness_threshold==51&&ui_face_parameters.auth_threshold==23&&ui_face_parameters.filter_threshold==23);
    assert(ui_parameters.hand_threshold==77); /* SDK modalities have independent saves. */
    tap(392,739);save(images,"application-departure-default");
    tap(90,250);tap(390,416);tap(390,582);tap(120,806);
    AppParameters app;assert(!app_config_load(dir,&app)&&app.palm_leave_frames==4&&app.face_leave_frames==6&&app.relay_seconds==6);
    save(images,"application-departure-saved");
    for(int i=0;i<7;i++)tap(90,582);
    tap(120,806);assert(!app_config_load(dir,&app)&&app.relay_seconds==1);
    for(int i=0;i<5;i++)tap(390,582);
    tap(120,806);assert(!app_config_load(dir,&app)&&app.relay_seconds==6);
    for(int i=0;i<6;i++)tap(90,250);
    tap(120,806);assert(!app_config_load(dir,&app)&&app.palm_leave_frames==1&&app.face_leave_frames==6&&app.relay_seconds==6);
    tap(392,739);save(images,"application-match-default");
    assert(!app_config_load(dir,&app)&&app.match_confirmations==1);
    tap(390,250);assert(!app_config_load(dir,&app)&&app.match_confirmations==1);
    tap(120,806);assert(!app_config_load(dir,&app)&&app.match_confirmations==2);
    save(images,"application-match-saved");
    for(int i=0;i<12;i++)tap(390,250);
    tap(120,806);assert(!app_config_load(dir,&app)&&app.match_confirmations==10);
    for(int i=0;i<12;i++)tap(90,250);
    tap(120,806);assert(!app_config_load(dir,&app)&&app.match_confirmations==1);
    assert(!app_config_load(dir,&app)&&app.success_cooldown_seconds==0);
    for(int i=0;i<3;i++)tap(390,416);
    tap(120,806);assert(!app_config_load(dir,&app)&&app.success_cooldown_seconds==3);
    save(images,"application-shared-policy");
    for(int i=0;i<5;i++)tap(90,416);
    tap(120,806);assert(!app_config_load(dir,&app)&&app.success_cooldown_seconds==0);
    for(int i=0;i<2;i++)tap(390,416);
    tap(120,806);
    /* IR can be enabled with RGB off; the two switches stay independent. */
    tap(390,582);tap(120,806);assert(!app_config_load(dir,&app)&&app.strong_liveness&&!ui_face_parameters.liveness_enable);
    for(int i=0;i<3;i++)tap(88,739);
    tap(390,250);tap(120,806);assert(ui_face_parameters.liveness_enable==1);
    for(int i=0;i<3;i++)tap(392,739);
    tap(390,582);tap(120,806);assert(!app_config_load(dir,&app)&&app.strong_liveness==1);
    save(images,"application-strong-live");
    tap(90,582);tap(120,806);assert(!app_config_load(dir,&app)&&app.strong_liveness==0);
    tap(390,582);tap(120,806);assert(!app_config_load(dir,&app)&&app.strong_liveness==1);
    access_close();ui_face_parameters=(FaceParameters){50,30,50,1,1,24,24};
    assert(!access_open_biometrics(dir,&test_backend,&ui_face_backend));step();
    assert(ui_face_parameters.detection_threshold==51&&ui_face_parameters.liveness_enable==1);
    assert(!app_config_load(dir,&app)&&app.palm_leave_frames==1&&app.face_leave_frames==6&&app.relay_seconds==6);
    assert(!app_config_load(dir,&app)&&app.match_confirmations==1&&app.success_cooldown_seconds==2&&app.strong_liveness==1);
    puts("PASS face/departure/relay touch: six pages, shared match count, seven SDK fields, immediate save, independent app.conf and restart restore");
    ui_show(UI_USERS,0);step();save(images,"users-empty");
    tap(110,800);assert(ui_current_page()==UI_USER_ID);tap(350,795);assert(ui_current_page()==UI_USER_ID);
    tap(240,690);tap(240,690);tap(90,395);tap(240,395);tap(390,395);
    assert(!strcmp(user_ui_employee_text(),"00123"));save(images,"user-id");tap(350,795);assert(ui_current_page()==UI_USER_NAME);
    const char *keys[]={"z","h","a","n","g"};for(unsigned i=0;i<5;i++)tap_name_key(keys[i]);
    assert(!strcmp(user_ui_composition(),"zhang"));save(images,"name-pinyin");tap(98,405);
    assert(!strcmp(user_ui_name_text(),"张"));tap_name_key("中/英");tap_name_key("大写");tap_name_key("A");tap_name_key("小写");tap_name_key("b");
    tap_name_key("数字");tap_name_key("9");tap_name_key("0");tap_name_key("删除");
    assert(!strcmp(user_ui_name_text(),"张Ab9"));save(images,"name-mixed");tap_name_key("确认");assert(ui_current_page()==UI_USER_TYPES);
    save(images,"user-types");tap(240,355);assert(ui_current_page()==UI_PALM_ENROLL);step();save(images,"face-start");
    uint8_t face_image[1]={0};HandPoseRes_t absent={0};FacePoseRes_t face_pose={0};
    access_biometrics_frame(face_image,face_image,540,960,&absent,0,&face_pose,1,1,business_time+=1000);step();
    assert(ui_current_page()==UI_USER_TYPES&&ui_faces[0]);save(images,"face-complete");
    tap(240,455);assert(ui_current_page()==UI_PALM_ENROLL);step();save(images,"palm-start");
    uint8_t image[1]={0};HandPoseRes_t pose={0};
    for(int i=0;i<4;i++){access_frame(image,540,960,&pose,1,1,business_time+=1000);step();if(i==1)save(images,"palm-progress");}
    assert(ui_current_page()==UI_USER_TYPES);save(images,"palm-complete");tap(50,100);assert(ui_current_page()==UI_USERS);save(images,"users-registered");
    /* Optional name and cancel registration. */
    tap(110,800);tap(90,395);tap(350,795);assert(ui_current_page()==UI_USER_NAME);tap_name_key("确认");assert(ui_current_page()==UI_USER_TYPES);
    tap(240,455);assert(ui_current_page()==UI_PALM_ENROLL);tap(50,100);step();assert(ui_current_page()==UI_USER_TYPES);
    tap(50,100);tap(350,800);assert(ui_current_page()==UI_USERS_MANAGE);save(images,"users-manage");
    tap(420,225);tap(420,333);save(images,"users-selected");tap(110,800);step();
    AccessSnapshot s;access_snapshot(&s,0);assert(s.count==0&&!ui_templates[0]&&!ui_templates[1]&&!ui_faces[0]);save(images,"users-deleted");
    tap(350,800);assert(ui_current_page()==UI_USERS);
    for(int i=0;i<12;i++) {
        AccessCommand c={.type=ACCESS_CREATE};snprintf(c.employee_id,sizeof(c.employee_id),"%d",10000+i);
        assert(!access_submit(&c));access_poll(business_time+=100);
    }
    step();ui_pointer(220,650,1);step();
    for(int y=620;y>=230;y-=30){ui_pointer(220,y,1);step();}
    ui_pointer(220,230,0);step();save(images,"users-scrolled");
    tap(110,800);assert(ui_current_page()==UI_USER_ID);tap(50,100);assert(ui_current_page()==UI_USERS);
    business_active=0;access_close();snprintf(path,sizeof(path),"%s/users.sqlite3",dir);unlink(path);
    snprintf(path,sizeof(path),"%s/app.conf",dir);unlink(path);
    snprintf(path,sizeof(path),"%s/avatars",dir);rmdir(path);rmdir(dir);
    puts("PASS real touch flow: numeric ID, pinyin candidate, upper/lower/digit/delete, optional name, registration progress/return/cancel, two checkboxes and delete");
}
