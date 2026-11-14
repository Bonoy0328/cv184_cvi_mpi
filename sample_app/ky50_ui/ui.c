#include "ui.h"
#include "lvgl.h"
#include "user_ui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

extern const lv_font_t ky50_font_20, ky50_font_24;
static lv_display_t *display;
static lv_indev_t *pointer;
static uint32_t *framebuffer, *drawbuf;
static lv_obj_t *root, *link_label, *clock_label, *camera_label, *input_label, *toast, *touch_label;
static lv_timer_t *toast_timer;
static lv_obj_t *factory_label, *source_label, *calibration_label, *calibration_progress;
static char calibration_text[192];
static int calibration_count=-1;
static lv_obj_t *light_buttons[2], *light_titles[2], *light_labels[2];
static int light_states[2]={-1,-1};
static lv_obj_t *recognition_label;
static AccessSnapshot access_view;
static PalmParameters parameter_draft;
static FaceParameters face_parameter_draft;
static AppParameters app_parameter_draft;
static lv_obj_t *parameter_value[3],*parameter_controls[6],*parameter_save,*parameter_message;
static lv_obj_t *parameter_cards[3],*parameter_title[3],*parameter_hint[3],*parameter_save_label,*parameter_page_label,*parameter_pages[2];
static int parameter_loaded[3],parameter_waiting[3],parameter_dirty[3],parameter_page;
static unsigned parameter_sequence[3];
static enum ui_action pending_action;
static lv_obj_t *rtsp_button,*rtsp_title;
static int rtsp_state;
static char factory_text[160]="请选择测试项目",source_name[8]="RGB";
static unsigned screen_color;
static const uint32_t test_colors[]={0xff0000,0x00ff00,0x0000ff,0xffffff,0x000000,0x808080};
static enum ui_page page;
static unsigned generation;
static int px,py,pressed,link_state=-2,camera_state=-1;
static char input[33],last_time[96];
static const char *names[]={"用户管理","数据管理","信息查询","U盘功能","门禁设置","考勤设置","通讯设置","高级设置"};
static const char *icons[]={LV_SYMBOL_EDIT,LV_SYMBOL_DIRECTORY,LV_SYMBOL_EYE_OPEN,LV_SYMBOL_USB,
    LV_SYMBOL_HOME,LV_SYMBOL_LIST,LV_SYMBOL_SHUFFLE,LV_SYMBOL_SETTINGS};
#define BG 0x0b1726
#define CARD 0x182a3e
#define ACCENT 0x51d5ee
#define TEXT 0xf3f7fc
#define MUTED 0x9eafc1

