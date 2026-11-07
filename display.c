#include "ui/ui.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#ifdef DESKTOP
#include <windows.h>
#include <psapi.h>
#include <SDL.h>
#else
#include <linux/fb.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <signal.h>
#endif
#define WIDTH 480
#define HEIGHT 272
#ifdef DESKTOP
#define RENDER_HEARTBEAT_PATH "build/display-heartbeat"
#else
#define RENDER_HEARTBEAT_PATH "/tmp/panel-v3-render-heartbeat"
#endif
static uint32_t canvas[WIDTH*HEIGHT];
static uint32_t drawbuf[WIDTH*48];
static int brightness=100,dirty=1;
static unsigned long rendered_revision;
static int rendered_page;
static uint32_t last_heartbeat_tick;
#ifdef DESKTOP
static int running=1;
static SDL_Window *window;static SDL_Renderer *renderer;static SDL_Texture *texture;
static uint32_t ticks(void){return SDL_GetTicks();}
#else
static volatile sig_atomic_t running=1;
static int fbfd=-1;static struct fb_var_screeninfo var;static struct fb_fix_screeninfo fix;
static uint8_t *fbmap;static size_t mapsize;
static uint32_t ticks(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint32_t)(t.tv_sec*1000+t.tv_nsec/1000000);}
static void stop(int sig){(void)sig;running=0;}
#endif
static void flush(lv_display_t *d,const lv_area_t *a,uint8_t *pixels){
    uint32_t *src=(uint32_t *)pixels;
    for(int y=a->y1;y<=a->y2;y++)for(int x=a->x1;x<=a->x2;x++){
        uint32_t v=*src++;if(x>=0&&x<WIDTH&&y>=0&&y<HEIGHT)canvas[y*WIDTH+x]=v|0xff000000u;
    }
    dirty=1;lv_display_flush_ready(d);
}
/* Monotonic seconds since boot.
 *
 * T03: the watchdog used to compare the wall-clock field below against
 * `date +%s`. A clock step - and the T113 has no RTC, so the clock IS stepped
 * at boot when NTP lands - makes that difference negative or absurd, which the
 * old watchdog read as "render stalled" and rebooted a perfectly healthy
 * display. /proc/uptime cannot be stepped, so the heartbeat now carries it as a
 * 4th field and the watchdog prefers it. The wall-clock field stays for logging
 * and for older readers. */
static long long monotonic_seconds(void){
#ifdef DESKTOP
    return (long long)(ticks()/1000u);
#else
    FILE *u=fopen("/proc/uptime","r");
    if(!u)return (long long)time(NULL);          /* no /proc: fall back, and let
                                                    the watchdog's negative-age
                                                    guard protect us */
    double up=0.0;
    if(fscanf(u,"%lf",&up)!=1)up=0.0;
    fclose(u);
    return (long long)up;
#endif
}
static void write_render_heartbeat(void){
    char tmp[128];
    snprintf(tmp,sizeof(tmp),"%s.tmp",RENDER_HEARTBEAT_PATH);
    FILE *f=fopen(tmp,"w");if(!f)return;
    fprintf(f,"%lu %d %lld %lld\n",rendered_revision,rendered_page,
            (long long)time(NULL),monotonic_seconds());
    fflush(f);
    fclose(f);
#ifdef DESKTOP
    /* MSVCRT rename() does not replace an existing file.  The desktop
     * preview is a test harness, so remove the old marker before replacing
     * it; the Linux target keeps the atomic rename path below. */
    remove(RENDER_HEARTBEAT_PATH);
#endif
    if(rename(tmp,RENDER_HEARTBEAT_PATH)!=0)remove(tmp);
    last_heartbeat_tick=ticks();
}
static void present(void){
    uint32_t now=ticks();
    if(!dirty){
        if((uint32_t)(now-last_heartbeat_tick)>=1000)write_render_heartbeat();
        return;
    }
    dirty=0;
#ifdef DESKTOP
    SDL_UpdateTexture(texture,NULL,canvas,WIDTH*4);
    SDL_SetTextureColorMod(texture,brightness*255/100,brightness*255/100,brightness*255/100);
    SDL_RenderClear(renderer);SDL_RenderCopyEx(renderer,texture,NULL,NULL,180.0,NULL,SDL_FLIP_NONE);SDL_RenderPresent(renderer);
#else
    for(int y=0;y<HEIGHT;y++){
        uint8_t *row=fbmap+(y+var.yoffset)*fix.line_length+var.xoffset*4;
        for(int x=0;x<WIDTH;x++){
            uint32_t c=canvas[(HEIGHT-1-y)*WIDTH+(WIDTH-1-x)];
            uint32_t r=((c>>16)&255)*brightness/100,g=((c>>8)&255)*brightness/100,b=(c&255)*brightness/100;
            uint32_t out=(r<<var.red.offset)|(g<<var.green.offset)|(b<<var.blue.offset);
            if(var.transp.length)out|=((1u<<var.transp.length)-1u)<<var.transp.offset;
            memcpy(row+x*4,&out,4);
        }
    }
#endif
    write_render_heartbeat();
}
static cJSON *read_state(const char *path){
    FILE *f=fopen(path,"rb");if(!f)return NULL;
    char buf[16384];size_t n=fread(buf,1,sizeof(buf)-1,f);fclose(f);buf[n]=0;
    return cJSON_ParseWithOpts(buf,NULL,1);
}
#ifdef DESKTOP
/* TRAE-G5 host soak harness (PLAN §9): in-process page-switch churn to prove
 * object count and RSS stay flat. Desktop-only; the ARM build is untouched. */
