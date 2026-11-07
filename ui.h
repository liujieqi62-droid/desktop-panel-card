#ifndef PANEL_UI_H
#define PANEL_UI_H
#include "lvgl.h"
#include "cJSON.h"
#include "glass_theme.h"
#define BG 0xF3F4F7
#define CARD 0xFFFFFF
#define INK 0x20232B
#define MUTED 0x616775
#define CYAN 0x3871E0
#define SOFT 0xEDF3FF
#define STROKE 0xDEE2EA
#define SUCCESS 0x197A56
#define AMBER 0x946000
#define ERROR 0xB63D48
/* TRAE-G1 玻璃主题文字色（liquid-glass-v1 示例） */
#define NAVY 0x082B56
#define BLUE 0x307CF5
#define SUB 0x385A7B
extern lv_font_t *font_small,*font_body,*font_title;
extern lv_font_t *font_metric,*font_caption;
extern const lv_font_t panel_font_zh16;
typedef struct {
    lv_obj_t *box,*marker,*title,*main,*aux,*side;
    /* TRAE-G1：图标×2 + 进度条×2 + 自绘天气图标（全部 scene_create 一次创建） */
    lv_obj_t *icon,*icon2,*meter,*meter2,*wicon;
} ui_slot_t;
typedef struct {
    lv_obj_t *root;
    ui_slot_t slots[4];
} ui_scene_t;
const char *str(cJSON *s,const char *key,const char *fallback);
int num(cJSON *s,const char *key,int fallback);
lv_obj_t *ui_box(lv_obj_t *parent,int x,int y,int w,int h,unsigned color,int radius);
lv_obj_t *ui_text(lv_obj_t *p,int x,int y,int w,const char *text,const lv_font_t *font,unsigned color);
void ui_metric(lv_obj_t *p,int x,int y,int w,const char *title,const char *value,int percent);
void ui_scene_reset(ui_scene_t *scene);
void ui_box_set(lv_obj_t *obj,int x,int y,int w,int h,unsigned color,int radius);
void ui_label_set(lv_obj_t *obj,int x,int y,int w,const char *text,const lv_font_t *font,unsigned color,lv_text_align_t align);
void ui_dow_label(const char *ymd,char *out,size_t n); /* N10: "YYYY-MM-DD"→"MM.DD 周X"（子栏共用） */
void ui_foot_set(const char *symbol,const char *text); /* TRAE-G1：底栏图标+文案，各页可覆盖 */
int ui_init(const char *font_path);
void ui_render(cJSON *s,int stale);
void page_demo(ui_scene_t *scene,cJSON *s);
void page_home(ui_scene_t *scene,cJSON *s);
void page_weather(ui_scene_t *scene,cJSON *s);
void page_system(ui_scene_t *scene,cJSON *s);
void page_devices(ui_scene_t *scene,cJSON *s);
void page_words(ui_scene_t *scene,cJSON *s);   /* M5-A: words=4 每日单词页 */
void page_briefing(ui_scene_t *scene,cJSON *s);/* M5-B: briefing=5 每日简报页 */
void page_timetable(ui_scene_t *scene,cJSON *s);/* N6: timetable=6 课程表页 */
void page_plans(ui_scene_t *scene,cJSON *s);   /* N6: plans=7 今日计划页 */
#endif
