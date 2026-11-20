#include "ky50_features.h"
#include "user_ui.h"
#include "ime.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include <sys/stat.h>

extern const lv_font_t ky50_font_20,ky50_font_24;
#define BG 0x0b1726
#define CARD 0x182a3e
#define ACCENT 0x51d5ee
#define TEXT 0xf3f7fc
#define MUTED 0x9eafc1
static AccessSnapshot data;
static lv_obj_t *parent,*message_label,*employee_label,*name_area,*keyboard,*composition_label,*candidate_labels[5];
static lv_obj_t *progress_bar,*progress_label,*selected_label,*finger_round_label;
static enum ui_page current=UI_HOME;
static char employee[APP_EMPLOYEE_BYTES],name[APP_NAME_BYTES],composition[8];
static uint16_t selected_id=UINT16_MAX;
static uint8_t selected[APP_MAX_USERS];
static int chinese=1,upper,numeric,creating,ime_available;
static unsigned candidate_page,seen_event;
static lv_obj_t *box(lv_obj_t *p,int x,int y,int w,int h,uint32_t color,int opacity,int radius)
{
    lv_obj_t *o=lv_obj_create(p);lv_obj_remove_style_all(o);lv_obj_set_pos(o,x,y);lv_obj_set_size(o,w,h);
    lv_obj_set_style_bg_color(o,lv_color_hex(color),0);lv_obj_set_style_bg_opa(o,opacity,0);
    lv_obj_set_style_radius(o,radius,0);lv_obj_set_scrollable(o,false);return o;
}
static lv_obj_t *text(lv_obj_t *p,const char *s,int x,int y,int w,const lv_font_t *font,uint32_t color)
{
    lv_obj_t *o=lv_label_create(p);lv_obj_remove_style_all(o);lv_obj_set_pos(o,x,y);lv_obj_set_width(o,w);
    lv_obj_set_style_text_font(o,font,0);lv_obj_set_style_text_color(o,lv_color_hex(color),0);
    lv_obj_set_style_text_align(o,LV_TEXT_ALIGN_CENTER,0);lv_label_set_text(o,s);lv_obj_set_clickable(o,false);return o;
}
static lv_obj_t *button(lv_obj_t *p,int x,int y,int w,int h,const char *title,lv_event_cb_t cb,intptr_t id)
{
    lv_obj_t *o=box(p,x,y,w,h,CARD,245,12);lv_obj_set_clickable(o,true);
    lv_obj_set_style_bg_color(o,lv_color_hex(0x286078),LV_STATE_PRESSED);
    lv_obj_set_style_border_color(o,lv_color_hex(0x36506a),0);lv_obj_set_style_border_width(o,1,0);
    lv_obj_add_event_cb(o,cb,LV_EVENT_CLICKED,(void*)id);
    if(title)text(o,title,2,(h-32)/2,w-4,&ky50_font_24,TEXT);
    return o;
}
static void message(const char *s){if(message_label)lv_label_set_text(message_label,s);}
static int submit(AccessCommand *c)
{
    if(access_submit(c)){message("正在处理，请稍候");return -1;}return 0;
}
static void navigate(lv_event_t *e){ui_show((enum ui_page)(intptr_t)lv_event_get_user_data(e),0);}
static void heading(const char *title,enum ui_page back)
{
    button(parent,20,72,72,54,"返回",navigate,back);
    text(parent,title,102,82,344,&ky50_font_24,TEXT);
    message_label=text(parent,"",20,136,440,&ky50_font_20,ACCENT);
}
static AppUser *selected_user(void)
{
    for(int i=0;i<data.count;i++)if(data.users[i].local_id==selected_id)return &data.users[i];
    return NULL;
}
static void show_avatar(lv_obj_t *row,const AppUser *u)
{
    char base[PATH_MAX],path[PATH_MAX],source[PATH_MAX+2];struct stat st;
    const char *directory=access_data_directory();
    if((u->registered&CREDENTIAL_FACE)&&*u->avatar&&*directory) {
        char dir[448];snprintf(dir,sizeof(dir),"%s/avatars",directory);
        if(realpath(dir,base)&&realpath(u->avatar,path)&&!strncmp(path,base,strlen(base))&&path[strlen(base)]=='/'&&
           !stat(path,&st)&&S_ISREG(st.st_mode)&&st.st_size>0&&st.st_size<=131072) {
            snprintf(source,sizeof(source),"S:%s",path);lv_image_header_t info;
            if(lv_image_decoder_get_info(source,&info)==LV_RESULT_OK&&info.w&&info.h&&info.w<=512&&info.h<=512) {
                lv_obj_t *image=lv_image_create(row);lv_image_set_src(image,source);lv_obj_set_pos(image,12,16);
                lv_image_set_pivot(image,0,0);
                lv_image_set_scale(image,256*56/(info.w>info.h?info.w:info.h));return;
            }
        }
    }
    /* Standard neutral head-and-shoulders icon; no synthetic biometric image. */
    lv_obj_t *tile=box(row,12,16,58,64,0x284159,255,10);
    box(tile,21,10,16,16,MUTED,255,8);box(tile,13,30,32,23,MUTED,255,12);
}
static const int registration_enabled[]={KY50_ENABLE_FACE,KY50_ENABLE_PALM,KY50_ENABLE_FINGERPRINT,1,1};
static void types_text(unsigned mask,char *out,size_t n)
{
    static const char *names[]={"人脸","掌静脉","指纹","卡","密码"};out[0]=0;
    for(int i=0;i<5;i++)if(registration_enabled[i]&&(mask&(1u<<i))){size_t len=strlen(out);snprintf(out+len,n-len,"%s%s",len?" / ":"",names[i]);}
    if(!*out)snprintf(out,n,"未注册");
}
static unsigned selection_count(void){unsigned n=0;for(int i=0;i<APP_MAX_USERS;i++)n+=!!selected[i];return n;}
static void selection_changed(lv_event_t *e)
{
    unsigned id=(unsigned)(uintptr_t)lv_event_get_user_data(e);
    if(id>=APP_MAX_USERS)return;
    selected[id]=lv_obj_has_state(lv_event_get_target(e),LV_STATE_CHECKED);
    if(selected_label)lv_label_set_text_fmt(selected_label,"已选择 %u 人",selection_count());
}
static void open_user(lv_event_t *e)
{
    selected_id=(uint16_t)(uintptr_t)lv_event_get_user_data(e);ui_show(UI_USER_TYPES,0);
}
static void add_user(lv_event_t *e)
{
    (void)e;if(!data.ready){message("用户数据库尚未就绪");return;}
    employee[0]=name[0]=composition[0]=0;creating=0;ui_show(UI_USER_ID,0);
}
static void manage(lv_event_t *e){(void)e;memset(selected,0,sizeof(selected));ui_show(UI_USERS_MANAGE,0);}
static void delete_selected(lv_event_t *e)
{
    (void)e;AccessCommand c={.type=ACCESS_DELETE};
    if(data.busy){message("正在删除，请稍候");return;}
    for(int i=0;i<APP_MAX_USERS;i++)if(selected[i])c.ids[c.count++]=i;
    if(!c.count){message("请先勾选需要删除的用户");return;}
    if(!submit(&c))message("正在删除用户数据");
}
static void users_page(int managing)
{
    heading(managing?"管理用户":"用户管理",managing?UI_USERS:UI_MENU);
    if(managing)selected_label=message_label;
    if(managing)lv_label_set_text_fmt(message_label,"已选择 %u 人",selection_count());
    else lv_label_set_text_fmt(message_label,"本机用户 %d / %d",data.count,APP_MAX_USERS);
    lv_obj_t *list=box(parent,16,174,448,566,BG,255,0);
    lv_obj_set_scrollable(list,true);lv_obj_set_scroll_dir(list,LV_DIR_VER);lv_obj_set_scrollbar_mode(list,LV_SCROLLBAR_MODE_AUTO);
    if(!data.count)text(list,data.ready?"暂无用户，请点击添加用户":"正在读取用户数据",20,170,408,&ky50_font_24,MUTED);
    for(int i=0;i<data.count;i++) {
        AppUser *u=&data.users[i];
        lv_obj_t *row=button(list,0,i*108,442,100,NULL,open_user,u->local_id);
        if(managing)lv_obj_set_clickable(row,false);
        show_avatar(row,u);
        lv_obj_t *n=text(row,*u->name?u->name:"未设置姓名",82,8,managing?292:348,&ky50_font_24,TEXT);
        lv_obj_set_style_text_align(n,LV_TEXT_ALIGN_LEFT,0);lv_label_set_long_mode(n,LV_LABEL_LONG_DOT);
        char line[192];snprintf(line,sizeof(line),"ID  %s",u->employee_id);
        n=text(row,line,82,39,managing?292:348,&ky50_font_20,MUTED);lv_obj_set_style_text_align(n,LV_TEXT_ALIGN_LEFT,0);lv_label_set_long_mode(n,LV_LABEL_LONG_DOT);
        types_text(u->registered,line,sizeof(line));
        if(u->deleting)snprintf(line,sizeof(line),"删除待重试");
        else if(KY50_ENABLE_FACE&&u->face_state==CRED_MISSING)snprintf(line,sizeof(line),"人脸需重新注册");
        else if(KY50_ENABLE_PALM&&u->palm_state==CRED_MISSING)snprintf(line,sizeof(line),"掌静脉需重新注册");
        n=text(row,line,82,69,managing?292:348,&ky50_font_20,ACCENT);lv_obj_set_style_text_align(n,LV_TEXT_ALIGN_LEFT,0);
        if(managing) {
            lv_obj_t *check=lv_checkbox_create(row);lv_checkbox_set_text(check,"");lv_obj_set_pos(check,382,29);
            lv_obj_set_style_pad_all(check,8,0);lv_obj_set_style_pad_all(check,6,LV_PART_INDICATOR);
            lv_obj_set_style_bg_opa(check,LV_OPA_COVER,LV_PART_INDICATOR);
            lv_obj_set_style_bg_color(check,lv_color_hex(BG),LV_PART_INDICATOR);
            lv_obj_set_style_border_width(check,2,LV_PART_INDICATOR);
            lv_obj_set_style_border_color(check,lv_color_hex(MUTED),LV_PART_INDICATOR);
            lv_obj_set_style_radius(check,5,LV_PART_INDICATOR);
            lv_obj_set_style_bg_image_src(check,LV_SYMBOL_OK,LV_PART_INDICATOR|LV_STATE_CHECKED);
            lv_obj_set_style_bg_image_recolor(check,lv_color_hex(BG),LV_PART_INDICATOR|LV_STATE_CHECKED);
            lv_obj_set_style_bg_color(check,lv_color_hex(ACCENT),LV_PART_INDICATOR|LV_STATE_CHECKED);
            if(u->local_id<APP_MAX_USERS&&selected[u->local_id])lv_obj_add_state(check,LV_STATE_CHECKED);
            lv_obj_add_event_cb(check,selection_changed,LV_EVENT_VALUE_CHANGED,(void*)(uintptr_t)u->local_id);
        }
    }
    /* Footer is outside the scroll container and remains visible. */
    box(parent,0,748,480,106,BG,255,0);
    if(managing){button(parent,20,768,210,66,"删除",delete_selected,0);button(parent,250,768,210,66,"返回",navigate,UI_USERS);}
    else {button(parent,20,768,210,66,"添加用户",add_user,0);button(parent,250,768,210,66,"管理用户",manage,0);}
}
static void id_key(lv_event_t *e)
{
    int k=(int)(intptr_t)lv_event_get_user_data(e);size_t n=strlen(employee);
    if(k==10){if(n)employee[n-1]=0;}else if(k==11)employee[0]=0;
    else if(n<APP_EMPLOYEE_BYTES-1){employee[n]='0'+k;employee[n+1]=0;}
    lv_label_set_text(employee_label,*employee?employee:"请输入数字工号");message("");
}
static void confirm_id(lv_event_t *e)
{
    (void)e;if(!store_employee_valid(employee)){message("工号不能为空，只能输入数字");return;}
    for(int i=0;i<data.count;i++)if(!strcmp(data.users[i].employee_id,employee)){message("工号已存在，请重新输入");return;}
    composition[0]=0;candidate_page=0;ui_show(UI_USER_NAME,0);
}
static void id_page(void)
{
    heading("输入工号",UI_USERS);
    lv_obj_t *field=box(parent,24,186,432,112,CARD,255,14);
    employee_label=text(field,*employee?employee:"请输入数字工号",16,22,400,&ky50_font_24,TEXT);
    text(parent,"最多 32 位数字，保留前导零",20,306,440,&ky50_font_20,MUTED);
    static const char *keys[]={"1","2","3","4","5","6","7","8","9","删除","0","清空"};
    for(int i=0;i<12;i++)button(parent,24+(i%3)*148,352+(i/3)*98,136,86,keys[i],id_key,i<9?i+1:i==9?10:i==10?0:11);
    button(parent,24,760,208,70,"返回",navigate,UI_USERS);button(parent,248,760,208,70,"确认",confirm_id,0);
}
static const char *const lower_map[]={"q","w","e","r","t","y","u","i","o","p","\n","a","s","d","f","g","h","j","k","l","\n","大写","z","x","c","v","b","n","m","删除","\n","中/英","数字","空格","返回","确认",""};
static const char *const upper_map[]={"Q","W","E","R","T","Y","U","I","O","P","\n","A","S","D","F","G","H","J","K","L","\n","小写","Z","X","C","V","B","N","M","删除","\n","中/英","数字","空格","返回","确认",""};
static const char *const number_map[]={"1","2","3","4","5","6","7","8","9","0","\n","-","_",".","@","#","/","(",")","删除","\n","字母","空格","返回","确认",""};
static void update_candidates(void)
{
    if(!composition_label)return;
    lv_label_set_text_fmt(composition_label,chinese?"拼音：%s":"英文输入%s",chinese?composition:"");
    const char *candidates=ime_lookup(composition);
    for(unsigned i=0;i<5;i++){char c[5];ime_candidate(candidates,candidate_page*5+i,c);lv_label_set_text(candidate_labels[i],c);}
}
static void commit_text(const char *s)
{
    if(!name_area)return;
    lv_textarea_add_text(name_area,s);app_utf8_copy(name,sizeof(name),lv_textarea_get_text(name_area));
}
static void choose_candidate(lv_event_t *e)
{
    char c[5];unsigned i=(unsigned)(uintptr_t)lv_event_get_user_data(e);
    if(!ime_candidate(ime_lookup(composition),candidate_page*5+i,c)){commit_text(c);composition[0]=0;candidate_page=0;update_candidates();}
}
static void candidate_move(lv_event_t *e)
{
    int delta=(int)(intptr_t)lv_event_get_user_data(e);
    if(delta<0){if(candidate_page)--candidate_page;}else if((candidate_page+1)*5<ime_count(ime_lookup(composition)))++candidate_page;
    update_candidates();
}
static void confirm_name(void)
{
    if(creating||data.busy){message("正在保存，请稍候");return;}
    if(*composition){message("请选择汉字，或切换英文输入");return;}
    app_utf8_copy(name,sizeof(name),lv_textarea_get_text(name_area));
    if(!store_name_valid(name)){message("姓名最多 32 个字符");return;}
    AccessCommand c={.type=ACCESS_CREATE};strcpy(c.employee_id,employee);strcpy(c.name,name);
    if(!submit(&c)){creating=1;message("正在保存用户");}
}
static void name_key(lv_event_t *e)
{
    (void)e;
    if(creating)return;
    unsigned key=lv_buttonmatrix_get_selected_button(keyboard);if(key==LV_BUTTONMATRIX_BUTTON_NONE)return;
    const char *s=lv_buttonmatrix_get_button_text(keyboard,key);if(!s)return;
    if(!strcmp(s,"确认")){confirm_name();return;}
    if(!strcmp(s,"返回")){app_utf8_copy(name,sizeof(name),lv_textarea_get_text(name_area));composition[0]=0;ui_show(UI_USER_ID,0);return;}
    if(!strcmp(s,"删除")) {
        size_t n=strlen(composition);if(n)composition[n-1]=0;else lv_textarea_delete_char(name_area);
        app_utf8_copy(name,sizeof(name),lv_textarea_get_text(name_area));candidate_page=0;update_candidates();return;
    }
    if(!strcmp(s,"中/英")) {
        if(*composition){commit_text(composition);composition[0]=0;}
        chinese=!chinese;if(chinese&&!ime_available){chinese=0;message("拼音词典不可用，请使用英文");}
        upper=0;update_candidates();
    } else if(!strcmp(s,"大写")||!strcmp(s,"小写"))upper=!upper;
    else if(!strcmp(s,"数字")||!strcmp(s,"字母")) {
        if(*composition){commit_text(composition);composition[0]=0;}numeric=!numeric;update_candidates();
    } else if(!strcmp(s,"空格")) {
        if(*composition){char c[5];if(!ime_candidate(ime_lookup(composition),candidate_page*5,c)){commit_text(c);composition[0]=0;candidate_page=0;update_candidates();}}
        else commit_text(" ");
    } else if(chinese&&!numeric&&!upper&&s[0]>='a'&&s[0]<='z'&&!s[1]) {
        size_t n=strlen(composition);if(n<6){composition[n]=s[0];composition[n+1]=0;candidate_page=0;update_candidates();}
    } else {if(*composition){commit_text(composition);composition[0]=0;update_candidates();}commit_text(s);}
    lv_buttonmatrix_set_map(keyboard,numeric?number_map:upper?upper_map:lower_map);
}
static void name_page(void)
{
    heading("输入姓名",UI_USER_ID);
    text(parent,"姓名可留空，支持中文、字母和数字",20,174,440,&ky50_font_20,MUTED);
    name_area=lv_textarea_create(parent);lv_obj_set_pos(name_area,24,218);lv_obj_set_size(name_area,432,110);
    lv_obj_set_style_text_font(name_area,&ky50_font_24,0);lv_obj_set_style_text_color(name_area,lv_color_hex(TEXT),0);
    lv_obj_set_style_bg_opa(name_area,LV_OPA_COVER,0);lv_obj_set_style_radius(name_area,12,0);
    lv_obj_set_style_pad_all(name_area,12,0);lv_obj_set_style_border_width(name_area,1,0);
    lv_obj_set_style_border_color(name_area,lv_color_hex(0x36506a),0);
    lv_obj_set_style_border_width(name_area,2,LV_PART_CURSOR);lv_obj_set_style_border_color(name_area,lv_color_hex(ACCENT),LV_PART_CURSOR);
    lv_obj_set_style_bg_color(name_area,lv_color_hex(CARD),0);lv_textarea_set_max_length(name_area,32);lv_textarea_set_text(name_area,name);
    lv_textarea_set_cursor_pos(name_area,LV_TEXTAREA_CURSOR_LAST);
    composition_label=text(parent,"",24,338,432,&ky50_font_20,ACCENT);
    button(parent,12,380,48,52,"<",candidate_move,-1);
    for(int i=0;i<5;i++) {lv_obj_t *b=button(parent,66+i*70,380,64,52,NULL,choose_candidate,i);candidate_labels[i]=text(b,"",0,10,64,&ky50_font_24,TEXT);}
    button(parent,420,380,48,52,">",candidate_move,1);
    keyboard=lv_buttonmatrix_create(parent);lv_obj_set_pos(keyboard,8,450);lv_obj_set_size(keyboard,464,384);
    lv_obj_set_style_bg_color(keyboard,lv_color_hex(BG),0);lv_obj_set_style_border_width(keyboard,0,0);
    lv_obj_set_style_pad_all(keyboard,3,0);lv_obj_set_style_pad_gap(keyboard,5,0);
    lv_obj_set_style_text_font(keyboard,&ky50_font_20,LV_PART_ITEMS);lv_obj_set_style_text_color(keyboard,lv_color_hex(TEXT),LV_PART_ITEMS);
    lv_obj_set_style_bg_color(keyboard,lv_color_hex(CARD),LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(keyboard,LV_OPA_COVER,LV_PART_ITEMS);lv_obj_set_style_radius(keyboard,8,LV_PART_ITEMS);
    lv_obj_set_style_border_width(keyboard,1,LV_PART_ITEMS);lv_obj_set_style_border_color(keyboard,lv_color_hex(0x36506a),LV_PART_ITEMS);
    lv_obj_set_style_bg_color(keyboard,lv_color_hex(0x286078),LV_PART_ITEMS|LV_STATE_PRESSED);
    lv_buttonmatrix_set_map(keyboard,numeric?number_map:upper?upper_map:lower_map);
    lv_obj_add_event_cb(keyboard,name_key,LV_EVENT_VALUE_CHANGED,NULL);
    if(!ime_available){chinese=0;message("拼音词典不可用，请使用英文");}
    update_candidates();
}
static unsigned enrollment_kind=CREDENTIAL_PALM;
static void registration_type(lv_event_t *e)
{
    int type=(int)(intptr_t)lv_event_get_user_data(e);AppUser *u=selected_user();
    if(type>2){message("此注册方式暂未实现");return;}
    if(!u||u->deleting){message("用户不可用");return;}
    if((type==2?u->finger_state:type==0?u->face_state:u->palm_state)==CRED_REGISTERED){message("该方式已注册");return;}
    if(type==2?!data.finger_ready:type==0?!data.face_ready:!data.palm_ready){message("算法不可用");return;}
    enrollment_kind=type==2?CREDENTIAL_FINGER:type==0?CREDENTIAL_FACE:CREDENTIAL_PALM;
    AccessCommand c={.type=ACCESS_ENROLL,.local_id=u->local_id,.modality=enrollment_kind};if(!submit(&c))ui_show(UI_PALM_ENROLL,0);
}
static void types_page(void)
{
    heading("选择注册方式",UI_USERS);AppUser *u=selected_user();
    if(!u){message("用户不存在，请返回");return;}
    lv_obj_t *card=box(parent,24,180,432,110,CARD,255,12);show_avatar(card,u);
    text(card,*u->name?u->name:"未设置姓名",82,12,340,&ky50_font_24,TEXT);
    char id[64];snprintf(id,sizeof(id),"ID  %s",u->employee_id);text(card,id,82,54,340,&ky50_font_20,MUTED);
    static const char *titles[]={"人脸","掌静脉","指纹","卡","密码"};
    int row=0;
    for(int i=0;i<5;i++) {
        if(!registration_enabled[i])continue;
        lv_obj_t *b=button(parent,24,316+row++*100,432,86,NULL,registration_type,i);
        text(b,titles[i],22,26,150,&ky50_font_24,i<=2?TEXT:MUTED);
        text(b,u->registered&(1u<<i)?"已注册":i<=2?"点击注册":"暂未实现",208,28,202,&ky50_font_20,i<=2?ACCENT:MUTED);
    }
}
static void enroll_page(void)
{
    heading(enrollment_kind==CREDENTIAL_FINGER?"指纹注册":enrollment_kind==CREDENTIAL_FACE?"人脸注册":"掌静脉注册",UI_USER_TYPES);
    if(enrollment_kind==CREDENTIAL_FINGER) {
        lv_obj_set_style_bg_color(parent,lv_color_hex(BG),0);
        lv_obj_set_style_bg_opa(parent,LV_OPA_COVER,0);
        text(parent,"请使用同一手指完成5次按压",24,230,432,&ky50_font_24,TEXT);
        finger_round_label=text(parent,"第1次 / 共5次",24,298,432,&ky50_font_24,ACCENT);
        progress_label=text(parent,"0%",24,360,432,&ky50_font_24,TEXT);
        progress_bar=lv_bar_create(parent);lv_obj_set_pos(progress_bar,40,420);lv_obj_set_size(progress_bar,400,24);
        lv_bar_set_range(progress_bar,0,100);lv_bar_set_value(progress_bar,0,LV_ANIM_OFF);
        lv_obj_set_style_bg_color(progress_bar,lv_color_hex(0x36506a),0);
        lv_obj_set_style_bg_color(progress_bar,lv_color_hex(ACCENT),LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(progress_bar,LV_OPA_COVER,0);lv_obj_set_style_bg_opa(progress_bar,LV_OPA_COVER,LV_PART_INDICATOR);
        lv_obj_set_pos(message_label,28,492);lv_obj_set_width(message_label,424);
        text(parent,"每次采集10帧\n采满后请抬起手指，保持离开2秒",28,630,424,&ky50_font_20,MUTED);
        text(parent,"点击返回可取消本次注册",28,750,424,&ky50_font_20,MUTED);
        message("请将同一手指放在指纹采集区");return;
    }
    /* Transparent middle: the main RGB preview and IR-derived overlay remain
     * at full-screen coordinates. No additional inset or rescaling. */
    lv_obj_set_style_bg_opa(parent,0,0);
    box(parent,0,0,480,174,BG,220,0); /* Place below existing heading objects. */
    lv_obj_move_to_index(lv_obj_get_child(parent,-1),0);
    lv_obj_t *bottom=box(parent,0,674,480,180,BG,235,0);
    text(bottom,enrollment_kind==CREDENTIAL_FINGER?"请保持同一手指在指纹采集区":enrollment_kind==CREDENTIAL_FACE?"请正对摄像头，并移开手掌":"请保持同一只手掌，手指朝上",12,10,456,&ky50_font_20,TEXT);
    progress_bar=lv_bar_create(bottom);lv_obj_set_pos(progress_bar,24,58);lv_obj_set_size(progress_bar,432,20);
    lv_bar_set_range(progress_bar,0,data.total>0?data.total:1);lv_bar_set_value(progress_bar,data.progress,LV_ANIM_OFF);
    lv_obj_set_style_bg_color(progress_bar,lv_color_hex(0x36506a),0);
    lv_obj_set_style_bg_color(progress_bar,lv_color_hex(ACCENT),LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(progress_bar,LV_OPA_COVER,0);lv_obj_set_style_bg_opa(progress_bar,LV_OPA_COVER,LV_PART_INDICATOR);
    lv_obj_set_style_radius(progress_bar,10,0);lv_obj_set_style_radius(progress_bar,10,LV_PART_INDICATOR);
    progress_label=text(bottom,"准备采集",24,88,432,&ky50_font_24,TEXT);
    text(bottom,"返回可取消本次注册",20,130,440,&ky50_font_20,MUTED);
    message(enrollment_kind==CREDENTIAL_FINGER?"正在准备指纹画面":"正在准备 RGB 画面和红外采集");
}
void user_ui_before_page(enum ui_page next)
{
    if(current==UI_USER_NAME&&name_area)app_utf8_copy(name,sizeof(name),lv_textarea_get_text(name_area));
    if(current==UI_PALM_ENROLL&&next!=UI_PALM_ENROLL){AccessCommand c={.type=ACCESS_CANCEL};access_submit(&c);}
    access_enable_recognition(next==UI_HOME);
    current=next;parent=message_label=employee_label=name_area=keyboard=composition_label=progress_bar=progress_label=selected_label=finger_round_label=NULL;
    for(int i=0;i<5;i++)candidate_labels[i]=NULL;
}
int user_ui_build(lv_obj_t *root,enum ui_page next)
{
    if(next<UI_USERS||next>UI_USERS_MANAGE)return 0;
    parent=root;
    if(!ime_available)ime_available=ime_load("assets/ime/pinyin.tsv")==0;
    switch(next){case UI_USERS:users_page(0);break;case UI_USERS_MANAGE:users_page(1);break;
    case UI_USER_ID:id_page();break;case UI_USER_NAME:name_page();break;
    case UI_USER_TYPES:types_page();break;case UI_PALM_ENROLL:enroll_page();break;default:break;}
    return 1;
}
void user_ui_update(const AccessSnapshot *snapshot)
{
    int event_new=snapshot->event_sequence!=seen_event;
    data=*snapshot;
    if(event_new) {
        seen_event=data.event_sequence;
        if(data.event==ACCESS_EVENT_CREATED) {
            selected_id=data.selected_id;creating=0;
            if(current==UI_USER_NAME){ui_show(UI_USER_TYPES,0);message(data.message);return;}
        } else if(data.event==ACCESS_EVENT_ENROLLED) {
            if(current==UI_PALM_ENROLL){ui_show(UI_USER_TYPES,0);message(data.message);return;}
        } else if(data.event==ACCESS_EVENT_DELETED||data.event==ACCESS_EVENT_ERROR) {
            creating=0;
            if(current==UI_USERS_MANAGE)memset(selected,0,sizeof(selected));
            if(current==UI_PALM_ENROLL&&!data.registering){ui_show(UI_USER_TYPES,0);message(data.message);return;}
        }
    }
    if(current==UI_USERS||current==UI_USERS_MANAGE) {
        ui_show(current,0);if(event_new)message(data.message);
    } else if(current==UI_PALM_ENROLL) {
        if(enrollment_kind==CREDENTIAL_FINGER) {
            int percent=data.total>0?data.progress*100/data.total:0;if(percent<0)percent=0;if(percent>100)percent=100;
            if(progress_bar)lv_bar_set_value(progress_bar,percent,LV_ANIM_OFF);
            if(progress_label)lv_label_set_text_fmt(progress_label,"%d%%",percent);
            if(finger_round_label)lv_label_set_text_fmt(finger_round_label,"第%d次 / 共5次",data.finger_enroll_press>0?data.finger_enroll_press:1);
        } else {
            if(progress_bar){lv_bar_set_range(progress_bar,0,data.total>0?data.total:1);lv_bar_set_value(progress_bar,data.progress,LV_ANIM_OFF);}
            if(progress_label)lv_label_set_text_fmt(progress_label,"注册进度  %d / %d",data.progress,data.total);
        }
        message(data.message);
    } else if(event_new)message(data.message);
}
void user_ui_shutdown(void){ime_close();ime_available=0;}
const char *user_ui_employee_text(void){return employee;}
const char *user_ui_name_text(void){return name_area?lv_textarea_get_text(name_area):name;}
const char *user_ui_composition(void){return composition;}

unsigned user_ui_enrollment_kind(void){return enrollment_kind;}