static unsigned long soak_rss_bytes(void){
    PROCESS_MEMORY_COUNTERS pmc;memset(&pmc,0,sizeof pmc);pmc.cb=sizeof pmc;
    return GetProcessMemoryInfo(GetCurrentProcess(),&pmc,sizeof pmc)?(unsigned long)pmc.WorkingSetSize:0;
}
static unsigned long soak_count_objs(lv_obj_t *o){
    unsigned long n=1;uint32_t c=lv_obj_get_child_cnt(o);
    for(uint32_t i=0;i<c;i++)n+=soak_count_objs(lv_obj_get_child(o,i));
    return n;
}
#endif
int main(int argc,char **argv){
    setvbuf(stdout,NULL,_IONBF,0);setvbuf(stderr,NULL,_IONBF,0);
    fprintf(stderr,"panel display starting\n");
#ifdef DESKTOP
    const char *snapshot=argc>1?argv[1]:"assets/demo.json";
    const char *font="A:assets/panel-font.ttf";
    int capture=argc>2;
    /* TRAE-G5: argv[2]=="--soak" runs the host page-switch harness instead of
     * the single-shot BMP capture; optional argv[3] overrides iterations. */
    int soak=0,soak_n=1000;
    if(capture&&!strcmp(argv[2],"--soak")){capture=0;soak=1;if(argc>3)soak_n=atoi(argv[3]);if(soak_n<1)soak_n=1;}
    if(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_TIMER)!=0){fprintf(stderr,"SDL: %s\n",SDL_GetError());return 1;}
    window=SDL_CreateWindow("T113 LVGL - phone remote",SDL_WINDOWPOS_CENTERED,SDL_WINDOWPOS_CENTERED,960,544,(capture||soak)?SDL_WINDOW_HIDDEN:SDL_WINDOW_SHOWN);
    renderer=SDL_CreateRenderer(window,-1,SDL_RENDERER_SOFTWARE);
    texture=SDL_CreateTexture(renderer,SDL_PIXELFORMAT_ARGB8888,SDL_TEXTUREACCESS_STREAMING,WIDTH,HEIGHT);
    if(!window||!renderer||!texture){fprintf(stderr,"SDL objects: %s\n",SDL_GetError());return 1;}
