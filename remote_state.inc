/* Versioned remote-display API and atomic snapshots. Included by service.c
 * after the existing business state and HTTP response helpers. */
#define REMOTE_CONFIG "/mnt/UDISK/panel-v3/display.json"
#define SNAPSHOT "/tmp/panel-v3-state.json"
/* M5-A: "words" appends as page id 4. M5-B: "briefing" appends as page id 5.
 * N6: "timetable" appends as page id 6, "plans" as page id 7 (续工方案 §4.2).
 * Existing ids keep their indexes; the old devices=3 entry is never reused
 * (实施方案 39 行). */
static const char *page_names[] = {"home", "weather", "system", "devices", "words", "briefing", "timetable", "plans"};
#define PAGE_COUNT 8
/* M5-A/M5-B: words/briefing snapshot producers. Defined in service.c after
 * g_v2 (this file is included earlier), read-only over the pv2 entities; the
 * display process only ever sees these few fields, never the library.
 * TRAE-F2: words_today_snapshot 也回填注释三字段（annotation 实体，id=word_id），
 * 缺失/损坏为空串，页面「—」占位。 */
/* TRAE-N2: 每日清单来自手机 daily_plan 实体（pv2_planlist_pick），快照同时
 * 回填真实进度 done/review/goal 与 stale（清单日期≠业务日=昨日缓存）。 */
static void words_today_snapshot(char *text, size_t tcap, int *total, int *index,
                                 char *datebuf, size_t dcap,
                                 char *phon, size_t phcap, char *mean, size_t mcap,
                                 char *ex, size_t xcap,
                                 int *done, int *review, int *goal, int *stale);
static int briefing_snapshot(cJSON *br);   /* fills date/pages/part/items[≤3] */
/* N6: timetable/plans snapshot producers (fills the whole object; 0 = absent so
 * the page renders its waiting-for-sync placeholder). Defined in service.c. */
static int timetable_snapshot(cJSON *tt);
static int plans_snapshot(cJSON *pl);
static int64_t last_snapshot_ms;

/* M1 - unified capability switch for the smart-home entry (page id "devices").
 *
 * Everything that can offer, select or rotate to that entry reads this one
 * flag: the state/capabilities API, the App navigation, the web navigation and
 * the on-screen page indicator. While it is off the entry is hidden, nothing
 * more: the page id, its code, its configuration keys and its data all stay in
 * place, so switching the flag back on (advanced settings) restores the entry
 * without a migration. Default is off, per the execution plan. */
#define SMART_HOME_BACKUP "/mnt/UDISK/panel-v3/display.json.pre-m1-migration"
#define MIGRATION_LOG "/mnt/UDISK/panel-v3/migration.log"
static int smart_home_visible;
static int smart_home_page_migrated;
static int handle_remote(int, const char *, const char *, const char *, home_state_t *);
static int read_request(int, char *, size_t, const char **, long *);
static void demo_snapshot(cJSON *o);
static int demo_stop(void);

