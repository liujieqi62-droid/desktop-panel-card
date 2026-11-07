/* Live water-bubble material: sharp transmitted wallpaper, convex edge
 * refraction and moving specular highlights. No background blur is used. */
#include "ui.h"
#include "glass_theme.h"
#include "panel_icons.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#define GW 480
#define GH 272
#ifdef DESKTOP
#define WP_THEMES_ROOT "build/g3-themes"
#else
#define WP_THEMES_ROOT "/mnt/UDISK/panel-v3/themes"
#endif
static unsigned char wallpaper_raw[GW*GH*4];
static lv_image_dsc_t wallpaper_img;
static int wallpaper_ok;
static char loaded_id[48]={'D'};
static long loaded_rev=-1;
static lv_obj_t *bg_img;
static unsigned material_revision=1;
typedef struct {
    uint32_t source;
    int8_t nx,ny;
    uint8_t edge,u,rim;
} bubble_sample_t;
typedef struct {
    lv_obj_t *card,*surface;
    lv_image_dsc_t image;
    unsigned char *pixels;
    bubble_sample_t *samples;
    size_t capacity;
    int x,y,w,h;
    unsigned revision;
} glass_material_t;
static glass_material_t materials[6];
static int material_count;
static uint32_t last_bubble_frame;
static int bound(int value,int high){return value<0?0:value>high?high:value;}
static glass_material_t *material_for(lv_obj_t *card){
    for(int i=0;i<material_count;i++)if(materials[i].card==card)return &materials[i];
    return NULL;
}
/* Geometry is cached; lighting and refracted pixels are rendered continuously.
 * Rounded-rectangle surface normals create a thick, convex water meniscus. */
static void bubble_paint(glass_material_t *m,uint32_t now){
    float phase=(now%12000u)*0.0005235988f;
    int light_x=(int)(sinf(phase)*85)-35;
    int light_y=-105;
    int glint_x=90+(int)(sinf(phase*0.75f)*100);
    int drift=(int)(sinf(phase*1.5f)*2.0f);
    int count=m->w*m->h;
    for(int i=0;i<count;i++){
        bubble_sample_t *q=&m->samples[i];
        int edge=q->edge,fresnel=edge*edge/255;
        int facing=bound((q->nx*light_x+q->ny*light_y)/127,127);
        int specular=facing*facing/127;
        specular=specular*specular/127;
        int glint=bound(90-abs((int)q->u-glint_x),90);
        int white=bound(fresnel*(38+specular+glint)/255+
                        q->rim*(64+specular/3)/255,230);
        int shade=(edge-fresnel)*32/255;
        int sx=(int)(q->source%GW),sy=(int)(q->source/GW);
        sx=bound(sx+(q->nx*drift*edge)/(127*255),GW-1);
        sy=bound(sy+(q->ny*drift*edge)/(127*255),GH-1);
        int src=(sy*GW+sx)*4,dst=i*4;
        for(int c=0;c<3;c++){
            /* Clear water, not frosted glass: retain sharp wallpaper detail. */
            int value=wallpaper_raw[src+c];
            int tint=c==0?255:c==1?250:244;
            value=(value*199+tint*56)/255;
            value=value*(255-shade)/255;
            value=(value*(255-white)+255*white)/255;
            m->pixels[dst+c]=(unsigned char)bound(value,255);
        }
        m->pixels[dst+3]=255;
    }
    lv_obj_invalidate(m->surface);
}
static void material_update(glass_material_t *m,int x,int y,int w,int h){
    if(!m||w<1||h<1)return;
    if(m->revision==material_revision&&m->x==x&&m->y==y&&m->w==w&&m->h==h)return;
    size_t count=(size_t)w*h;
    if(count>m->capacity){
        unsigned char *pixels=malloc(count*4);
        bubble_sample_t *samples=malloc(count*sizeof(*samples));
        if(!pixels||!samples){free(pixels);free(samples);lv_obj_add_flag(m->surface,LV_OBJ_FLAG_HIDDEN);return;}
        free(m->pixels);free(m->samples);
        m->pixels=pixels;m->samples=samples;m->capacity=count;
    }
    float radius=fminf(22.0f,h/2.0f),cx=w/2.0f,cy=h/2.0f;
    for(int py=0;py<h;py++)for(int px=0;px<w;px++){
        float dx=fabsf(px+0.5f-cx),dy=fabsf(py+0.5f-cy);
        float qx=dx-(cx-radius),qy=dy-(cy-radius);
        float ax=fmaxf(qx,0),ay=fmaxf(qy,0),len=sqrtf(ax*ax+ay*ay);
        float distance=radius-len-fminf(fmaxf(qx,qy),0);
        float meniscus=fminf(12.0f,h*0.22f);
        float edge=fmaxf(0,1.0f-fmaxf(distance,0)/meniscus);
        float nx=0,ny=0;
        if(len>0){nx=ax/len;ny=ay/len;}
        else if(qx>qy)nx=1;else ny=1;
        if(px<cx)nx=-nx;if(py<cy)ny=-ny;
        float bend=sinf(edge*3.14159265f)*fminf(8.0f,h*0.15f);
        int sx=bound(x+(int)(cx+(px-cx)*0.985f-nx*bend),GW-1);
        int sy=bound(y+(int)(cy+(py-cy)*0.985f-ny*bend),GH-1);
        bubble_sample_t *q=&m->samples[py*w+px];
        q->source=sy*GW+sx;q->nx=(int8_t)(nx*127);q->ny=(int8_t)(ny*127);
        q->edge=(uint8_t)(edge*255);q->u=(uint8_t)(bound(x+px,GW-1)*255/(GW-1));
        q->rim=(uint8_t)(fmaxf(0,1.0f-fabsf(distance-2.5f)/1.8f)*255);
    }
    m->x=x;m->y=y;m->w=w;m->h=h;m->revision=material_revision;
    m->image.header.magic=LV_IMAGE_HEADER_MAGIC;m->image.header.cf=LV_COLOR_FORMAT_XRGB8888;
    m->image.header.w=w;m->image.header.h=h;m->image.header.stride=w*4;
    m->image.data_size=count*4;m->image.data=m->pixels;
    bubble_paint(m,lv_tick_get());
    lv_image_set_src(m->surface,&m->image);lv_obj_set_pos(m->surface,-1,-1);
    lv_obj_remove_flag(m->surface,LV_OBJ_FLAG_HIDDEN);
}
void glass_tick(uint32_t now){
    if((uint32_t)(now-last_bubble_frame)<83u)return;
    last_bubble_frame=now;
    for(int i=0;i<material_count;i++){
        glass_material_t *m=&materials[i];
        if(m->pixels&&lv_obj_is_visible(m->card))bubble_paint(m,now);
    }
}

