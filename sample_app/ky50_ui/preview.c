/* Real LVGL renderer + real input callbacks, headless host verification. */
#include "ui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include "settings.h"
#include <fcntl.h>
#include "user_ui.h"
#include "vendor/lvgl/src/widgets/buttonmatrix/lv_buttonmatrix_private.h"
static int business_active;
static uint64_t business_time=10000;
static void settings_check(void)
{
    char dir[]="/tmp/ky50-settings-XXXXXX",path[160],tmp[160];
    assert(mkdtemp(dir));enum camera_source source;
    assert(!settings_load(dir,&source)&&source==CAMERA_RGB);
    assert(!settings_save(dir,CAMERA_IR));
    snprintf(path,sizeof(path),"%s/display-source",dir);
    FILE *old=fopen(path,"rb");assert(old);
    assert(!settings_save(dir,CAMERA_DVP));
    char data[16]={0};assert(fread(data,1,15,old)==3&&!strcmp(data,"IR\n"));fclose(old);
    assert(!settings_load(dir,&source)&&source==CAMERA_DVP);
    snprintf(tmp,sizeof(tmp),"%s/display-source.tmp",dir);
    assert(!symlink(path,tmp));assert(settings_save(dir,CAMERA_RGB)<0);
    assert(!settings_load(dir,&source)&&source==CAMERA_DVP);
    FILE *f=fopen(path,"wb");assert(f);fputs("DV",f);fclose(f);
    assert(settings_load(dir,&source)<0&&source==CAMERA_RGB);
    unlink(tmp);unlink(path);rmdir(dir);
}
static void step(void) { for(int i=0;i<4;i++) {if(business_active){access_poll(business_time+=20);ui_access_poll();}ui_process();usleep(18000);} }
static void tap(int x,int y) {ui_pointer(x,y,1);step();ui_pointer(x,y,0);step();}
static void save(const char *dir,const char *name)
{
    char path[512];snprintf(path,sizeof(path),"%s/%s.ppm",dir,name);
    FILE *f=fopen(path,"wb");assert(f);
    fprintf(f,"P6\n480 854\n255\n");
    const uint32_t *p=ui_pixels();
    for(int y=0;y<854;y++)for(int x=0;x<480;x++) {
        unsigned v=p[y*480+x],a=v>>24;
        unsigned char b[3]={25+x/5,40+y/10,70+x/8};
        for(int c=0;c<3;c++) b[c]=(((v>>(16-8*c))&255)*a+b[c]*(255-a)+127)/255;
        fwrite(b,1,3,f);
    }
    fclose(f);
}
#include "tools/user_ui_test.inc"
static lv_obj_t *find_label(lv_obj_t *obj,const char *text)
{
    if(lv_obj_check_type(obj,&lv_label_class)&&!strcmp(lv_label_get_text(obj),text)) return obj;
    for(unsigned i=0;i<lv_obj_get_child_count(obj);i++) {
        lv_obj_t *found=find_label(lv_obj_get_child(obj,i),text);
        if(found) return found;
    }
    return NULL;
}
static void lights_check(const char *dir)
{
    ui_factory_lights(0,0);ui_factory_status("请选择测试项目");step();save(dir,"factory-lights-off");
    assert(find_label(lv_screen_active(),"开启白光灯"));
    assert(find_label(lv_screen_active(),"开启红外灯"));
    tap(349,521);assert(ui_take_action()==UI_ACTION_WHITE_LIGHT);
    /* The button must wait for the hardware result, not show optimistic ON. */
    assert(find_label(lv_screen_active(),"开启白光灯"));
    ui_factory_lights(1,0);
    assert(find_label(lv_screen_active(),"关闭白光灯"));
    assert(find_label(lv_screen_active(),"开启红外灯"));
    tap(125,606);assert(ui_take_action()==UI_ACTION_IR_LIGHT);
    ui_factory_lights(1,1);ui_factory_status("红外灯已开启");step();save(dir,"factory-lights-on");
    assert(find_label(lv_screen_active(),"关闭红外灯"));
    ui_show(UI_CAMERA_PREVIEW,0);ui_factory_lights(0,1);step();
    tap(45,35);assert(ui_current_page()==UI_FACTORY);
    assert(find_label(lv_screen_active(),"开启白光灯"));
    assert(find_label(lv_screen_active(),"关闭红外灯"));
    tap(125,606);assert(ui_take_action()==UI_ACTION_IR_LIGHT);
    ui_factory_lights(0,0);
    ui_factory_lights(-1,-1);ui_factory_status("灯控设备不可用或操作失败");step();save(dir,"factory-lights-unavailable");
    assert(find_label(lv_screen_active(),"设备不可用"));
    tap(349,521);assert(ui_take_action()==UI_ACTION_WHITE_LIGHT);
    assert(find_label(lv_screen_active(),"设备不可用"));
    ui_factory_lights(0,0);
    puts("PASS factory light touch: independent actions, result-driven states, page return, unavailable retry");
}
static void rtsp_check(const char *dir)
{
    ui_show(UI_FACTORY,0);ui_factory_rtsp(0);step();
    assert(find_label(lv_screen_active(),"开启RTSP"));
    tap(352,163);assert(ui_take_action()==UI_ACTION_RTSP);
    assert(find_label(lv_screen_active(),"开启RTSP"));
    ui_factory_rtsp(1);step();tap(352,163);assert(ui_take_action()==UI_ACTION_NONE);
    ui_factory_rtsp(2);ui_factory_status("RTSP已开启：8554 /rgb /ir /dvp");step();save(dir,"factory-rtsp-on");
    assert(find_label(lv_screen_active(),"关闭RTSP"));
    ui_show(UI_CAMERA_PREVIEW,0);step();tap(45,35);
    assert(find_label(lv_screen_active(),"关闭RTSP"));
    tap(352,163);assert(ui_take_action()==UI_ACTION_RTSP);
    ui_factory_rtsp(3);step();tap(352,163);assert(ui_take_action()==UI_ACTION_NONE);
    ui_factory_rtsp(-1);step();tap(352,163);assert(ui_take_action()==UI_ACTION_RTSP);
    ui_factory_rtsp(0);step();save(dir,"factory-rtsp-off");
    puts("PASS RTSP touch: result-driven state, busy suppression, return, stop and failure retry");
}
static void clean_preview_check(const char *dir)
{
    ui_show(UI_HOME,0);step();ui_render();
    assert(lv_obj_get_child_count(lv_layer_top())==0);
    /* Former infrared status strip is now transparent. */
    for(int y=64;y<100;y++)for(int x=12;x<298;x++)assert((ui_pixels()[y*480+x]>>24)==0);
    save(dir,"home-no-hand-status");
    ui_show(UI_CAMERA_PREVIEW,0);step();ui_render();
    for(int y=82;y<118;y++)for(int x=12;x<298;x++)assert((ui_pixels()[y*480+x]>>24)==0);
    save(dir,"camera-no-hand-status");ui_show(UI_HOME,0);
    puts("PASS removed detection/status overlays: clean home/preview pixels and empty top layer");
}
int main(int argc,char **argv)
{
    const char *dir=argc>1?argv[1]:"/tmp";char t[96];
    settings_check();
    assert(ui_time_text(0,t,sizeof(t))==0);
    assert(ui_time_text(1704124800,t,sizeof(t))==1);
    assert(!strcmp(t,"2024年01月02日 00:00:00"));
    assert(ui_init()==0);ui_update(1,1794312000,1);step();save(dir,"home");
    assert((ui_pixels()[300*480+240]>>24)==0);
    assert((ui_pixels()[10*480+240]>>24)>0&&(ui_pixels()[10*480+240]>>24)<255);
    tap(390,790);assert(ui_current_page()==UI_MENU);ui_update(1,1794312000,1);step();save(dir,"menu");
    for(int i=0;i<8;i++) {
        tap(110+(i%2)*224,230+(i/2)*158);assert(ui_current_page()==(i==7?UI_ADVANCED:i==0?UI_USERS:UI_FEATURE));
        if(!i)save(dir,"feature");
        if(i==7) {
            save(dir,"advanced");tap(240,230);assert(ui_current_page()==UI_FACTORY);
            ui_set_source("RGB");ui_factory_lights(0,0);step();save(dir,"factory");
            for(int k=1;k<14;k++) {
                tap(125+(k%2)*224,266+(k/2)*85);
                enum ui_action a=ui_take_action();
                if(k<=3) assert(a==(enum ui_action)(UI_ACTION_RGB+k-1));
                else if(k==4) assert(a==UI_ACTION_RELAY);
                else if(k==7) assert(a==UI_ACTION_WHITE_LIGHT);
                else if(k==8) assert(a==UI_ACTION_IR_LIGHT);
                else if(k==9) assert(a==UI_ACTION_SPEAKER);
                else if(k==13) assert(a==UI_ACTION_FACE_IR_CALIBRATE);
                else assert(a==UI_ACTION_NONE);
            }
            ui_show(UI_FACE_IR_CALIBRATE,0);
            FaceIrCalibrationStatus calibration={.state=FACE_IR_COLLECTING,.count=37};
            snprintf(calibration.message,sizeof(calibration.message),"正在采集，请保持常用距离并缓慢小幅移动");
            ui_face_ir_calibration_status(&calibration);step();save(dir,"face-ir-calibration");
            tap(42,35);assert(ui_current_page()==UI_FACTORY);
            lights_check(dir);rtsp_check(dir);
            tap(120,266);assert(ui_current_page()==UI_SCREEN_TEST);
            const unsigned colors[]={0xffff0000,0xff00ff00,0xff0000ff,0xffffffff,0xff000000,0xff808080};
            for(unsigned c=0;c<6;c++) {
                ui_update(1,1794312000,1);ui_render();
                for(int p=0;p<UI_WIDTH*UI_HEIGHT;p++) assert(ui_pixels()[p]==colors[c]);
                tap(240,400);
            }
            assert(ui_current_page()==UI_FACTORY);tap(50,100);assert(ui_current_page()==UI_ADVANCED);
            tap(50,100);
        } else if(i==0)tap(52,100);else tap(240,522);
        assert(ui_current_page()==UI_MENU);
    }
    tap(52,100);assert(ui_current_page()==UI_HOME);
    tap(90,790);assert(ui_current_page()==UI_INPUT);
    tap(90,380);tap(240,380);tap(390,380);assert(!strcmp(ui_input_text(),"123"));
    tap(390,738);assert(!strcmp(ui_input_text(),"12"));save(dir,"keyboard");
    tap(90,738);assert(!strcmp(ui_input_text(),""));
    for(int i=0;i<100;i++) {ui_show(UI_MENU,0);ui_show(UI_FEATURE,i%8);ui_show(UI_HOME,0);}
    ui_update(0,1794312000,0);step();save(dir,"disconnected");
    clean_preview_check(dir);business_check(dir);
    ui_destroy();puts("PASS: alpha, UTC+8 midnight, 8 menu routes, real pointer callbacks, digits/delete/clear, 100 page cycles");return 0;
}