static cJSON *state_json(const home_state_t *s) {
    cJSON *o = cJSON_CreateObject();
    if (!o) return NULL;
#define S(k,v) cJSON_AddStringToObject(o,k,v)
#define N(k,v) cJSON_AddNumberToObject(o,k,(double)(v))
#define B(k,v) cJSON_AddBoolToObject(o,k,(v))
    S("version",APP_VERSION); S("ip",s->ip); B("online",strcmp(s->ip,"OFFLINE")!=0);
    S("page",page_names[display_page]); N("display_page",display_page);
    S("view",page_names[display_page]); S("mode",rotation_enabled?"rotation":"fixed");
    B("rotation_enabled",rotation_enabled); N("rotation_interval",rotation_interval);
    N("rotation_mask",rotation_mask); N("revision",state_revision);
    N("brightness_percent",screen_brightness_percent); S("brightness_method","software_dimming");
    B("buttons_enabled",0); S("button_source","disabled"); B("touch_enabled",0);
    B("buttons_idle",1); B("buttons_stuck",0); S("last_key","disabled");
    B("auto_brightness",auto_brightness_enabled); B("pin_required",s->auth_enabled);
    B("smart_home_visible",smart_home_visible);
    B("page_migrated_from_devices",smart_home_page_migrated);
    /* G3: wallpaper state（display.json 镜像；current.json 是文件权威指针） */
    S("theme_id","liquid-glass-v1"); S("wallpaper_id",wp_current_id());
    N("wallpaper_revision",wp_current_revision()); N("wallpaper_preset",wp_current_preset());
    N("light_mask",s->light_mask); N("curtain",s->curtain); N("scene",s->scene);
    N("sleep_timer_remaining_seconds",sleep_timer_remaining_seconds());
    N("curtain_timeout",s->curtain_timeout_seconds); S("backend",backend_name());
    B("gpio_conflict",output_pins_conflict());
    cJSON *names=cJSON_AddArrayToObject(o,"light_names");
    for(int i=0;i<4;i++) cJSON_AddItemToArray(names,cJSON_CreateString(s->light_names[i]));
    S("curtain_name",s->curtain_device_name);
    S("title",s->panel_title); S("location",s->location); S("weather",s->weather);
    S("temperature",s->temperature); S("humidity",s->humidity);
    S("forecast_today",s->forecast_today); S("forecast_tomorrow",s->forecast_tomorrow);
    S("forecast_after",s->forecast_after); S("weather_updated",s->weather_updated);
    S("server_name",s->server_name); S("server_address",s->server_address); S("server_status",s->server_status);
    S("nas_name",s->nas_name); S("nas_address",s->nas_address); S("nas_status",s->nas_status);
    S("notice",s->notice); N("uptime_seconds",s->uptime_seconds);
    N("memory_used_percent",s->memory_used_percent); N("storage_used_percent",s->storage_used_percent);
    N("storage_available_mb",s->storage_available_mb); N("wifi_signal_dbm",s->wifi_signal_dbm);
    S("system_alert",s->system_alert); S("load_average",s->load_average);
    S("indoor_temperature",s->indoor_temperature); S("indoor_humidity",s->indoor_humidity);
    S("air_quality",s->air_quality); N("illuminance_lux",s->illuminance_lux);
    S("sensor_updated",s->sensor_updated); B("sensors_valid",s->sensors_valid); B("sensor_stale",s->sensor_stale);
    time_t t=time(NULL); struct tm local; localtime_r(&t,&local);
    char clock_text[12],date_text[32];
    strftime(clock_text,sizeof(clock_text),"%H:%M",&local);
    strftime(date_text,sizeof(date_text),"%Y / %m / %d",&local);
    S("clock",clock_text); S("date",date_text); N("timestamp",t);
    /* M5-A: words-page minimal snapshot (read-only over pv2 word entities).
     * TRAE-F2: phonetic/meaning/example come from the pv2 annotation entity
     * (id = word id) when it exists — empty strings keep the "—" placeholder
     * behaviour and the page never invents content. */
    {
        char wt[96];int wt_total=0,wt_index=0;char wd[16];
        int wd_done=0,wd_review=0,wd_goal=0,wd_stale=0;
        char wph[160],wmn[640],wex[640];   /* R1: 对齐手机端 3072B 注释上限，
                                            * API 消费侧不再丢长条目内容 */
        words_today_snapshot(wt,sizeof wt,&wt_total,&wt_index,wd,sizeof wd,
                             wph,sizeof wph,wmn,sizeof wmn,wex,sizeof wex,
                             &wd_done,&wd_review,&wd_goal,&wd_stale);
        N("words_total",wt_total); N("words_index",wt_index); S("words_date",wd);
        N("words_done",wd_done); N("words_review",wd_review);
        N("words_goal",wd_goal); B("words_stale",wd_stale);
        if(wt[0]){
            cJSON *w=cJSON_AddObjectToObject(o,"word_today");
            if(w){
                cJSON_AddStringToObject(w,"text",wt);
                cJSON_AddStringToObject(w,"phonetic",wph);
                cJSON_AddStringToObject(w,"meaning",wmn);
                cJSON_AddStringToObject(w,"example",wex);
            }
        }
    }
    /* M5-B: briefing minimal snapshot (pre-paged, ≤3 short items per screen).
     * Absent when the store has no briefing for today - the page renders its
     * placeholder and never invents content (实施方案 41 行). */
    {
        cJSON *br=cJSON_CreateObject();
        if(br&&briefing_snapshot(br)) cJSON_AddItemToObject(o,"briefing",br);
        else cJSON_Delete(br);
    }
    /* N6: timetable/plans minimal snapshots (pre-paged, ≤3 items per screen,
     * 续工方案 §4.2). Absent when the store has no course window / no plan
     * rows - the pages render their waiting-for-sync placeholders. */
    {
        cJSON *tt=cJSON_CreateObject();
        if(tt&&timetable_snapshot(tt)) cJSON_AddItemToObject(o,"timetable",tt);
        else cJSON_Delete(tt);
    }
    {
        cJSON *pl=cJSON_CreateObject();
        if(pl&&plans_snapshot(pl)) cJSON_AddItemToObject(o,"plans",pl);
        else cJSON_Delete(pl);
    }
#undef S
#undef N
#undef B
    demo_snapshot(o);
    return o;
}
static int atomic_json(const char *path,cJSON *o,int durable) {
    char tmp[256]; snprintf(tmp,sizeof(tmp),"%s.tmp",path);
    char *s=cJSON_PrintUnformatted(o); if(!s) return -1;
    FILE *f=fopen(tmp,"w"); if(!f){free(s);return -1;}
    int ok=fputs(s,f)>=0 && fflush(f)==0;
    if(durable && ok) ok=fsync(fileno(f))==0;
    if(fclose(f)!=0) ok=0;
    free(s);
    if(!ok || rename(tmp,path)!=0){unlink(tmp);return -1;} return 0;
}
static void export_snapshot(const home_state_t *s) {
    int64_t now=monotonic_ms(); if(now-last_snapshot_ms<1000)return;
    last_snapshot_ms=now;
    cJSON *o=state_json(s); if(o){atomic_json(SNAPSHOT,o,0);cJSON_Delete(o);}
}
static void send_state(int fd,const home_state_t *s) {
    cJSON *o=state_json(s); char *body=o?cJSON_PrintUnformatted(o):NULL;
    send_response(fd,body?"200 OK":"500 Internal Server Error","application/json; charset=utf-8",body?body:"{}");
    free(body);cJSON_Delete(o);
}
static int integer(cJSON *o,const char *key,int lo,int hi,int *value) {
    cJSON *v=cJSON_GetObjectItemCaseSensitive(o,key);
    if(!cJSON_IsNumber(v)||v->valuedouble<lo||v->valuedouble>hi||v->valuedouble!=(int)v->valuedouble)return 0;
    *value=v->valueint;return 1;
}
static int page_index(const char *v) {
    if(v)for(int i=0;i<PAGE_COUNT;i++)if(!strcmp(v,page_names[i]))return i;
    return -1;
}
static int persist_display(int page,int rotation,int interval,int mask,int brightness) {
    cJSON *o=cJSON_CreateObject(); if(!o)return -1;
    cJSON_AddNumberToObject(o,"page",page);cJSON_AddNumberToObject(o,"rotation",rotation);
    cJSON_AddNumberToObject(o,"interval",interval);cJSON_AddNumberToObject(o,"mask",mask);
    cJSON_AddNumberToObject(o,"brightness",brightness);
    cJSON_AddBoolToObject(o,"smart_home_visible",smart_home_visible);
    /* G3: 主配置只增主题四字段，不改既有字段（PLAN §7） */
    cJSON_AddStringToObject(o,"theme_id","liquid-glass-v1");
    cJSON_AddStringToObject(o,"wallpaper_id",wp_current_id());
    cJSON_AddNumberToObject(o,"wallpaper_revision",(double)wp_current_revision());
    cJSON_AddNumberToObject(o,"wallpaper_preset",wp_current_preset());
    int r=atomic_json(REMOTE_CONFIG,o,1);cJSON_Delete(o);return r;
}
/* Keeps the pre-migration display.json. Written once: the first migration is
 * the one whose "before" picture matters, and re-running must not overwrite
 * that evidence. Every migration is appended to the log instead. */
