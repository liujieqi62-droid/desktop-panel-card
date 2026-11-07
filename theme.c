#include "ui.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

lv_font_t *font_small,*font_body,*font_title,*font_metric;
lv_font_t *font_caption;

const char *str(cJSON *s,const char *key,const char *fallback){
    const char *v=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(s,key));return v?v:fallback;
}
int num(cJSON *s,const char *key,int fallback){
    cJSON *v=cJSON_GetObjectItemCaseSensitive(s,key);return cJSON_IsNumber(v)?v->valueint:fallback;
}
lv_obj_t *ui_box(lv_obj_t *parent,int x,int y,int w,int h,unsigned color,int radius){
    lv_obj_t *o=lv_obj_create(parent);lv_obj_remove_style_all(o);
    lv_obj_set_pos(o,x,y);lv_obj_set_size(o,w,h);
    lv_obj_set_style_bg_color(o,lv_color_hex(color),0);lv_obj_set_style_bg_opa(o,LV_OPA_COVER,0);
    lv_obj_set_style_radius(o,radius,0);
    lv_obj_set_style_border_width(o,radius>0?1:0,0);lv_obj_set_style_border_color(o,lv_color_hex(STROKE),0);
    lv_obj_remove_flag(o,LV_OBJ_FLAG_SCROLLABLE|LV_OBJ_FLAG_CLICKABLE);return o;
}
/* N10: "YYYY-MM-DD" -> "MM.DD 周X"（timetable/plans 子栏共用；非法输入原样回退） */
static const char *dow_names[]={"日","一","二","三","四","五","六"};
#if defined(_WIN32)
static void md_local(const time_t *t,struct tm *out){
    struct tm *lt=localtime(t);
    if(lt)*out=*lt;else memset(out,0,sizeof *out);
}
#else
static void md_local(const time_t *t,struct tm *out){localtime_r(t,out);}
#endif
void ui_dow_label(const char *ymd,char *out,size_t n){
    int y=0,m=0,d=0;
    if(ymd&&(sscanf(ymd,"%d-%d-%d",&y,&m,&d)==3||
             sscanf(ymd,"%d / %d / %d",&y,&m,&d)==3)&&y>=1900){
        struct tm t;memset(&t,0,sizeof t);
        t.tm_year=y-1900;t.tm_mon=m-1;t.tm_mday=d;
        time_t tt=mktime(&t);struct tm lt;
        md_local(&tt,&lt);
        if(lt.tm_wday>=0&&lt.tm_wday<=6){
            snprintf(out,n,"%02d.%02d 周%s",lt.tm_mon+1,lt.tm_mday,dow_names[lt.tm_wday]);
            return;
        }
    }
    snprintf(out,n,"%s",ymd?ymd:"");
}
lv_obj_t *ui_text(lv_obj_t *p,int x,int y,int w,const char *text,const lv_font_t *font,unsigned color){
    lv_obj_t *l=lv_label_create(p);lv_obj_set_pos(l,x,y);lv_obj_set_width(l,w);
    lv_obj_set_style_text_font(l,font,0);lv_obj_set_style_text_color(l,lv_color_hex(color),0);
    lv_label_set_long_mode(l,LV_LABEL_LONG_DOT);lv_label_set_text(l,text);return l;
}
void ui_metric(lv_obj_t *p,int x,int y,int w,const char *title,const char *value,int percent){(void)p;(void)x;(void)y;(void)w;(void)title;(void)value;(void)percent;}
void ui_box_set(lv_obj_t *obj,int x,int y,int w,int h,unsigned color,int radius){
    lv_obj_remove_flag(obj,LV_OBJ_FLAG_HIDDEN);lv_obj_set_pos(obj,x,y);lv_obj_set_size(obj,w,h);
    lv_obj_set_style_bg_color(obj,lv_color_hex(color),0);lv_obj_set_style_bg_opa(obj,LV_OPA_COVER,0);lv_obj_set_style_radius(obj,radius,0);
    lv_obj_set_style_border_width(obj,radius>0?1:0,0);lv_obj_set_style_border_color(obj,lv_color_hex(STROKE),0);
}
void ui_label_set(lv_obj_t *obj,int x,int y,int w,const char *text,const lv_font_t *font,unsigned color,lv_text_align_t align){
    lv_obj_remove_flag(obj,LV_OBJ_FLAG_HIDDEN);lv_obj_set_pos(obj,x,y);lv_obj_set_width(obj,w);
    lv_obj_set_height(obj,lv_font_get_line_height(font)*(text&&strchr(text,'\n')?2:1));
    lv_obj_set_style_text_font(obj,font,0);lv_obj_set_style_text_color(obj,lv_color_hex(color),0);
    lv_obj_set_style_text_align(obj,align,0);
    /* M5-B: force LONG_DOT everywhere. The page contract already promises
     * "LONG_DOT/固定行高"; WRAP multi-line rendering re-triggers the
     * lv_tiny_ttf heap corruption seen in M5-A (two wrapped CJK labels on one
     * screen crash 0xC0000374), and DOT keeps every label single-line. */
    lv_label_set_long_mode(obj,LV_LABEL_LONG_DOT);
    lv_label_set_text(obj,text);
}
/* TRAE-G1: 底栏玻璃条固定对象（ui_render 创建一次），各页通过本接口设置图标+文案 */
void ui_foot_set(const char *symbol,const char *text){
    extern lv_obj_t *g_foot_icon,*g_foot_text;
    ui_icon_set(g_foot_icon,12,6,symbol?symbol:LV_SYMBOL_BELL,&lv_font_montserrat_14,BLUE);
    ui_label_set(g_foot_text,36,6,418,text?text:"",font_caption,NAVY,LV_TEXT_ALIGN_LEFT);
}
void ui_scene_reset(ui_scene_t *scene){
    for(int i=0;i<4;i++){
        ui_slot_t *s=&scene->slots[i];
        /* box 隐藏即隐藏其全部玻璃子件/文本/图标；逐项隐藏保持防御性 */
        lv_obj_add_flag(s->box,LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s->marker,LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s->title,LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s->main,LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s->aux,LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s->side,LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s->icon,LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s->icon2,LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s->meter,LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s->meter2,LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s->wicon,LV_OBJ_FLAG_HIDDEN);
    }
}
static void scene_create(lv_obj_t *parent,ui_scene_t *scene){
    memset(scene,0,sizeof(*scene));scene->root=parent;
    for(int i=0;i<4;i++){
        ui_slot_t *slot=&scene->slots[i];
        /* 对象数恒定：玻璃子件(4)+marker+4 文本+2 图标+2 进度条+天气图标组，全部一次创建 */
        slot->box=ui_glass_card(parent,0,0,10,10);
        slot->marker=ui_box(slot->box,0,0,2,2,CYAN,1);
        slot->title=ui_text(slot->box,0,0,1,"",font_small,SUB);
        slot->main=ui_text(slot->box,0,0,1,"",font_body,NAVY);
        slot->aux=ui_text(slot->box,0,0,1,"",font_small,SUB);
        slot->side=ui_text(slot->box,0,0,1,"",font_small,SUB);
        slot->icon=ui_icon(slot->box,0,0,LV_SYMBOL_BELL,&lv_font_montserrat_20,BLUE);
        slot->icon2=ui_icon(slot->box,0,0,LV_SYMBOL_BELL,&lv_font_montserrat_20,BLUE);
        slot->meter=ui_meter(slot->box,0,0,10);
        slot->meter2=ui_meter(slot->box,0,0,10);
        slot->wicon=ui_weather_icon(slot->box);
    }
    ui_scene_reset(scene);
}
int ui_init(const char *font_path){
    fprintf(stderr,"loading font\n");
    font_small=(lv_font_t *)&panel_font_zh16;
    font_body=lv_tiny_ttf_create_file_ex(font_path,20,LV_FONT_KERNING_NONE,64);
    font_title=lv_tiny_ttf_create_file_ex(font_path,20,LV_FONT_KERNING_NONE,64);
    font_metric=lv_tiny_ttf_create_file_ex(font_path,36,LV_FONT_KERNING_NONE,64);
    font_caption=lv_tiny_ttf_create_file_ex(font_path,12,LV_FONT_KERNING_NONE,64);
    if(!font_caption)font_caption=font_small;
    fprintf(stderr,"fonts ready\n");return font_small&&font_body&&font_title&&font_metric?0:-1;
}
/* Booleans read as JSON booleans, but a number is accepted too so an older or
 * hand-written snapshot cannot silently hide a page. */
