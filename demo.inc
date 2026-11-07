/* Integrated feature demos. All sample data lives outside the user's store.
 * Timer uses monotonic time; reboot restores a paused timer, never fake credit. */
#define DEMO_FILE "/mnt/UDISK/panel-v3/demo.json"
static struct {
    int enabled,page,autoplay,scene,remaining,duration,running,sessions,seconds;
    int card,revealed,known,review,offline,lux,pc,knob;
    double deadline;
    char task[96],exam[96],event[96],saved[32];
    int64_t tick,next;
} demo={.scene=-1,.remaining=1500,.duration=1500,.lux=350};
static const char *demo_titles[]={"情景切屏","专注学习","知识复习","考试倒计时","离线学习","电脑副屏","桌面环境","快捷操作"};
static const char *demo_questions[]={"电阻上的电压如何求？","RC 电路时间常数？","GPIO 上拉有什么作用？"};
static const char *demo_answers[]={"U = I x R","tau = R x C","避免输入悬空"};
static int demo_num(cJSON *o,const char *k,int def){cJSON *v=cJSON_GetObjectItemCaseSensitive(o,k);return cJSON_IsNumber(v)?v->valueint:def;}
static void demo_text(cJSON *o,const char *k,char *dst,size_t cap){const char *v=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o,k));if(v&&strlen(v)<cap)snprintf(dst,cap,"%s",v);}
static cJSON *demo_data(void){
    cJSON *o=cJSON_CreateObject();
#define DN(k) cJSON_AddNumberToObject(o,#k,demo.k)
    DN(enabled);DN(page);DN(autoplay);DN(scene);DN(remaining);DN(duration);DN(running);
    DN(sessions);DN(seconds);DN(card);DN(revealed);DN(known);DN(review);DN(offline);
    DN(lux);DN(pc);DN(knob);DN(deadline);
#undef DN
    cJSON_AddStringToObject(o,"task",demo.task);cJSON_AddStringToObject(o,"exam",demo.exam);
    cJSON_AddStringToObject(o,"event",demo.event);cJSON_AddStringToObject(o,"saved",demo.saved);
    return o;
}
static int demo_save(void){cJSON *o=demo_data();int rc=atomic_json(DEMO_FILE,o,1);cJSON_Delete(o);return rc;}
static int demo_stop(void){
    int enabled=demo.enabled,autoplay=demo.autoplay;
    demo.enabled=0;demo.autoplay=0;
    if(demo_save()<0){demo.enabled=enabled;demo.autoplay=autoplay;return -1;}return 0;
}
static void demo_load(void){
    snprintf(demo.task,sizeof demo.task,"复习电路分析");snprintf(demo.exam,sizeof demo.exam,"电路分析测验（示例）");
    snprintf(demo.event,sizeof demo.event,"等待电脑任务通知");demo.deadline=(double)time(NULL)+7*86400;
    time_t t=time(NULL);struct tm tm;localtime_r(&t,&tm);strftime(demo.saved,sizeof demo.saved,"%m-%d %H:%M",&tm);
    FILE *f=fopen(DEMO_FILE,"r");if(!f)return;char b[4096];size_t n=fread(b,1,sizeof b-1,f);fclose(f);b[n]=0;
    cJSON *o=cJSON_Parse(b);if(!o)return;
#define DL(k,lo,hi) do{int v=demo_num(o,#k,demo.k);if(v>=lo&&v<=hi)demo.k=v;}while(0)
    DL(enabled,0,1);DL(page,0,7);DL(autoplay,0,1);DL(scene,-1,2);DL(remaining,0,7200);DL(duration,1,7200);
    DL(sessions,0,100000);DL(seconds,0,100000000);DL(card,0,2);DL(revealed,0,1);DL(known,0,100000);
    DL(review,0,100000);DL(offline,0,1);DL(lux,0,2000);DL(pc,0,100);DL(knob,0,2);
#undef DL
    cJSON *v=cJSON_GetObjectItemCaseSensitive(o,"deadline");if(cJSON_IsNumber(v)&&v->valuedouble>0)demo.deadline=v->valuedouble;
    demo_text(o,"task",demo.task,sizeof demo.task);demo_text(o,"exam",demo.exam,sizeof demo.exam);
    demo_text(o,"event",demo.event,sizeof demo.event);demo_text(o,"saved",demo.saved,sizeof demo.saved);
    demo.running=0;cJSON_Delete(o);
}
static void demo_tick(void){
    int64_t now=monotonic_ms();
    if(demo.running){
        int elapsed=(int)((now-demo.tick)/1000);
        if(elapsed>0){demo.tick+=(int64_t)elapsed*1000;demo.remaining-=elapsed;
            if(demo.remaining<=0){demo.remaining=0;demo.running=0;demo.sessions++;demo.seconds+=demo.duration;
                demo.page=2;demo.revealed=0;demo.autoplay=0;demo_save();}
        }
    }
    if(demo.enabled&&demo.autoplay&&now>=demo.next){demo.page=(demo.page+1)%8;demo.next=now+12000;}
}
static void demo_snapshot(cJSON *root){
    demo_tick();cJSON *o=demo_data();if(!o)return;
    char eyebrow[192]="",metric[128]="",detail[256]="",line1[256]="",line2[256]="",foot[192]="";
    snprintf(foot,sizeof foot,"功能体验 %d/8 · 手机网页“探索”可操作",demo.page+1);
    time_t t=time(NULL);struct tm tm;localtime_r(&t,&tm);
    int scene=demo.scene>=0?demo.scene:(tm.tm_hour>=22||tm.tm_hour<6?2:tm.tm_hour<9?0:1);
    switch(demo.page){
    case 0:
        snprintf(eyebrow,sizeof eyebrow,"%s · 早晨 / 上课 / 夜间",demo.scene<0?"按板卡时间自动选择":"时间场景模拟");
        snprintf(metric,sizeof metric,"%s",scene==0?"早安，开始今天":scene==1?"下一节 · 电路分析":"夜间安静模式");
        snprintf(detail,sizeof detail,"%s",scene==0?"示例：今天 2 节课 · 1 项复习计划":scene==1?"示例课程：09:00–09:45 · 教学楼 302":"亮度临时降至 20% · 退出后恢复原设置");
        snprintf(line1,sizeof line1,"%s",scene==0?"优先查看课程与今日任务":scene==1?"课前突出时间与教室，减少找课表":"保留时间和次日安排，减少干扰");
        snprintf(line2,sizeof line2,"课程是示例数据，未修改你的真实课表");
        if(demo.enabled&&scene==2)cJSON_SetNumberValue(cJSON_GetObjectItem(root,"brightness_percent"),20);
        break;
    case 1:
        snprintf(eyebrow,sizeof eyebrow,"%s · %s",demo.running?"正在专注":demo.remaining==0?"本轮完成":"已暂停 / 待开始",demo.task);
        snprintf(metric,sizeof metric,"%02d:%02d",demo.remaining/60,demo.remaining%60);
        snprintf(detail,sizeof detail,"完成 %d 轮 · 累计 %d 分 %d 秒",demo.sessions,demo.seconds/60,demo.seconds%60);
        snprintf(line1,sizeof line1,"计时结束自动进入知识复习");snprintf(line2,sizeof line2,"支持 25 分钟或 10 秒快速体验；重启后暂停");break;
    case 2:
        snprintf(eyebrow,sizeof eyebrow,"电路与单片机 · 示例卡片 %d / 3",demo.card+1);
        snprintf(metric,sizeof metric,"%s",demo.revealed?demo_answers[demo.card]:"先想一想");
        snprintf(detail,sizeof detail,"%s",demo_questions[demo.card]);
        snprintf(line1,sizeof line1,"%s",demo.revealed?"在手机选择“记住了”或“待复习”":"点击“揭示答案”，再判断掌握情况");
        snprintf(line2,sizeof line2,"本 Demo：记住 %d 次 · 待复习 %d 次",demo.known,demo.review);break;
    case 3:{
        double diff=demo.deadline-(double)t;long left=diff>0?(long)diff:0;
        snprintf(eyebrow,sizeof eyebrow,"%s",demo.exam);
        snprintf(metric,sizeof metric,"%ld 天 %02ld 小时",left/86400,(left%86400)/3600);
        snprintf(detail,sizeof detail,"%s",diff<=0?"已到截止时间":"手机可修改名称与截止日期");
        snprintf(line1,sizeof line1,"建议示例：今天完成 3 道电路分析题");snprintf(line2,sizeof line2,"截止时间与名称保存在板卡本地");break;}
    case 4:
        snprintf(eyebrow,sizeof eyebrow,"离线策略体验 · 本地示例缓存");
        snprintf(metric,sizeof metric,"%s",demo.offline?"离线缓存模式":"本地内容就绪");
        snprintf(detail,sizeof detail,"缓存保存于 %s · 重启后仍可读取",demo.saved);
        snprintf(line1,sizeof line1,"示例：09:00 电路分析 / RC 时间常数复习");
        snprintf(line2,sizeof line2,"仅模拟离线状态，不会断开板卡网络");break;
    case 5:
        snprintf(eyebrow,sizeof eyebrow,"电脑任务通知 · 可接收实际 HTTP 事件");
        snprintf(metric,sizeof metric,"%d%%",demo.pc);snprintf(detail,sizeof detail,"%s",demo.event);
        snprintf(line1,sizeof line1,"手机按钮发送模拟事件；电脑脚本可推送真实进度");
        snprintf(line2,sizeof line2,"收到通知后自动切到本页");break;
    case 6:{
        int light=demo.lux<50?20:demo.lux<300?50:100;
        snprintf(eyebrow,sizeof eyebrow,"传感器模拟 · 未接入真实硬件");
        snprintf(metric,sizeof metric,"%d lx",demo.lux);snprintf(detail,sizeof detail,"示例温度 25.6°C · 湿度 48%%");
        snprintf(line1,sizeof line1,"光照联动：当前显示亮度 %d%%",light);
        snprintf(line2,sizeof line2,"拖动手机光照滑块，体验实际屏幕调光");
        if(demo.enabled)cJSON_SetNumberValue(cJSON_GetObjectItem(root,"brightness_percent"),light);break;}
    case 7:
        snprintf(eyebrow,sizeof eyebrow,"实体旋钮交互模拟 · 使用手机按钮");
        snprintf(metric,sizeof metric,"%s",demo.knob==0?"开始 / 暂停":demo.knob==1?"揭示答案":"下一张卡片");
        snprintf(detail,sizeof detail,"左转 / 右转选择，按下执行");
        snprintf(line1,sizeof line1,"专注：%s · 当前复习卡 %d/3",demo.running?"进行中":"已暂停",demo.card+1);
        snprintf(line2,sizeof line2,"没有占用 GPIO；真实旋钮尚未接入");break;
    }
    cJSON_AddStringToObject(o,"title",demo_titles[demo.page]);cJSON_AddStringToObject(o,"eyebrow",eyebrow);
    cJSON_AddStringToObject(o,"metric",metric);cJSON_AddStringToObject(o,"detail",detail);
    cJSON_AddStringToObject(o,"line1",line1);cJSON_AddStringToObject(o,"line2",line2);cJSON_AddStringToObject(o,"footer",foot);
    cJSON_AddItemToObject(root,"demo",o);
}
static int demo_route(int fd,const char *method,const char *path,const char *req,home_state_t *state){
    if(!strcmp(path,"/demo.js")){send_file_response(fd,"/mnt/UDISK/panel-v3/demo.js","application/javascript; charset=utf-8",NULL);return 1;}
    if(strcmp(path,"/api/demo"))return 0;
    if(!strcmp(method,"GET")){send_state(fd,state);return 1;}
    if(strcmp(method,"POST")){send_response(fd,"405 Method Not Allowed","application/json","{\"error\":\"POST required\"}");return 1;}
    if(!remote_auth(req,state)){send_response(fd,"403 Forbidden","application/json","{\"error\":\"invalid control pin\"}");return 1;}
    const char *body=strstr(req,"\r\n\r\n");cJSON *o=body?cJSON_Parse(body+4):NULL;
    const char *action=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o,"action"));
    int value=0,ok=1;demo_tick();
    /* Capture old state so invalid requests or storage failures cannot half-apply. */
    __typeof__(demo) old=demo;
    if(!action)ok=0;
    else if(!strcmp(action,"page")&&integer(o,"value",0,7,&value)){demo.enabled=1;demo.page=value;demo.autoplay=0;}
    else if(!strcmp(action,"tour")){demo.enabled=1;demo.page=0;demo.autoplay=1;demo.next=monotonic_ms()+12000;}
    else if(!strcmp(action,"exit")){demo.enabled=0;demo.autoplay=0;}
    else if(!strcmp(action,"scene")&&integer(o,"value",-1,2,&value)){demo.scene=value;demo.page=0;demo.enabled=1;demo.autoplay=0;}
    else if(!strcmp(action,"start")&&integer(o,"value",1,7200,&value)){
        const char *task=cJSON_GetStringValue(cJSON_GetObjectItem(o,"task"));
        if(task&&(!*task||strlen(task)>=sizeof demo.task))ok=0;
        else {if(task)snprintf(demo.task,sizeof demo.task,"%s",task);demo.duration=demo.remaining=value;
            demo.running=1;demo.tick=monotonic_ms();demo.enabled=1;demo.page=1;demo.autoplay=0;}
    }
    else if(!strcmp(action,"pause"))demo.running=0;
    else if(!strcmp(action,"resume")){if(demo.remaining>0){demo.running=1;demo.tick=monotonic_ms();}}
    else if(!strcmp(action,"reveal"))demo.revealed=1;
    else if(!strcmp(action,"next")){demo.card=(demo.card+1)%3;demo.revealed=0;}
    else if(!strcmp(action,"known")||!strcmp(action,"review")){
        if(!demo.revealed)ok=0;else{if(!strcmp(action,"known"))demo.known++;else demo.review++;demo.card=(demo.card+1)%3;demo.revealed=0;}
    }
    else if(!strcmp(action,"deadline")){
        cJSON *v=cJSON_GetObjectItem(o,"value");const char *name=cJSON_GetStringValue(cJSON_GetObjectItem(o,"name"));
        if(!cJSON_IsNumber(v)||v->valuedouble<946684800||v->valuedouble>4102444800.0||!name||!*name||strlen(name)>=sizeof demo.exam)ok=0;
        else{demo.deadline=v->valuedouble;snprintf(demo.exam,sizeof demo.exam,"%s",name);}
    }
    else if(!strcmp(action,"offline")&&integer(o,"value",0,1,&value))demo.offline=value;
    else if(!strcmp(action,"light")&&integer(o,"value",0,2000,&value))demo.lux=value;
    else if(!strcmp(action,"pc")&&integer(o,"value",0,100,&value)){
        const char *msg=cJSON_GetStringValue(cJSON_GetObjectItem(o,"message"));
        if(!msg||!*msg||strlen(msg)>=sizeof demo.event)ok=0;
        else{demo.pc=value;snprintf(demo.event,sizeof demo.event,"%s",msg);demo.enabled=1;demo.page=5;demo.autoplay=0;}
    }
    else if(!strcmp(action,"turn")&&integer(o,"value",-1,1,&value))demo.knob=(demo.knob+value+3)%3;
    else if(!strcmp(action,"press")){
        if(demo.knob==0){if(demo.remaining==0)demo.remaining=demo.duration;demo.running=!demo.running;demo.tick=monotonic_ms();}
        else if(demo.knob==1)demo.revealed=1;else{demo.card=(demo.card+1)%3;demo.revealed=0;}
    }
    else ok=0;
    cJSON_Delete(o);
    if(!ok){demo=old;send_response(fd,"400 Bad Request","application/json","{\"error\":\"invalid demo action\"}");return 1;}
    if(demo_save()!=0){demo=old;send_response(fd,"500 Internal Server Error","application/json","{\"error\":\"cannot save demo\"}");return 1;}
    last_snapshot_ms=0;export_snapshot(state);send_state(fd,state);return 1;
}