static void backup_display_config(int from_page) {
    FILE *existing=fopen(SMART_HOME_BACKUP,"rb");
    if(!existing){
        FILE *src=fopen(REMOTE_CONFIG,"rb");
        if(src){
            FILE *dst=fopen(SMART_HOME_BACKUP,"wb");
            if(dst){char chunk[256];size_t got;
                while((got=fread(chunk,1,sizeof(chunk),src))>0)fwrite(chunk,1,got,dst);
                fclose(dst);}
            fclose(src);
        }
    }else fclose(existing);
    FILE *log=fopen(MIGRATION_LOG,"a");
    if(log){
        time_t t=time(NULL);struct tm lt;localtime_r(&t,&lt);
        fprintf(log,"%04d-%02d-%02d %02d:%02d:%02d migr-1 page=%s -> home (smart_home_visible off)\n",
                lt.tm_year+1900,lt.tm_mon+1,lt.tm_mday,lt.tm_hour,lt.tm_min,lt.tm_sec,
                (from_page>=0&&from_page<PAGE_COUNT)?page_names[from_page]:"?");
        fclose(log);
    }
}
/* The single enforcement point for the hidden smart-home entry: no code path
 * may leave it selected or in the rotation set. Returns 1 when it changed
 * state, so the caller repersists display.json. */