static uint32_t tick(void) { return (uint32_t)ui_monotonic_ms(); }
static void flush(lv_display_t *d,const lv_area_t *a,uint8_t *p)
{
    int w=a->x2-a->x1+1;
    for (int y=a->y1;y<=a->y2;y++)
        memcpy(framebuffer+(size_t)y*UI_WIDTH+a->x1,p+(size_t)(y-a->y1)*w*4,(size_t)w*4);
    if(lv_display_flush_is_last(d)) ++generation;
    lv_display_flush_ready(d);
}
static void read_pointer(lv_indev_t *in,lv_indev_data_t *d)
{
    (void)in; d->point.x=px; d->point.y=py;
    d->state=pressed?LV_INDEV_STATE_PRESSED:LV_INDEV_STATE_RELEASED;
}
static lv_obj_t *box(lv_obj_t *parent,int x,int y,int w,int h,uint32_t color,int opa,int radius)
{
    lv_obj_t *o=lv_obj_create(parent); lv_obj_remove_style_all(o);
    lv_obj_set_pos(o,x,y); lv_obj_set_size(o,w,h);
    lv_obj_set_style_bg_color(o,lv_color_hex(color),0);
    lv_obj_set_style_bg_opa(o,opa,0); lv_obj_set_style_radius(o,radius,0);
    lv_obj_set_scrollable(o,false); return o;
}
static lv_obj_t *label(lv_obj_t *parent,const char *text,int x,int y,int w,const lv_font_t *font,uint32_t color)
{
    lv_obj_t *o=lv_label_create(parent); lv_obj_remove_style_all(o);
    lv_obj_set_style_text_font(o,font,0); lv_obj_set_style_text_color(o,lv_color_hex(color),0);
    lv_obj_set_style_text_align(o,LV_TEXT_ALIGN_CENTER,0);
    lv_obj_set_pos(o,x,y); lv_obj_set_width(o,w); lv_label_set_text(o,text);
    lv_obj_set_clickable(o,false); return o;
}
static void nav(lv_event_t *e)
{
    int id=(int)(intptr_t)lv_event_get_user_data(e);
    if(id==100) ui_show(UI_USERS,0);
    else if(id==107) ui_show(UI_ADVANCED,0);
    else if(id>=100) ui_show(UI_FEATURE,id-100);
    else ui_show((enum ui_page)id,0);
}
static lv_obj_t *button(lv_obj_t *parent,int x,int y,int w,int h,lv_event_cb_t cb,int id)
{
    lv_obj_t *o=box(parent,x,y,w,h,CARD,225,16);
    lv_obj_set_clickable(o,true);
    lv_obj_set_style_border_width(o,1,0);
    lv_obj_set_style_border_color(o,lv_color_hex(0x36506a),0);
    lv_obj_set_style_bg_color(o,lv_color_hex(0x286078),LV_STATE_PRESSED);
    lv_obj_set_style_border_color(o,lv_color_hex(ACCENT),LV_STATE_PRESSED);
    lv_obj_add_event_cb(o,cb,LV_EVENT_CLICKED,(void *)(intptr_t)id); return o;
}
static void topbar(void)
{
    lv_obj_t *b=box(root,0,0,480,52,BG,175,0);
    /* Ethernet jack pictogram uses the same stroke/color vocabulary as icons. */
    lv_obj_t *jack=box(b,20,16,24,19,BG,0,3);
    lv_obj_set_style_border_width(jack,2,0);
    lv_obj_set_style_border_color(jack,lv_color_hex(ACCENT),0);
    for(int i=0;i<4;i++) box(jack,4+i*4,2,2,5,ACCENT,255,0);
    link_label=label(b,"检测网线",52,12,180,&ky50_font_20,TEXT);
    label(b,LV_SYMBOL_WIFI,300,16,28,&lv_font_montserrat_20,MUTED);
    label(b,"未启用",332,12,128,&ky50_font_20,MUTED);
}
static void heading(const char *title,enum ui_page back)
{
    lv_obj_t *b=button(root,20,72,64,54,nav,back);
    label(b,LV_SYMBOL_LEFT,0,14,64,&lv_font_montserrat_24,TEXT);
    label(root,title,94,80,292,&ky50_font_24,TEXT);
    box(root,24,146,432,1,0x34516b,180,0);
}
/* Palm, three face pages, then two application pages. */
static const struct {const char *title,*hint;int min,max,toggle;} parameter_rows[]={
    {"手掌检测阈值","范围 0.00-1.00，步长 0.01",0,100,0},
    {"识别相似度阈值","范围 0.00-1.00，步长 0.01",0,100,0},
    {"动态注册等级","范围 0-4，等级越高越严格",0,4,0},
    {"人脸检测阈值","范围 0.01-1.00，步长 0.01",1,100,0},
    {"人脸质量检测","关闭 / 开启",0,1,1},
    {"人脸质量阈值","范围 0.00-1.00，步长 0.01",0,100,0},
    {"RGB活体检测","仅控制RGB，红外单独设置",0,1,1},
    {"人脸活体阈值","范围 0.00-1.00，步长 0.01",0,100,0},
    {"人脸认证阈值","范围 -1.00-1.00，步长 0.01",-100,100,0},
    {"人脸过滤阈值","范围 -1.00-1.00，步长 0.01",-100,100,0},
    {"掌静脉离场帧数","连续未检测到，范围 1-100 帧",1,APP_MAX_LEAVE_FRAMES,0},
    {"人脸离场帧数","连续未检测到，范围 1-100 帧",1,APP_MAX_LEAVE_FRAMES,0},
    {"继电器开启秒数","识别成功后开启，范围 1-60 秒",1,APP_MAX_RELAY_SECONDS,0},
    {"匹配确认次数","人脸与掌静脉共用，范围 1-10 次",1,APP_MAX_MATCH_CONFIRMATIONS,0},
    {"成功冷却秒数","两种模态共用，0 秒不额外等待",0,APP_MAX_SUCCESS_COOLDOWN_SECONDS,0},
    {"强活体检测","仅控制IR，可与RGB同时开启",0,1,1}
};
static const int parameter_first[]={0,3,6,9,10,13},parameter_count[]={3,3,3,1,3,3};
static int parameter_kind(void){return parameter_page>=4?2:parameter_page!=0;}
static int *parameter_field(int row)
{
    int *fields[]={&parameter_draft.hand_threshold,&parameter_draft.auth_threshold,&parameter_draft.dynamic_level,
        &face_parameter_draft.detection_threshold,&face_parameter_draft.quality_enable,&face_parameter_draft.quality_threshold,
        &face_parameter_draft.liveness_enable,&face_parameter_draft.liveness_threshold,
        &face_parameter_draft.auth_threshold,&face_parameter_draft.filter_threshold,
        &app_parameter_draft.palm_leave_frames,&app_parameter_draft.face_leave_frames,&app_parameter_draft.relay_seconds,
        &app_parameter_draft.match_confirmations,&app_parameter_draft.success_cooldown_seconds,&app_parameter_draft.strong_liveness};
    return fields[row];
}
static void parameter_refresh(void)
{
    if(page!=UI_PARAMETERS||!parameter_message)return;
    int ready[]={access_view.parameters_ready,access_view.face_parameters_ready,access_view.app_parameters_ready};
    unsigned sequence[]={access_view.parameters_sequence,access_view.face_parameters_sequence,access_view.app_parameters_sequence};
    for(int k=0;k<3;k++) {
        if(parameter_waiting[k]&&sequence[k]!=parameter_sequence[k])parameter_waiting[k]=parameter_dirty[k]=0;
        if(!parameter_dirty[k]&&!parameter_waiting[k]&&ready[k]) {
            if(k==2)app_parameter_draft=access_view.app_parameters;
            else if(k)face_parameter_draft=access_view.face_parameters;else parameter_draft=access_view.parameters;
            parameter_loaded[k]=1;
        }
    }
    int kind=parameter_kind();
    int enabled=parameter_loaded[kind]&&ready[kind]&&!parameter_waiting[kind]&&!access_view.registering;
    for(int i=0;i<3;i++) {
        int row=parameter_first[parameter_page]+i;
        lv_obj_set_hidden(parameter_cards[i],i>=parameter_count[parameter_page]);if(i>=parameter_count[parameter_page])continue;
        int value=*parameter_field(row);
        lv_label_set_text(parameter_title[i],parameter_rows[row].title);
        lv_label_set_text(parameter_hint[i],parameter_rows[row].hint);
        lv_obj_set_style_text_font(parameter_value[i],parameter_rows[row].toggle?&ky50_font_24:&lv_font_montserrat_32,0);
        if(!parameter_loaded[kind])lv_label_set_text(parameter_value[i],"--");
        else if(parameter_rows[row].toggle)lv_label_set_text(parameter_value[i],value?"开启":"关闭");
        else if(row==2||row>=10)lv_label_set_text_fmt(parameter_value[i],"%d",value);
        else lv_label_set_text_fmt(parameter_value[i],"%s%d.%02d",value<0?"-":"",abs(value)/100,abs(value)%100);
        for(int k=0;k<2;k++)lv_obj_set_state(parameter_controls[2*i+k],LV_STATE_DISABLED,
            !enabled||(k?value>=parameter_rows[row].max:value<=parameter_rows[row].min));
    }
    lv_label_set_text_fmt(parameter_page_label,"%s %d/6",kind==2?"应用":kind?"人脸":"掌静脉",parameter_page+1);
    for(int k=0;k<2;k++)lv_obj_set_state(parameter_pages[k],LV_STATE_DISABLED,k?parameter_page==5:parameter_page==0);
    lv_obj_set_state(parameter_save,LV_STATE_DISABLED,!enabled);
    lv_label_set_text(parameter_save_label,kind==2?"保存应用参数":kind?"保存人脸":"保存掌静脉");
    const char *message=kind==2?access_view.app_parameters_message:kind?access_view.face_parameters_message:access_view.parameters_message;
    lv_label_set_text(parameter_message,parameter_waiting[kind]?"正在保存，请稍候":
        parameter_dirty[kind]?"尚未保存":*message?message:"算法启动中，请稍候");
}
static void parameter_adjust(lv_event_t *e)
{
    int kind=parameter_kind();
    if(!parameter_loaded[kind]||parameter_waiting[kind]||access_view.registering)return;
    int id=(int)(intptr_t)lv_event_get_user_data(e),row=parameter_first[parameter_page]+id/2;
    if(id/2>=parameter_count[parameter_page])return;
    int *value=parameter_field(row),next=*value+(id%2?1:-1);
    if(next>=parameter_rows[row].min&&next<=parameter_rows[row].max){*value=next;parameter_dirty[kind]=1;}
    parameter_refresh();
}
static void parameter_turn(lv_event_t *e)
{
    int next=parameter_page+((intptr_t)lv_event_get_user_data(e)?1:-1);
    if(next>=0&&next<6){parameter_page=next;parameter_refresh();}
}
static void parameter_submit(lv_event_t *e)
{
    (void)e;
    int kind=parameter_kind();
    if(!parameter_loaded[kind]||parameter_waiting[kind])return;
    AccessCommand c={.type=kind==2?ACCESS_APP_PARAMETERS:kind?ACCESS_FACE_PARAMETERS:ACCESS_PARAMETERS,
        .parameters=parameter_draft,.face_parameters=face_parameter_draft,.app_parameters=app_parameter_draft};
    parameter_sequence[kind]=kind==2?access_view.app_parameters_sequence:kind?access_view.face_parameters_sequence:access_view.parameters_sequence;
    if(access_submit(&c)){lv_label_set_text(parameter_message,"操作繁忙，请重试");return;}
    parameter_waiting[kind]=1;parameter_refresh();
}
static void parameters_page(void)
{
    heading("参数设置",UI_ADVANCED);
    memset(parameter_loaded,0,sizeof(parameter_loaded));memset(parameter_waiting,0,sizeof(parameter_waiting));
    memset(parameter_dirty,0,sizeof(parameter_dirty));parameter_page=0;
    parameter_draft=(PalmParameters){0};face_parameter_draft=(FaceParameters){0};app_parameter_draft=(AppParameters){0};
    for(int i=0;i<3;i++) {
        int y=168+i*166;lv_obj_t *card=box(root,24,y,432,152,CARD,240,16);
        parameter_cards[i]=card;parameter_title[i]=label(card,"",16,12,400,&ky50_font_24,TEXT);
        for(int k=0;k<2;k++) {
            lv_obj_t *b=button(card,k?326:24,52,82,58,parameter_adjust,2*i+k);
            parameter_controls[2*i+k]=b;
            label(b,k?"+":"-",0,10,82,&lv_font_montserrat_32,ACCENT);
            lv_obj_set_style_bg_opa(b,80,LV_STATE_DISABLED);
        }
        parameter_value[i]=label(card,"--",120,62,192,&lv_font_montserrat_32,TEXT);
        parameter_hint[i]=label(card,"",8,120,416,&ky50_font_20,MUTED);
    }
    parameter_message=label(root,"",24,682,432,&ky50_font_20,ACCENT);
    for(int k=0;k<2;k++) {
        parameter_pages[k]=button(root,k?328:24,716,128,46,parameter_turn,k);
        label(parameter_pages[k],k?"下一页":"上一页",0,8,128,&ky50_font_20,TEXT);
    }
    parameter_page_label=label(root,"",152,724,176,&ky50_font_20,MUTED);
    parameter_save=button(root,24,774,208,64,parameter_submit,0);
    parameter_save_label=label(parameter_save,"",0,18,208,&ky50_font_24,TEXT);
    lv_obj_t *back=button(root,248,774,208,64,nav,UI_ADVANCED);
    label(back,"返回",0,18,208,&ky50_font_24,TEXT);
    parameter_refresh();
}
static void hide_toast(lv_timer_t *timer)
{
    (void)timer; if(toast) lv_obj_set_hidden(toast,true); toast_timer=NULL;
}
static void bell(lv_event_t *e)
{
    (void)e;
    if(!toast) toast=label(root,"",40,610,400,&ky50_font_24,TEXT);
    lv_label_set_text(toast,"门铃暂未实现");
    lv_obj_set_hidden(toast,false);
    if(toast_timer) lv_timer_delete(toast_timer);
    toast_timer=lv_timer_create(hide_toast,1500,NULL); lv_timer_set_repeat_count(toast_timer,1);
}
void ui_factory_status(const char *text)
{
    snprintf(factory_text,sizeof(factory_text),"%s",text);
    if(factory_label) lv_label_set_text(factory_label,factory_text);
    if(page==UI_HOME && toast) lv_label_set_text(toast,text);
}
void ui_face_ir_calibration_status(const FaceIrCalibrationStatus *state)
{
    if(!state||!calibration_label||!calibration_progress)return;
    if(strcmp(calibration_text,state->message)) {
        snprintf(calibration_text,sizeof(calibration_text),"%s",state->message);
        lv_label_set_text(calibration_label,calibration_text);++generation;
    }
    if(calibration_count!=state->count) {
        calibration_count=state->count;
        lv_label_set_text_fmt(calibration_progress,"有效配对：%d / %d",state->count,FACE_IR_CALIBRATION_FRAMES);++generation;
    }
}
void ui_factory_lights(int white,int infrared)
{
    light_states[0]=white;light_states[1]=infrared;
    for(int i=0;i<2;i++) {
        if(!light_labels[i]) continue;
        int state=light_states[i];
        lv_label_set_text_fmt(light_titles[i],"%s%s",state>0?"关闭":"开启",i?"红外灯":"白光灯");
        lv_label_set_text(light_labels[i],state<0?"设备不可用":state?"当前：开启":"当前：关闭");
        lv_obj_set_style_text_color(light_labels[i],lv_color_hex(state>0?ACCENT:MUTED),0);
        lv_obj_set_style_border_color(light_buttons[i],lv_color_hex(state>0?ACCENT:0x36506a),0);
    }
}
void ui_factory_rtsp(int state)
{
    rtsp_state=state;
    if(!rtsp_button)return;
    lv_label_set_text(rtsp_title,state==2?"关闭RTSP":state==1?"正在开启":state==3?"正在关闭":"开启RTSP");
    lv_obj_set_state(rtsp_button,LV_STATE_DISABLED,state==1||state==3);
    lv_obj_set_style_border_color(rtsp_button,lv_color_hex(state==2?ACCENT:0x36506a),0);
}
void ui_set_source(const char *name)
{
    snprintf(source_name,sizeof(source_name),"%s",name);
    if(source_label) lv_label_set_text_fmt(source_label,"当前显示：%s",source_name);
}
enum ui_action ui_take_action(void)
{
    enum ui_action a=pending_action;pending_action=UI_ACTION_NONE;return a;
}
static void factory_action(lv_event_t *e)
{
    int id=(int)(intptr_t)lv_event_get_user_data(e);
    if(id==0) { screen_color=0;ui_show(UI_SCREEN_TEST,0);return; }
    static const enum ui_action actions[]={UI_ACTION_NONE,UI_ACTION_RGB,UI_ACTION_IR,
        UI_ACTION_DVP,UI_ACTION_RELAY,UI_ACTION_NONE,UI_ACTION_NONE,UI_ACTION_WHITE_LIGHT,
        UI_ACTION_IR_LIGHT,UI_ACTION_SPEAKER,UI_ACTION_NONE,UI_ACTION_NONE,UI_ACTION_NONE,UI_ACTION_FACE_IR_CALIBRATE,UI_ACTION_RTSP};
    if(id<0||id>=15) return;
    if(!actions[id]) { ui_factory_status("此项目暂未实现");return; }
    if(!pending_action) { pending_action=actions[id];ui_factory_status("正在处理，请稍候"); }
}
static void next_color(lv_event_t *e)
{
    (void)e;
    if(++screen_color==sizeof(test_colors)/sizeof(test_colors[0])) {
        ui_show(UI_FACTORY,0);ui_factory_status("屏幕测试完成");
    } else lv_obj_set_style_bg_color(root,lv_color_hex(test_colors[screen_color]),0);
}
static void key(lv_event_t *e)
{
    int k=(int)(intptr_t)lv_event_get_user_data(e); size_t n=strlen(input);
    if(k==10) input[0]=0;
    else if(k==11) { if(n) input[n-1]=0; }
    else if(n<sizeof(input)-1) { input[n]='0'+k; input[n+1]=0; }
    lv_label_set_text(input_label,*input?input:"请输入数字");
}
void ui_show(enum ui_page next,int feature)
{
    user_ui_before_page(next);
    if(toast_timer) { lv_timer_delete(toast_timer);toast_timer=NULL; }
    link_label=clock_label=camera_label=input_label=toast=touch_label=factory_label=source_label=NULL;
    rtsp_button=rtsp_title=NULL;
    calibration_label=calibration_progress=NULL;calibration_text[0]=0;calibration_count=-1;
    memset(light_buttons,0,sizeof(light_buttons));
    memset(light_titles,0,sizeof(light_titles));
    memset(light_labels,0,sizeof(light_labels));
    recognition_label=parameter_message=parameter_save=NULL;
    lv_obj_remove_event_cb(root,next_color);
    lv_obj_clean(root); page=next; last_time[0]=0; link_state=-2;camera_state=-1;
    lv_obj_set_style_bg_color(root,lv_color_hex(BG),0);
    lv_obj_set_style_bg_opa(root,page==UI_HOME||page==UI_CAMERA_PREVIEW||page==UI_FACE_IR_CALIBRATE?0:225,0);
    if(page==UI_SCREEN_TEST) {
        lv_obj_set_style_bg_opa(root,255,0);
        lv_obj_set_style_bg_color(root,lv_color_hex(test_colors[screen_color]),0);
        lv_obj_set_clickable(root,true);
        lv_obj_add_event_cb(root,next_color,LV_EVENT_CLICKED,NULL);
        printf("UI page=%d solid_color=%u\n",page,screen_color);return;
    }
    if(page==UI_CAMERA_PREVIEW) {
        lv_obj_t *bar=box(root,0,0,480,70,BG,170,0);
        lv_obj_t *back=button(bar,12,8,64,54,nav,UI_FACTORY);
        label(back,LV_SYMBOL_LEFT,0,14,64,&lv_font_montserrat_24,TEXT);
        source_label=label(bar,"",92,22,360,&ky50_font_24,TEXT);
        lv_label_set_text_fmt(source_label,"当前显示：%s",source_name);
        camera_label=label(root,"等待摄像头",40,390,400,&ky50_font_24,TEXT);
        return;
    }
    if(page==UI_FACE_IR_CALIBRATE) {
        lv_obj_t *bar=box(root,0,0,480,130,BG,220,0);
        lv_obj_t *back=button(bar,12,10,64,54,nav,UI_FACTORY);
        label(back,LV_SYMBOL_LEFT,0,14,64,&lv_font_montserrat_24,TEXT);
        label(bar,"人脸双摄对齐",92,23,360,&ky50_font_24,TEXT);
        label(bar,"请保持一人在画面中，正对摄像头",24,84,432,&ky50_font_20,TEXT);
        lv_obj_t *bottom=box(root,0,626,480,228,BG,220,0);
        calibration_progress=label(bottom,"",24,16,432,&ky50_font_24,ACCENT);
        calibration_label=label(bottom,"",24,65,432,&ky50_font_20,TEXT);
        label(bottom,"保持常用距离，缓慢小幅移动\n完成后返回，参数自动保存",24,151,432,&ky50_font_20,MUTED);
        return;
    }
    topbar();
    if(user_ui_build(root,page)) {printf("UI page=%d users\n",page);return;}
    if(page==UI_HOME) {
        camera_label=label(root,"等待摄像头",60,395,360,&ky50_font_24,TEXT);
        recognition_label=label(root,"",24,116,432,&ky50_font_24,0x36ff70);
        lv_obj_set_style_bg_color(recognition_label,lv_color_hex(BG),0);
        lv_obj_set_style_bg_opa(recognition_label,190,0);
        lv_obj_set_style_pad_all(recognition_label,8,0);
        lv_obj_set_hidden(recognition_label,true);
        lv_obj_t *b=box(root,0,694,480,160,BG,185,0);
        clock_label=label(b,"",12,12,456,&ky50_font_24,TEXT);
        const char *s[]={LV_SYMBOL_KEYBOARD,LV_SYMBOL_BELL,LV_SYMBOL_LIST};
        const char *t[]={"键盘","门铃","菜单"};
        for(int i=0;i<3;i++) {
            lv_obj_t *bt=button(b,24+i*148,58,136,84,i==1?bell:nav,i==0?UI_INPUT:UI_MENU);
            label(bt,s[i],0,9,136,&lv_font_montserrat_32,ACCENT);
            label(bt,t[i],0,48,136,&ky50_font_20,TEXT);
        }
    } else if(page==UI_MENU) {
        heading("功能菜单",UI_HOME);
        for(int i=0;i<8;i++) {
            lv_obj_t *bt=button(root,24+(i%2)*224,172+(i/2)*158,208,140,nav,100+i);
            label(bt,icons[i],0,26,208,&lv_font_montserrat_40,ACCENT);
            label(bt,names[i],0,86,208,&ky50_font_24,TEXT);
        }
    } else if(page==UI_INPUT) {
        heading("数字输入",UI_HOME); input[0]=0;
        lv_obj_t *field=box(root,24,175,432,126,CARD,255,16);
        input_label=label(field,"请输入数字",16,24,400,&ky50_font_24,TEXT);
        lv_label_set_long_mode(input_label,LV_LABEL_LONG_WRAP);
        const char *keys[]={"1","2","3","4","5","6","7","8","9","清空","0","删除"};
        for(int i=0;i<12;i++) {
            int v=i<9?i+1:i==9?10:i==10?0:11;
            lv_obj_t *bt=button(root,24+(i%3)*148,330+(i/3)*119,136,103,key,v);
            label(bt,keys[i],0,32,136,i<9||i==10?&lv_font_montserrat_32:&ky50_font_24,TEXT);
        }
    } else if(page==UI_ADVANCED) {
        heading("高级设置",UI_MENU);
        lv_obj_t *bt=button(root,24,180,432,112,nav,UI_FACTORY);
        label(bt,LV_SYMBOL_SETTINGS,14,34,70,&lv_font_montserrat_32,ACCENT);
        label(bt,"工厂测试",84,35,320,&ky50_font_24,TEXT);
        bt=button(root,24,312,432,112,nav,UI_PARAMETERS);
        label(bt,LV_SYMBOL_EDIT,14,34,70,&lv_font_montserrat_32,ACCENT);
        label(bt,"参数设置",84,35,320,&ky50_font_24,TEXT);
    } else if(page==UI_PARAMETERS) {
        parameters_page();
    } else if(page==UI_FACTORY) {
        heading("工厂测试",UI_ADVANCED);
        source_label=label(root,"",24,153,220,&ky50_font_20,ACCENT);
        rtsp_button=button(root,254,142,202,42,factory_action,14);
        rtsp_title=label(rtsp_button,"",12,5,178,&ky50_font_20,TEXT);
        ui_factory_rtsp(rtsp_state);
        lv_label_set_text_fmt(source_label,"当前显示：%s",source_name);
        factory_label=label(root,factory_text,24,187,432,&ky50_font_20,TEXT);
        static const char *titles[]={"屏幕测试模式","RGB显示","IR显示","DVP显示",
            "继电器测试","门铃测试","韦根测试","开启白光灯","开启红外灯",
            "Speaker测试","MIC测试","IC测试","ID测试","人脸双摄对齐"};
        static const char *symbols[]={LV_SYMBOL_IMAGE,LV_SYMBOL_VIDEO,LV_SYMBOL_VIDEO,
            LV_SYMBOL_VIDEO,LV_SYMBOL_POWER,LV_SYMBOL_BELL,LV_SYMBOL_SHUFFLE,
            LV_SYMBOL_CHARGE,LV_SYMBOL_CHARGE,LV_SYMBOL_AUDIO,LV_SYMBOL_AUDIO,
            LV_SYMBOL_SD_CARD,LV_SYMBOL_SD_CARD,LV_SYMBOL_REFRESH};
        for(int i=0;i<14;i++) {
            lv_obj_t *bt=button(root,24+(i%2)*224,231+(i/2)*85,208,76,factory_action,i);
            int placeholder=i==5||i==6||(i>=10&&i<=12);
            int light=i==7||i==8;
            label(bt,symbols[i],8,24,34,&lv_font_montserrat_20,placeholder?MUTED:ACCENT);
            lv_obj_t *title=label(bt,titles[i],43,placeholder||light?8:22,159,&ky50_font_20,placeholder?MUTED:TEXT);
            if(light) {
                int index=i-7;
                light_buttons[index]=bt;light_titles[index]=title;
                light_labels[index]=label(bt,"",43,40,159,&ky50_font_20,MUTED);
            }
            if(placeholder) label(bt,"未实现",43,40,159,&ky50_font_20,MUTED);
        }
        ui_factory_lights(light_states[0],light_states[1]);
    } else if(page==UI_FEATURE) {
        if(feature<0||feature>7) feature=0;
        heading(names[feature],UI_MENU);
        label(root,icons[feature],0,304,480,&lv_font_montserrat_40,ACCENT);
        label(root,"功能建设中",40,380,400,&ky50_font_24,TEXT);
        lv_obj_t *bt=button(root,120,490,240,64,nav,UI_MENU);
        label(bt,"返回菜单",0,16,240,&ky50_font_24,TEXT);
    } else {
        heading("触控测试",UI_HOME);
        touch_label=label(root,"请点击四角和中心",20,390,440,&ky50_font_24,TEXT);
        const int x[]={24,432,24,432,228},y[]={166,166,800,800,460};
        for(int i=0;i<5;i++) box(root,x[i],y[i],24,24,ACCENT,255,12);
    }
    printf("UI page=%d feature=%d\n",page,feature);
}
int ui_init(void)
{
    lv_init(); lv_tick_set_cb(tick);
    framebuffer=calloc(UI_WIDTH*UI_HEIGHT,4); drawbuf=calloc(UI_WIDTH*64,4);
    if(!framebuffer||!drawbuf) { free(framebuffer); free(drawbuf); return -1; }
    display=lv_display_create(UI_WIDTH,UI_HEIGHT);
    if(!display) return -1;
    lv_display_set_color_format(display,LV_COLOR_FORMAT_ARGB8888);
    lv_display_set_buffers(display,drawbuf,NULL,UI_WIDTH*64*4,LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display,flush);
    root=lv_screen_active(); lv_obj_remove_style_all(root);
    lv_obj_set_scrollable(root,false);
    pointer=lv_indev_create(); lv_indev_set_type(pointer,LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(pointer,read_pointer);
    ui_show(UI_HOME,0); return 0;
}
void ui_pointer(int x,int y,int down)
{
    px=x<0?0:x>=480?479:x; py=y<0?0:y>=854?853:y; pressed=!!down;
    if(touch_label) lv_label_set_text_fmt(touch_label,"%s  x=%d y=%d",down?"按下":"抬起",px,py);
    /* Preserve each evdev SYN_REPORT, even when DOWN and UP arrive together. */
    lv_indev_read(pointer);
}
void ui_update(int eth,time_t now,int cam)
{
    if(link_label && link_state!=eth) {
        lv_label_set_text(link_label,eth>0?"网线已连接":eth==0?"网线未连接":"网口不可用");
        lv_obj_set_style_text_color(link_label,lv_color_hex(eth>0?ACCENT:MUTED),0);link_state=eth;
    }
    if(clock_label) {
        char t[96];ui_time_text(now,t,sizeof(t));
        if(strcmp(t,last_time)) { lv_label_set_text(clock_label,t);strcpy(last_time,t); }
    }
    if(camera_label&&cam!=camera_state) {
        if(cam) lv_obj_set_hidden(camera_label,true);
        else { lv_label_set_text(camera_label,"等待摄像头");lv_obj_set_hidden(camera_label,false); }
        camera_state=cam;
    }
}
void ui_process(void) { lv_timer_handler(); }
void ui_access_poll(void)
{
    if(access_snapshot(&access_view,access_view.revision)) {
        user_ui_update(&access_view);
        parameter_refresh();
        if(recognition_label) {
            lv_label_set_text(recognition_label,access_view.recognition);
            lv_obj_set_hidden(recognition_label,!*access_view.recognition);
            lv_obj_set_style_text_color(recognition_label,lv_color_hex(access_view.matched>0?0x36ff70:0xffdf38),0);
        }
    }
}
const AccessSnapshot *ui_access_state(void) { return &access_view; }
/* Flush after input, status and page callbacks have all been applied. */
void ui_render(void) { lv_refr_now(display); }
const uint32_t *ui_pixels(void) { return framebuffer; }
unsigned ui_generation(void) { return generation; }
enum ui_page ui_current_page(void) { return page; }
const char *ui_input_text(void) { return input; }
void ui_destroy(void)
{
    user_ui_shutdown();
    lv_deinit(); free(framebuffer);free(drawbuf);framebuffer=drawbuf=NULL;
    toast_timer=NULL;
}
