/* Live water material: sharp transmission, convex edge refraction, moving
 * highlights and shadow. Each card's first child is its rendered texture. */
#ifndef GLASS_THEME_H
#define GLASS_THEME_H
#include "lvgl.h"
#include "cJSON.h"

/* TRAE-G3: 壁纸由快照 wallpaper_id+wallpaper_revision 驱动。
 * refresh 每次渲染调用：同 id+revision → no-op（重复激活不重载不分配）；
 * 变化 → 载入（自选 clear 直读 / default 直读）；失败逐级回退
 * 自选→default→纯色，UI 恒可读。 */
void glass_wallpaper_refresh(cJSON *s);
void glass_wallpaper_apply(lv_obj_t *screen,cJSON *s); /* 创建底层图像对象并载入 */
void glass_tick(uint32_t now); /* Continuous water optics, independently of data updates. */
lv_obj_t *ui_glass_card(lv_obj_t *parent,int x,int y,int w,int h); /* 新建实底卡 */
void ui_glass_build(lv_obj_t *card);          /* 向已有容器注入实底卡样式 */
void ui_glass_set(lv_obj_t *card,int x,int y,int w,int h);
lv_obj_t *ui_icon(lv_obj_t *parent,int x,int y,const char *symbol,const lv_font_t *font,unsigned color);
void ui_icon_set(lv_obj_t *icon,int x,int y,const char *symbol,const lv_font_t *font,unsigned color);
lv_obj_t *ui_meter(lv_obj_t *parent,int x,int y,int w);    /* 6px 圆角3 进度条 */
void ui_meter_set(lv_obj_t *bar,int x,int y,int w,int percent);
lv_obj_t *ui_weather_icon(lv_obj_t *parent);               /* Two reusable native image objects. */
void ui_weather_icon_set(lv_obj_t *g,int x,int y,int type);/* 0 sun,1 partly cloudy,2 rain,3 snow,4 overcast,5 storm,6 fog,7 unknown */
int glass_weather_type(const char *condition);
#endif