static int enforce_smart_home_visibility(void) {
    int changed=0;
    if(smart_home_visible)return 0;
    if(rotation_mask&(1<<3)){rotation_mask&=~(1<<3);changed=1;}
    if(!rotation_mask){rotation_mask=1;changed=1;}          /* never rotate with no page */
    if(display_page==3){
        backup_display_config(display_page);                 /* keep the "before" file */
        display_page=0;smart_home_page_migrated=1;changed=1;
    }
    return changed;
}
static void load_remote_config(void) {
    FILE *f=fopen(REMOTE_CONFIG,"r");if(!f)return;
    char buf[512];size_t n=fread(buf,1,sizeof(buf)-1,f);fclose(f);buf[n]=0;
    cJSON *o=cJSON_Parse(buf);int p,r,i,m,b;
    if(o){
        /* Optional in v3.1 configs: absent means "keep the hidden default". */
        cJSON *vis=cJSON_GetObjectItemCaseSensitive(o,"smart_home_visible");
        if(cJSON_IsBool(vis))smart_home_visible=cJSON_IsTrue(vis);
        /* G3: wallpaper revision floor（跨重启单调；文件权威在 current.json） */
        cJSON *wrev=cJSON_GetObjectItemCaseSensitive(o,"wallpaper_revision");
        if(cJSON_IsNumber(wrev))wp_note_display_revision((long)wrev->valuedouble);
    }
    if(o&&integer(o,"page",0,7,&p)&&integer(o,"rotation",0,1,&r)&&integer(o,"interval",3,120,&i)
       &&integer(o,"mask",1,255,&m)&&integer(o,"brightness",20,100,&b)){
        display_page=p;rotation_enabled=r;rotation_interval=i;rotation_mask=m;screen_brightness_percent=b;
        if(enforce_smart_home_visibility())
            persist_display(display_page,rotation_enabled,rotation_interval,rotation_mask,screen_brightness_percent);
    }
    cJSON_Delete(o);
}

/* Small bounded HTTP reader (T03B2): the implementation lives in
 * http_reader.inc so the host unit test can exercise the exact production
 * code with scripted TCP segmentation (PV2_RECV/PV2_NOW hooks). */