static int flag(cJSON *s,const char *key,int fallback){
    cJSON *v=cJSON_GetObjectItemCaseSensitive(s,key);
    if(cJSON_IsBool(v))return cJSON_IsTrue(v)?1:0;
    if(cJSON_IsNumber(v))return v->valueint?1:0;
    return fallback;
}
lv_obj_t *g_foot_icon,*g_foot_text;
void ui_render(cJSON *s,int stale){
    static int built;static ui_scene_t scene;
    static lv_obj_t *dots[8],*heading,*connection,*wifi,*foot_mode,*foot_chev,*content,*title_bar,*foot;
    glass_wallpaper_refresh(s); /* TRAE-G3: id+revision 变化才重载壁纸（先于建屏） */
    int page=num(s,"display_page",0);if(page<0||page>7)page=0;
    /* M1: one capability switch hides the smart-home page. The screen refuses
     * to render it even if a snapshot still names it, so a display that starts
     * before the service has migrated a pinned page can never flash it.
     * M5-A: words=4 appends after devices; M5-B: briefing=5 appends after
     * words; the hidden-entry rule stays bound to id 3 only. */
    int smart_home=flag(s,"smart_home_visible",0);
    if(!smart_home&&page==3)page=0;
    lv_obj_t *screen=lv_screen_active();
    if(!built){
        lv_obj_set_style_bg_color(screen,lv_color_hex(BG),0);lv_obj_set_style_bg_opa(screen,LV_OPA_COVER,0);
        lv_obj_remove_flag(screen,LV_OBJ_FLAG_SCROLLABLE);
        glass_wallpaper_apply(screen,s); /* 成功贴壁纸；失败保持 BG 纯色 */
        /* 标题玻璃条：蓝竖条+标题 | 连接状态+WiFi 图标 */
        title_bar=ui_glass_card(screen,12,8,456,40);
        lv_obj_t *accent=ui_box(title_bar,10,10,4,20,BLUE,2);
        lv_obj_set_style_border_width(accent,0,0);
        heading=ui_text(title_bar,26,7,150,"",font_title,NAVY);
        connection=ui_text(title_bar,180,13,232,"",font_caption,SUB);
        lv_obj_set_style_text_align(connection,LV_TEXT_ALIGN_RIGHT,0);
        wifi=ui_icon(title_bar,424,10,LV_SYMBOL_WIFI,&lv_font_montserrat_20,BLUE);
        /* 场景层：与屏幕对齐的透明容器，页内玻璃卡用绝对坐标 */
        content=ui_box(screen,0,0,480,272,BG,0);
        lv_obj_set_style_bg_opa(content,LV_OPA_TRANSP,0);
        scene_create(content,&scene);
        /* 底栏玻璃条：图标+文案 | 模式 | 右箭头 */
        foot=ui_glass_card(screen,12,232,456,28);
        g_foot_icon=ui_icon(foot,10,3,LV_SYMBOL_BELL,&lv_font_montserrat_20,BLUE);
        g_foot_text=ui_text(foot,38,6,418,"",font_caption,NAVY);
        foot_mode=ui_text(foot,336,7,90,"",font_small,SUB);
        lv_obj_set_style_text_align(foot_mode,LV_TEXT_ALIGN_RIGHT,0);
        foot_chev=ui_icon(foot,436,7,LV_SYMBOL_RIGHT,&lv_font_montserrat_14,SUB);
        /* The board is display-only; leave navigation actions to the phone. */
        lv_obj_add_flag(foot_chev,LV_OBJ_FLAG_HIDDEN);
        for(int i=0;i<8;i++)dots[i]=ui_box(screen,199+i*14,263,5,3,STROKE,1);
        built=1;
    }
    ui_scene_reset(&scene);
    ui_foot_set(LV_SYMBOL_BELL,"详情请在手机查看"); /* 默认底栏，页面可覆盖 */
    cJSON *demo=cJSON_GetObjectItemCaseSensitive(s,"demo");
    if(num(demo,"enabled",0))page_demo(&scene,demo);
    else if(page==0)page_home(&scene,s);else if(page==1)page_weather(&scene,s);else if(page==2)page_system(&scene,s);else if(page==4)page_words(&scene,s);else if(page==5)page_briefing(&scene,s);else if(page==6)page_timetable(&scene,s);else if(page==7)page_plans(&scene,s);else page_devices(&scene,s);
    const char *titles[]={"信息总览","三天天气","运行状态","设备状态","今日单词","每日简报","课程表","今日计划"};lv_label_set_text(heading,titles[page]);
    int online=flag(s,"online",strcmp(str(s,"ip","OFFLINE"),"OFFLINE")!=0);char status[80];
    /* 顶栏只放可扫读的连接状态；IP 仍在系统页展示，避免长地址挤压标题栏。 */
    snprintf(status,sizeof(status),"%s",stale?"服务等待":online?"已联网":"未联网");
    lv_label_set_text(connection,status);lv_obj_set_style_text_color(connection,lv_color_hex(stale||!online?AMBER:SUB),0);
    ui_icon_set(wifi,424,10,online&&!stale?"wifi":"wifi-off",&lv_font_montserrat_20,stale||!online?AMBER:BLUE);
    lv_label_set_text(foot_mode,strcmp(str(s,"mode","rotation"),"fixed")==0?"固定显示":"自动轮播");
    lv_obj_add_flag(foot_mode,LV_OBJ_FLAG_HIDDEN);
    int dot_count=smart_home?8:7;
    int active_dot=!smart_home&&page>3?page-1:page;
    for(int i=0;i<8;i++){
        if(i>=dot_count){lv_obj_add_flag(dots[i],LV_OBJ_FLAG_HIDDEN);continue;}
        lv_obj_remove_flag(dots[i],LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_x(dots[i],199+i*14);
        lv_obj_set_width(dots[i],i==active_dot?12:5);
        lv_obj_set_style_bg_color(dots[i],lv_color_hex(i==active_dot?CYAN:0xFFFFFF),0);
        lv_obj_set_style_bg_opa(dots[i],i==active_dot?LV_OPA_COVER:150,0);
    }
    if(num(demo,"enabled",0)){
        lv_label_set_text(heading,str(demo,"title","功能体验"));
        lv_label_set_text(connection,"DEMO · 示例体验");
        for(int i=0;i<8;i++){
            lv_obj_remove_flag(dots[i],LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_width(dots[i],i==num(demo,"page",0)?12:5);
            lv_obj_set_style_bg_color(dots[i],lv_color_hex(i==num(demo,"page",0)?CYAN:0xFFFFFF),0);
        }
    }
    (void)foot_chev;(void)content;
}