static lv_obj_t *glass_box(lv_obj_t *p,int x,int y,int w,int h){
    lv_obj_t *o=lv_obj_create(p);lv_obj_remove_style_all(o);
    lv_obj_set_pos(o,x,y);lv_obj_set_size(o,w,h);
    lv_obj_remove_flag(o,LV_OBJ_FLAG_SCROLLABLE|LV_OBJ_FLAG_CLICKABLE);
    return o;
}
/* 默认壁纸：wallpaper.bgra 直读（N10 起不再预计算模糊帧） */
static int wp_load_default(void){
    const char *path=
#ifdef DESKTOP
        "docs/design/liquid-glass-v1/assets/wallpaper.bgra";
#else
        WP_THEMES_ROOT "/wallpaper.bgra";
#endif
    FILE *f=fopen(path,"rb");
    if(!f)return -1;
    size_t n=fread(wallpaper_raw,1,sizeof(wallpaper_raw),f);
    int extra=fgetc(f);fclose(f);
    if(n!=sizeof(wallpaper_raw)||extra!=EOF)return -1;
    return 0;
}
/* 自选壁纸：手机预生成 clear 帧直读（frost 帧自 N10 起不再使用） */
static int wp_load_custom(const char *id){
    char p[160];FILE *f;size_t n;int extra;
    snprintf(p,sizeof p,WP_THEMES_ROOT "/wallpapers/%s.clear",id);
    f=fopen(p,"rb");if(!f)return -1;
    n=fread(wallpaper_raw,1,sizeof(wallpaper_raw),f);
    extra=fgetc(f);fclose(f);
    if(n!=sizeof(wallpaper_raw)||extra!=EOF)return -1;
    return 0;
}
static void wp_img_dsc(void){
    wallpaper_img.header.magic=LV_IMAGE_HEADER_MAGIC;
    wallpaper_img.header.cf=LV_COLOR_FORMAT_XRGB8888;
    wallpaper_img.header.w=GW;wallpaper_img.header.h=GH;wallpaper_img.header.stride=GW*4;
    wallpaper_img.data_size=sizeof(wallpaper_raw);wallpaper_img.data=wallpaper_raw;
}
void glass_wallpaper_refresh(cJSON *s){
    if(!bg_img)return;
    const char *id="default";long rev=0;
    if(s){
        cJSON *v=cJSON_GetObjectItemCaseSensitive(s,"wallpaper_id");
        if(cJSON_IsString(v)&&v->valuestring[0]&&strlen(v->valuestring)<sizeof(loaded_id))id=v->valuestring;
        v=cJSON_GetObjectItemCaseSensitive(s,"wallpaper_revision");
        if(cJSON_IsNumber(v))rev=(long)v->valuedouble;
    }
    if(rev==loaded_rev&&!strcmp(id,loaded_id))return;   /* 同 id+revision：no-op */
    int ok;
    if(!strcmp(id,"default"))ok=wp_load_default();
    else{
        ok=wp_load_custom(id);
        if(ok!=0)ok=wp_load_default();                  /* 缺失/损坏→内置默认 */
    }
    /* 记录"尝试过"的 id+revision：失败也不在每次渲染重读文件 */
    snprintf(loaded_id,sizeof(loaded_id),"%s",id);
    loaded_rev=rev;
    wallpaper_ok=(ok==0);
    if(!wallpaper_ok){
        for(int i=0;i<GW*GH;i++){
            wallpaper_raw[i*4]=0xF7;wallpaper_raw[i*4+1]=0xF4;
            wallpaper_raw[i*4+2]=0xF3;wallpaper_raw[i*4+3]=255;
        }
    }
    material_revision++;
    if(wallpaper_ok){
        wp_img_dsc();
        lv_image_set_src(bg_img,&wallpaper_img);
        lv_obj_remove_flag(bg_img,LV_OBJ_FLAG_HIDDEN);
        lv_obj_invalidate(bg_img);
    }else lv_obj_add_flag(bg_img,LV_OBJ_FLAG_HIDDEN);
    for(int i=0;i<material_count;i++){
        glass_material_t *m=&materials[i];
        material_update(m,m->x,m->y,m->w,m->h);
    }
}
void glass_wallpaper_apply(lv_obj_t *screen,cJSON *s){
    bg_img=lv_image_create(screen);
    lv_obj_set_pos(bg_img,0,0);
    lv_obj_move_background(bg_img);
    glass_wallpaper_refresh(s);   /* 成功贴壁纸；失败保持 BG 纯色 */
}
/* The cached material is the first child, behind every live page element. */
void ui_glass_build(lv_obj_t *card){
    lv_obj_set_style_radius(card,22,0);
    lv_obj_set_style_clip_corner(card,true,0);
    lv_obj_set_style_bg_color(card,lv_color_hex(CARD),0);
    lv_obj_set_style_bg_opa(card,36,0);
    lv_obj_set_style_border_width(card,1,0);
    lv_obj_set_style_border_color(card,lv_color_hex(0xFFFFFF),0);
    lv_obj_set_style_border_opa(card,200,0);
    lv_obj_set_style_border_post(card,true,0);
    lv_obj_set_style_shadow_color(card,lv_color_hex(0x426B98),0);
    lv_obj_set_style_shadow_width(card,12,0);
    lv_obj_set_style_shadow_opa(card,45,0);
    lv_obj_set_style_shadow_ofs_y(card,3,0);
    if(material_count<6){
        glass_material_t *m=&materials[material_count++];
        m->card=card;m->surface=lv_image_create(card);
        lv_obj_remove_flag(m->surface,LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_SCROLLABLE);
    }
}
/* Page changes refresh only cards whose rectangle changed. */
void ui_glass_set(lv_obj_t *card,int x,int y,int w,int h){
    lv_obj_remove_flag(card,LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(card,x,y);lv_obj_set_size(card,w,h);
    lv_obj_set_style_radius(card,h<44?h/2:22,0);
    material_update(material_for(card),x,y,w,h);
}
lv_obj_t *ui_glass_card(lv_obj_t *parent,int x,int y,int w,int h){
    lv_obj_t *card=glass_box(parent,x,y,w,h);
    ui_glass_build(card);
    ui_glass_set(card,x,y,w,h);
    return card;
}
lv_obj_t *ui_icon(lv_obj_t *parent,int x,int y,const char *symbol,const lv_font_t *font,unsigned color){
    lv_obj_t *l=lv_image_create(parent);
    lv_obj_remove_flag(l,LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_SCROLLABLE);
    ui_icon_set(l,x,y,symbol,font,color);
    return l;
}
static const char *icon_name(const char *symbol){
    static const struct {const char *symbol,*name;} aliases[]={
        {LV_SYMBOL_WIFI,"wifi"},{LV_SYMBOL_BELL,"bell"},{LV_SYMBOL_RIGHT,"chevron-right"},
        {LV_SYMBOL_BARS,"chart-bar"},{LV_SYMBOL_LIST,"list-details"},
        {LV_SYMBOL_VOLUME_MAX,"volume"},{LV_SYMBOL_GPS,"map-pin"},
        {LV_SYMBOL_HOME,"home"},{LV_SYMBOL_REFRESH,"clock"},
        {LV_SYMBOL_SETTINGS,"cpu"},{LV_SYMBOL_DRIVE,"database"},
        {LV_SYMBOL_TINT,"droplet"},{LV_SYMBOL_CALL,"device-mobile"}
    };
    if(!symbol)return "info-circle";
    for(unsigned i=0;i<sizeof(aliases)/sizeof(aliases[0]);i++)
        if(!strcmp(symbol,aliases[i].symbol))return aliases[i].name;
    return symbol;
}
void ui_icon_set(lv_obj_t *icon,int x,int y,const char *symbol,const lv_font_t *font,unsigned color){
    lv_obj_remove_flag(icon,LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(icon,x,y);
    int size=font==&lv_font_montserrat_14?16:font==&lv_font_montserrat_32?32:20;
    const lv_image_dsc_t *image=panel_icon_find(icon_name(symbol),size);
    if(!image)image=panel_icon_find("info-circle",size);
    lv_image_set_src(icon,image);
    lv_obj_set_style_image_recolor(icon,lv_color_hex(color),0);
    lv_obj_set_style_image_recolor_opa(icon,LV_OPA_COVER,0);
}
lv_obj_t *ui_meter(lv_obj_t *parent,int x,int y,int w){
    lv_obj_t *o=lv_bar_create(parent);lv_obj_remove_style_all(o);
    lv_obj_set_pos(o,x,y);lv_obj_set_size(o,w,6);
    lv_obj_set_style_radius(o,3,LV_PART_MAIN);
    lv_obj_set_style_bg_color(o,lv_color_hex(0xE6EDF8),LV_PART_MAIN);
    lv_obj_set_style_bg_opa(o,LV_OPA_COVER,LV_PART_MAIN);
    lv_obj_set_style_radius(o,3,LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(o,lv_color_hex(BLUE),LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(o,LV_OPA_COVER,LV_PART_INDICATOR);
    lv_bar_set_value(o,0,LV_ANIM_OFF);
    return o;
}
void ui_meter_set(lv_obj_t *bar,int x,int y,int w,int percent){
    lv_obj_remove_flag(bar,LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(bar,x,y);lv_obj_set_size(bar,w,6);
    if(percent<0)percent=0;if(percent>100)percent=100;
    lv_bar_set_value(bar,percent,LV_ANIM_OFF);
}
/* Native antialiased weather icons, independent of the text font. */
lv_obj_t *ui_weather_icon(lv_obj_t *parent){
    lv_obj_t *g=glass_box(parent,0,0,44,40);
    lv_obj_t *sun=lv_image_create(g);
    lv_image_set_src(sun,panel_icon_find("sun",20));
    lv_obj_set_style_image_recolor(sun,lv_color_hex(0xD89116),0);
    lv_obj_set_style_image_recolor_opa(sun,LV_OPA_COVER,0);
    lv_obj_t *main=lv_image_create(g);
    lv_obj_set_style_image_recolor_opa(main,LV_OPA_COVER,0);
    ui_weather_icon_set(g,0,0,7);
    return g;
}
void ui_weather_icon_set(lv_obj_t *g,int x,int y,int type){
    static const char *names[]={"sun","cloud","cloud-rain","cloud-snow",
                                "cloud","cloud-storm","cloud-fog","help-circle"};
    if(type<0||type>7)type=7;
    lv_obj_remove_flag(g,LV_OBJ_FLAG_HIDDEN);lv_obj_set_pos(g,x,y);
    lv_obj_t *sun=lv_obj_get_child(g,0),*main=lv_obj_get_child(g,1);
    if(type==1){lv_obj_remove_flag(sun,LV_OBJ_FLAG_HIDDEN);lv_obj_set_pos(sun,24,0);}
    else lv_obj_add_flag(sun,LV_OBJ_FLAG_HIDDEN);
    lv_image_set_src(main,panel_icon_find(names[type],36));
    lv_obj_set_pos(main,type==1?0:4,type==1?4:2);
    unsigned color=type==0?0xD89116:type==4||type==6?0x52769E:0x3076CA;
    lv_obj_set_style_image_recolor(main,lv_color_hex(color),0);
}
int glass_weather_type(const char *condition){
    if(!condition||!*condition)return 7;
    if(strstr(condition,"雷"))return 5;
    if(strstr(condition,"雪"))return 3;
    if(strstr(condition,"雨")||strstr(condition,"雹")||strstr(condition,"冻"))return 2;
    if(strstr(condition,"雾")||strstr(condition,"霾")||strstr(condition,"沙"))return 6;
    if(strstr(condition,"阴"))return 4;
    if(strstr(condition,"云"))return 1;
    if(strstr(condition,"晴"))return 0;
    return 7;
}