#include "http_reader.inc"
static int remote_auth(const char *req,const home_state_t *s) {
    /* 取消配对密码：auth_enabled=0 时一律放行，旧 /api/v1/ 命令与壁纸上传不再
     * 要求 X-Control-Pin。与 service.c::control_pin_valid() 行为一致。 */
    if(!s->auth_enabled)return 1;
    const char *p=strstr(req,"\r\n");if(!p)return 0;p+=2;
    while(*p){const char *e=strstr(p,"\r\n");if(!e||e==p)break;
        if(!strncasecmp(p,"X-Control-Pin:",14)){p+=14;while(p<e&&(*p==' '||*p=='\t'))p++;
            return (size_t)(e-p)==strlen(s->control_pin)&&!strncmp(p,s->control_pin,e-p);}
        p=e+2;
    }return 0;
}
static int handle_remote(int fd,const char *method,const char *path,const char *req,home_state_t *s) {
    if(strncmp(path,"/api/v1/",8))return 0;
    if(!strcmp(path,"/api/v1/state")||!strcmp(path,"/api/v1/capabilities")){
        if(strcmp(method,"GET")){send_response(fd,"405 Method Not Allowed","application/json","{\"error\":\"GET required\"}");return 1;}
        if(!strcmp(path,"/api/v1/state"))send_state(fd,s);
        else {
            /* Built from the switch, so a client that follows capabilities can
             * never offer the hidden entry. The id itself stays registered in
             * page_names and in the old endpoints - only its listing changes. */
            cJSON *c=cJSON_CreateObject();char *body=NULL;
            if(c){
                cJSON_AddNumberToObject(c,"api",1);cJSON_AddStringToObject(c,"display","lvgl");
                cJSON *pages=cJSON_AddArrayToObject(c,"pages");
                for(int i=0;i<PAGE_COUNT;i++)
                    if(i!=3||smart_home_visible)cJSON_AddItemToArray(pages,cJSON_CreateString(page_names[i]));
                cJSON_AddBoolToObject(c,"smart_home_visible",smart_home_visible);
                cJSON_AddNumberToObject(c,"brightness_min",20);cJSON_AddNumberToObject(c,"brightness_max",100);
                cJSON_AddStringToObject(c,"brightness_method","software_dimming");
                cJSON_AddBoolToObject(c,"local_input",0);cJSON_AddBoolToObject(c,"touch",0);
                body=cJSON_PrintUnformatted(c);cJSON_Delete(c);
            }
            send_response(fd,"200 OK","application/json",body?body:"{\"api\":1}");free(body);
        }
        return 1;
    }
    if(strcmp(method,"POST")){send_response(fd,"405 Method Not Allowed","application/json","{\"error\":\"POST required\"}");return 1;}
    if(!remote_auth(req,s)){send_response(fd,"403 Forbidden","application/json","{\"error\":\"invalid control pin\"}");return 1;}
    /* G3: wallpaper session/activate/reset（upload 不经过这里——handle_client
     * 在 read_request 之前已把流式上传分派走）。 */
    if(!strncmp(path,"/api/v1/theme/wallpaper/",24))return wallpaper_route(fd,method,path,req,s);
    const char *body=strstr(req,"\r\n\r\n");
    cJSON *o=body?cJSON_ParseWithOpts(body+4,NULL,1):NULL;
    int p=display_page,r=rotation_enabled,i=rotation_interval,m=rotation_mask,b=screen_brightness_percent;
    int ok=cJSON_IsObject(o),persist=1;
    if(!strcmp(path,"/api/v1/display/page")){
        p=page_index(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o,"page")));ok=ok&&p>=0;r=0;
        /* The hidden entry keeps its id and its endpoint: asking for it is not
         * an error, it simply resolves to the first visible page. */
        if(ok&&!smart_home_visible&&p==3)p=0;
        /* M5-B: phone-side briefing screen switch (实施方案 41 行). A numeric
         * "part" is an absolute screen; its absence steps to the next one.
         * Either form pins the paging until the business day rolls over. */
        if(ok&&p==5){
            int q;
            if(integer(o,"part",0,9,&q))briefing_part=q;
            else if(cJSON_HasObjectItem(o,"part"))ok=0;
            else if(briefing_pages>0)briefing_part=(briefing_part+1)%briefing_pages;
            briefing_pinned=1;
        }
        /* N6: timetable/plans 同款 phone-side screen switch（续工方案 §4.2）。
         * 显式 part=绝对屏号；缺省 part 步进到下一屏；两者都固定分页直到业务
         * 日切换。 */
        if(ok&&p==6){
            int q;
            if(integer(o,"part",0,9,&q))timetable_part=q;
            else if(cJSON_HasObjectItem(o,"part"))ok=0;
            else if(timetable_pages>0)timetable_part=(timetable_part+1)%timetable_pages;
            timetable_pinned=1;
        }else if(ok&&p==7){
            int q;
            if(integer(o,"part",0,9,&q))plans_part=q;
            else if(cJSON_HasObjectItem(o,"part"))ok=0;
            else if(plans_pages>0)plans_part=(plans_part+1)%plans_pages;
            plans_pinned=1;
        }
    }else if(!strcmp(path,"/api/v1/display/rotation")){
        cJSON *en=cJSON_GetObjectItemCaseSensitive(o,"enabled");ok=ok&&cJSON_IsBool(en);r=cJSON_IsTrue(en);
        if(cJSON_HasObjectItem(o,"interval"))ok=ok&&integer(o,"interval",3,120,&i);
        if(cJSON_HasObjectItem(o,"pages")){
            cJSON *a=cJSON_GetObjectItemCaseSensitive(o,"pages");m=0;ok=ok&&cJSON_IsArray(a)&&cJSON_GetArraySize(a)>0&&cJSON_GetArraySize(a)<=PAGE_COUNT;
            cJSON *e; cJSON_ArrayForEach(e,a){int n=page_index(cJSON_GetStringValue(e));if(n<0)ok=0;else m|=1<<n;}
            if(!smart_home_visible){m&=~(1<<3);if(!m)ok=0;}  /* hidden: not rotatable */
        }
    }else if(!strcmp(path,"/api/v1/display/visibility")){
        /* The advanced-settings switch. It flips visibility and nothing else:
         * no page, no endpoint and no byte of the hidden data is touched. */
        cJSON *v=cJSON_GetObjectItemCaseSensitive(o,"smart_home_visible");
        ok=ok&&cJSON_IsBool(v);
        if(ok){
            smart_home_visible=cJSON_IsTrue(v);
            if(enforce_smart_home_visibility()){
                p=display_page;r=rotation_enabled;i=rotation_interval;m=rotation_mask;b=screen_brightness_percent;
            }
        }
    }else if(!strcmp(path,"/api/v1/display/brightness")){
        ok=ok&&integer(o,"value",20,100,&b);
        cJSON *v=cJSON_GetObjectItemCaseSensitive(o,"persist");if(v){ok=ok&&cJSON_IsBool(v);persist=cJSON_IsTrue(v);}
    }else if(!strcmp(path,"/api/v1/display/notice")){
        const char *v=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o,"text"));
        ok=ok&&v&&strlen(v)<sizeof(s->notice);
        if(ok){for(const unsigned char *q=(const unsigned char *)v;*q;q++)if(*q<32)ok=0;}
        if(ok){char old[sizeof(s->notice)];memcpy(old,s->notice,sizeof(old));snprintf(s->notice,sizeof(s->notice),"%s",v);
            if(save_info_config(s)<0){memcpy(s->notice,old,sizeof(old));goto storage_error;}}
    }else if(!strcmp(path,"/api/v1/light/set")){
        int ch,enabled;ok=ok&&integer(o,"channel",1,4,&ch)&&integer(o,"enabled",0,1,&enabled);
        if(ok){if(enabled)s->light_mask|=1u<<(ch-1);else s->light_mask&=~(1u<<(ch-1));apply_outputs(s);save_state(s);}
    }else{
        cJSON_Delete(o);send_response(fd,"404 Not Found","application/json","{\"error\":\"unknown endpoint\"}");return 1;
    }
    if(!ok){cJSON_Delete(o);send_response(fd,"400 Bad Request","application/json","{\"error\":\"invalid parameters\"}");return 1;}
    if((!strcmp(path,"/api/v1/display/page")||!strcmp(path,"/api/v1/display/rotation"))&&demo_stop()<0)goto storage_error;
    if(persist&&strncmp(path,"/api/v1/display/notice",22)&&strcmp(path,"/api/v1/light/set")&&persist_display(p,r,i,m,b)<0)goto storage_error;
    display_page=p;current_view=VIEW_DASHBOARD;rotation_enabled=r;rotation_interval=i;rotation_mask=m;
    screen_brightness_percent=b;auto_brightness_enabled=0;state_revision++;
    next_page_switch_ms=monotonic_ms()+i*1000;last_snapshot_ms=0;export_snapshot(s);
    cJSON_Delete(o);send_state(fd,s);return 1;
storage_error:
    cJSON_Delete(o);send_response(fd,"500 Internal Server Error","application/json","{\"error\":\"cannot save settings\"}");return 1;
}