#else
    const char *snapshot="/tmp/panel-v3-state.json",*font="A:/mnt/UDISK/panel-v3/panel-font.ttf";
    fbfd=open("/dev/fb0",O_RDWR);if(fbfd<0||ioctl(fbfd,FBIOGET_VSCREENINFO,&var)<0||ioctl(fbfd,FBIOGET_FSCREENINFO,&fix)<0){perror("framebuffer");return 1;}
    if(var.xres!=WIDTH||var.yres!=HEIGHT||var.bits_per_pixel!=32||var.red.length!=8||var.green.length!=8||var.blue.length!=8){fprintf(stderr,"Unsupported display mode\n");return 1;}
    mapsize=fix.smem_len;if(mapsize<(size_t)(var.yoffset+HEIGHT)*fix.line_length||fix.line_length<(var.xoffset+WIDTH)*4)return 1;
    fbmap=mmap(NULL,mapsize,PROT_READ|PROT_WRITE,MAP_SHARED,fbfd,0);if(fbmap==MAP_FAILED){perror("mmap");return 1;}
    signal(SIGTERM,stop);signal(SIGINT,stop);setenv("TZ","CST-8",1);tzset();
#endif
    fprintf(stderr,"platform ready\n");lv_init();lv_tick_set_cb(ticks);
    lv_display_t *d=lv_display_create(WIDTH,HEIGHT);
    lv_display_set_color_format(d,LV_COLOR_FORMAT_XRGB8888);
    lv_display_set_buffers(d,drawbuf,NULL,sizeof(drawbuf),LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(d,flush);
    if(ui_init(font)<0){fprintf(stderr,"Cannot open font %s\n",font);return 1;}
    uint32_t next=0,start=ticks();cJSON *state=NULL;char *previous=NULL;
    while(running){
        uint32_t now=ticks();
        if((int32_t)(now-next)>=0){
            next=now+200;
            cJSON *fresh=read_state(snapshot);
            if(fresh){cJSON_Delete(state);state=fresh;}
            if(!state)state=cJSON_CreateObject();
            int stale=0;
#ifndef DESKTOP
            stale=(time(NULL)-num(state,"timestamp",0))>10;
#endif
            cJSON_DeleteItemFromObjectCaseSensitive(state,"service_stale");
            cJSON_AddBoolToObject(state,"service_stale",stale);
            cJSON *timestamp_item=cJSON_DetachItemFromObjectCaseSensitive(state,"timestamp");
            char *serialized=cJSON_PrintUnformatted(state);
            if(timestamp_item)cJSON_AddItemToObject(state,"timestamp",timestamp_item);
            if(!previous||!serialized||strcmp(previous,serialized)){
                free(previous);previous=serialized;brightness=num(state,"brightness_percent",100);
                if(brightness<20)brightness=20;if(brightness>100)brightness=100;
                ui_render(state,stale);
                rendered_revision=(unsigned long)num(state,"revision",0);
                rendered_page=num(state,"display_page",0);
            }else free(serialized);
        }
        glass_tick(now);
        lv_timer_handler();present();
#ifdef DESKTOP
        if(soak){
            for(int i=1;i<=soak_n;i++){
                cJSON *pg=cJSON_GetObjectItemCaseSensitive(state,"display_page");
                if(pg)cJSON_SetNumberValue(pg,(double)(i%6));
                uint32_t t0=ticks();
                ui_render(state,0);
                if(i==1)fprintf(stderr,"SOAK-FIRST-RENDER %u\n",(unsigned)(ticks()-t0));
                lv_timer_handler();
                if(i==1||i%100==0||i==soak_n)
                    fprintf(stderr,"SOAK %d %lu %lu\n",i,soak_rss_bytes(),soak_count_objs(lv_screen_active()));
            }
            fprintf(stderr,"SOAK-DONE n=%d\n",soak_n);
            running=0;
        }
        SDL_Event e;while(SDL_PollEvent(&e))if(e.type==SDL_QUIT)running=0;
        if(capture&&now-start>400){
            SDL_Surface *surface=SDL_CreateRGBSurfaceWithFormatFrom(canvas,WIDTH,HEIGHT,32,WIDTH*4,SDL_PIXELFORMAT_ARGB8888);
            if(SDL_SaveBMP(surface,argv[2])!=0)return 2;SDL_FreeSurface(surface);running=0;
        }
        SDL_Delay(10);
#else
        usleep(10000);
#endif
    }
    free(previous);cJSON_Delete(state);
#ifndef DESKTOP
    munmap(fbmap,mapsize);close(fbfd);
#else
    SDL_DestroyTexture(texture);SDL_DestroyRenderer(renderer);SDL_DestroyWindow(window);SDL_Quit();
#endif
    return 0;
}
