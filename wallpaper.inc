/* TRAE-G3: 可替换壁纸资源链路（PLAN §7）。Included by service.c AFTER
 * remote_state.inc；host 单测 test_wallpaper.c 通过钩子替换传输/时钟/文件层后
 * include 本文件，测试同一份生产代码。
 *
 * 协议权威表：docs/execution/TRAE-G3/checkpoint.md §1–§3。
 * 红线：帧固定 480×272 XRGB8888 小端 B,G,R,X = 522240 B；上传走独立分块流式
 * 读取（绝不经过 4096 B 的 read_request）；staging→wallpapers→current.json
 * 逐级提交、current.json 是唯一原子切换点；失败不影响当前显示；日志只记
 * id/结果/字节数；词库游标与 state_revision 不被本文件触碰。 */
#ifndef WP_THEMES_ROOT
#ifdef DESKTOP
#define WP_THEMES_ROOT "build/g3-themes"
#else
#define WP_THEMES_ROOT "/mnt/UDISK/panel-v3/themes"
#endif
#endif
#define WP_CURRENT_JSON   WP_THEMES_ROOT "/current.json"
#define WP_DIR_WALLPAPERS WP_THEMES_ROOT "/wallpapers"
#define WP_DIR_STAGING    WP_THEMES_ROOT "/staging"
#define WP_FRAME_BYTES      522240      /* 480*272*4 */
#define WP_UPLOAD_BUDGET_MS 10000       /* 单帧整体传输预算 */

/* ---- 可钩子依赖（host 单测替换；缺省绑定 service.c 同名实现） ---- */
#ifndef WP_NOW
#define WP_NOW() monotonic_ms()
#endif
#ifndef PV2_RECV
#define PV2_RECV(fd,buf,len) recv((fd),(buf),(len),0)
#endif
#ifndef WP_RECV_PEEK
#define WP_RECV_PEEK(fd,buf,cap) recv((fd),(buf),(cap),MSG_PEEK)
#endif
#ifndef WP_AUTH
#define WP_AUTH(req,s) remote_auth((req),(s))
#endif
#ifndef WP_SEND_RESPONSE
#define WP_SEND_RESPONSE(fd,status,type,body) send_response((fd),(status),(type),(body))
#endif
#ifndef WP_SEND_STATE
#define WP_SEND_STATE(fd,s) send_state((fd),(s))
#endif
#ifndef WP_EXPORT_SNAPSHOT
#define WP_EXPORT_SNAPSHOT(s) export_snapshot((s))
#endif
#ifndef WP_PERSIST
#define WP_PERSIST() persist_display(display_page,rotation_enabled,rotation_interval,rotation_mask,screen_brightness_percent)
#endif
#ifndef WP_ATOMIC_JSON
#define WP_ATOMIC_JSON(path,o,durable) atomic_json((path),(o),(durable))
#endif
#ifndef WP_FOPEN
#define WP_FOPEN(path,mode) fopen((path),(mode))
#endif
#ifndef WP_FWRITE
#define WP_FWRITE(p,sz,n,f) fwrite((p),(sz),(n),(f))
#endif
#ifndef WP_FSYNC
#ifdef _WIN32
#define WP_FSYNC(fd) _commit(fd)
#else
#define WP_FSYNC(fd) fsync(fd)
#endif
#endif
#ifndef WP_MKDIR
#ifdef _WIN32
#define WP_MKDIR(p) _mkdir(p)
#else
#define WP_MKDIR(p) mkdir((p),0755)
#endif
#endif
#ifdef _WIN32
#ifndef WP_STRNCASECMP
#define WP_STRNCASECMP _strnicmp
#endif
#include <direct.h>
#include <io.h>
#else
#ifndef WP_STRNCASECMP
#define WP_STRNCASECMP strncasecmp
#endif
#include <unistd.h>
#endif
#include <sys/stat.h>
#include <dirent.h>
#include <time.h>

/* ---- SHA-256（FIPS 180-4 紧凑实现，仅用于帧摘要校验） ---- */
typedef struct { uint32_t h[8]; uint64_t len; unsigned char buf[64]; size_t fill; } wp_sha256;
static const uint32_t wp_sha_k[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
static uint32_t wp_ror(uint32_t x,int n){return (x>>n)|(x<<(32-n));}
static void wp_sha_block(wp_sha256 *ctx,const unsigned char *p){
    uint32_t w[64];
    uint32_t a=ctx->h[0],b=ctx->h[1],c=ctx->h[2],d=ctx->h[3];
    uint32_t e=ctx->h[4],f=ctx->h[5],g=ctx->h[6],h=ctx->h[7];
    for(int i=0;i<16;i++)
        w[i]=((uint32_t)p[i*4]<<24)|((uint32_t)p[i*4+1]<<16)|((uint32_t)p[i*4+2]<<8)|p[i*4+3];
    for(int i=16;i<64;i++){
        uint32_t s0=wp_ror(w[i-15],7)^wp_ror(w[i-15],18)^(w[i-15]>>3);
        uint32_t s1=wp_ror(w[i-2],17)^wp_ror(w[i-2],19)^(w[i-2]>>10);
        w[i]=w[i-16]+s0+w[i-7]+s1;
    }
    for(int i=0;i<64;i++){
        uint32_t t1=h+(wp_ror(e,6)^wp_ror(e,11)^wp_ror(e,25))+((e&f)^((~e)&g))+wp_sha_k[i]+w[i];
        uint32_t t2=(wp_ror(a,2)^wp_ror(a,13)^wp_ror(a,22))+((a&b)^(a&c)^(b&c));
        h=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;
    }
    ctx->h[0]+=a;ctx->h[1]+=b;ctx->h[2]+=c;ctx->h[3]+=d;
    ctx->h[4]+=e;ctx->h[5]+=f;ctx->h[6]+=g;ctx->h[7]+=h;
}
static void wp_sha_init(wp_sha256 *c){
    static const uint32_t iv[8]={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                                 0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    memcpy(c->h,iv,sizeof iv);c->len=0;c->fill=0;
}
static void wp_sha_update(wp_sha256 *c,const void *data,size_t n){
    const unsigned char *p=data;c->len+=n;
    while(n){
        size_t take=64-c->fill;if(take>n)take=n;
        memcpy(c->buf+c->fill,p,take);c->fill+=take;p+=take;n-=take;
        if(c->fill==64){wp_sha_block(c,c->buf);c->fill=0;}
    }
}
static void wp_sha_final(wp_sha256 *c,unsigned char out[32]){
    uint64_t bits=c->len*8;
    unsigned char pad=0x80,z=0,lenb[8];
    wp_sha_update(c,&pad,1);
    while(c->fill!=56)wp_sha_update(c,&z,1);
    for(int i=0;i<8;i++)lenb[i]=(unsigned char)(bits>>(56-8*i));
    wp_sha_update(c,lenb,8);
    for(int i=0;i<8;i++){
        out[i*4]=(unsigned char)(c->h[i]>>24);out[i*4+1]=(unsigned char)(c->h[i]>>16);
        out[i*4+2]=(unsigned char)(c->h[i]>>8);out[i*4+3]=(unsigned char)c->h[i];
    }
}
static void wp_sha_hex(wp_sha256 *c,char out[65]){
    static const char *hx="0123456789abcdef";
    unsigned char d[32];wp_sha_final(c,d);
    for(int i=0;i<32;i++){out[i*2]=hx[d[i]>>4];out[i*2+1]=hx[d[i]&15];}
    out[64]=0;
}

/* ---- 状态（进程内单份；单线程服务无并发） ---- */
static char wp_id[48]="default";
static long wp_revision=1;
static int wp_preset=1;
static char wp_digest_clear[65],wp_digest_frost[65];
static unsigned wp_seq;
static struct { char id[48];char digest_clear[65],digest_frost[65];int preset; } wp_session;

static const char *wp_current_id(void){return wp_id;}
static long wp_current_revision(void){return wp_revision;}
static int wp_current_preset(void){return wp_preset;}
/* load_remote_config 只回填 revision 下限（current.json 是文件权威，display.json
 * 只镜像给手机确认；两处取 max 保证跨重启单调） */
static void wp_note_display_revision(long rev){if(rev>wp_revision)wp_revision=rev;}

static int wp_hex64(const char *s){
    size_t n=s?strlen(s):0;
    if(n!=64)return 0;
    for(size_t i=0;i<n;i++){
        char c=s[i];
        if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')))return 0;
    }
    return 1;
}
static int wp_valid_id(const char *s){          /* "wp-"+小写hex，防路径穿越 */
    size_t n=s?strlen(s):0;
    if(n<6||n>40)return 0;
    for(size_t i=3;i<n;i++){
        char c=s[i];
        if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')))return 0;
    }
    return s[0]=='w'&&s[1]=='p'&&s[2]=='-';
}
static long wp_file_size(const char *path){
    FILE *f=WP_FOPEN(path,"rb");if(!f)return -1;
    if(fseek(f,0,SEEK_END)!=0){fclose(f);return -1;}
    long n=ftell(f);fclose(f);
    return n;
}
static int wp_rename_replace(const char *src,const char *dst){
#ifdef _WIN32
    unlink(dst);                                /* 仅 host 测试环境；板卡 POSIX rename 原子替换 */
#endif
    return rename(src,dst);
}
static void wp_staging_clean(void){             /* 会话不跨重启/换会话：staging 清空 */
    DIR *d=opendir(WP_DIR_STAGING);if(!d)return;
    struct dirent *e;char p[256];
    while((e=readdir(d))){
        if(e->d_name[0]=='.')continue;
        snprintf(p,sizeof p,WP_DIR_STAGING "/%s",e->d_name);
        unlink(p);
    }
    closedir(d);
}
static void wp_gc(void){                        /* 只留 current id 与 previous 固定槽 */
    DIR *d=opendir(WP_DIR_WALLPAPERS);if(!d)return;
    struct dirent *e;char p[256];
    size_t idn=strlen(wp_id);
    while((e=readdir(d))){
        const char *nm=e->d_name;
        if(nm[0]=='.')continue;
        if(!strncmp(nm,"previous.",9))continue;
        if(!strncmp(nm,wp_id,idn)&&nm[idn]=='.')continue;
        snprintf(p,sizeof p,WP_DIR_WALLPAPERS "/%s",nm);
        unlink(p);
    }
    closedir(d);
}

static void wp_ok_json(int fd,const char *body){
    WP_SEND_RESPONSE(fd,"200 OK","application/json; charset=utf-8",body);
}
static void wp_err(int fd,const char *status,const char *msg){
    char body[160];
    snprintf(body,sizeof body,"{\"error\":\"%s\"}",msg);
    WP_SEND_RESPONSE(fd,status,"application/json; charset=utf-8",body);
}

/* ---- 启动：建目录、清 staging、读 current.json、校验文件存在 ---- */
static void wallpaper_boot_init(void){
    WP_MKDIR(WP_THEMES_ROOT);
    WP_MKDIR(WP_DIR_WALLPAPERS);
    WP_MKDIR(WP_DIR_STAGING);
    wp_staging_clean();
    FILE *f=WP_FOPEN(WP_CURRENT_JSON,"rb");
    if(f){
        char buf[512];
        size_t n=fread(buf,1,sizeof(buf)-1,f);
        fclose(f);buf[n]=0;
        cJSON *o=cJSON_ParseWithOpts(buf,NULL,1);
        if(o){
            const char *id=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o,"wallpaper_id"));
            int rev=0,preset=0;
            if(id&&wp_valid_id(id)&&integer(o,"revision",1,1<<30,&rev)&&integer(o,"preset",0,2,&preset)){
                snprintf(wp_id,sizeof wp_id,"%s",id);
                wp_preset=preset;
                if(rev>wp_revision)wp_revision=rev;
                const char *dc=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o,"digest_clear"));
                const char *df=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o,"digest_frost"));
                if(wp_hex64(dc))snprintf(wp_digest_clear,sizeof wp_digest_clear,"%s",dc);
                if(wp_hex64(df))snprintf(wp_digest_frost,sizeof wp_digest_frost,"%s",df);
            }
            cJSON_Delete(o);
        }
    }
    if(strcmp(wp_id,"default")){                /* 指向的自选文件缺失→回默认 */
        char p[160];
        snprintf(p,sizeof p,WP_DIR_WALLPAPERS "/%s.clear",wp_id);
        FILE *t=WP_FOPEN(p,"rb");
        if(!t)snprintf(wp_id,sizeof wp_id,"default");
        else fclose(t);
    }
    if(!strcmp(wp_id,"default")){
        wp_preset=1;wp_digest_clear[0]=0;wp_digest_frost[0]=0;
    }
    wp_gc();
    fprintf(stderr,"wallpaper: boot id=%s revision=%ld preset=%d\n",wp_id,wp_revision,wp_preset);
}

/* ---- 2.1 登记会话 ---- */
static void wallpaper_session_handler(int fd,const char *req){
    const char *body=strstr(req,"\r\n\r\n");
    cJSON *o=body?cJSON_ParseWithOpts(body+4,NULL,1):NULL;
    const char *dcv=cJSON_IsObject(o)?cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o,"digest_clear")):NULL;
    const char *dfv=cJSON_IsObject(o)?cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o,"digest_frost")):NULL;
    int preset=0;
    if(!o||!wp_hex64(dcv)||!wp_hex64(dfv)||!integer(o,"preset",0,2,&preset)){
        cJSON_Delete(o);wp_err(fd,"400 Bad Request","invalid parameters");return;
    }
    wp_staging_clean();                         /* 新会话替换旧会话 */
    memset(&wp_session,0,sizeof wp_session);
    snprintf(wp_session.id,sizeof wp_session.id,"wp-%08lx%02x",
             (unsigned long)time(NULL)&0xfffffffful,wp_seq++&0xff);
    snprintf(wp_session.digest_clear,sizeof wp_session.digest_clear,"%s",dcv);
    snprintf(wp_session.digest_frost,sizeof wp_session.digest_frost,"%s",dfv);
    wp_session.preset=preset;
    char out[160];
    snprintf(out,sizeof out,"{\"ok\":true,\"wallpaper_id\":\"%s\",\"preset\":%d}",
             wp_session.id,wp_session.preset);
    cJSON_Delete(o);
    fprintf(stderr,"wallpaper: session %s preset=%d\n",wp_session.id,preset);
    wp_ok_json(fd,out);
}

/* ---- 2.3 激活：staging 完整→逐级提交→current.json 原子切指针 ---- */
static void wallpaper_activate_handler(int fd,const char *req,home_state_t *s){
    if(!wp_session.id[0]){wp_err(fd,"404 Not Found","no wallpaper session");return;}
    const char *body=strstr(req,"\r\n\r\n");
    cJSON *o=body?cJSON_ParseWithOpts(body+4,NULL,1):NULL;
    const char *id=cJSON_IsObject(o)?cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o,"wallpaper_id")):NULL;
    if(!o||!id||strcmp(id,wp_session.id)){
        cJSON_Delete(o);wp_err(fd,"404 Not Found","unknown wallpaper id");return;
    }
    cJSON_Delete(o);
    char pc[160],pf[160];
    snprintf(pc,sizeof pc,WP_DIR_STAGING "/%s.clear",wp_session.id);
    snprintf(pf,sizeof pf,WP_DIR_STAGING "/%s.frost",wp_session.id);
    long szc=wp_file_size(pc),szf=wp_file_size(pf);
    if(szc!=WP_FRAME_BYTES||szf!=WP_FRAME_BYTES){
        wp_err(fd,"422 Unprocessable Entity","staging incomplete");return;
    }
    char a[200],b[200];
    if(strcmp(wp_id,"default")){                /* 旧当前→previous 槽（default 无文件） */
        snprintf(a,sizeof a,WP_DIR_WALLPAPERS "/%s.clear",wp_id);
        snprintf(b,sizeof b,WP_DIR_WALLPAPERS "/previous.clear");
        unlink(b);rename(a,b);
        snprintf(a,sizeof a,WP_DIR_WALLPAPERS "/%s.frost",wp_id);
        snprintf(b,sizeof b,WP_DIR_WALLPAPERS "/previous.frost");
        unlink(b);rename(a,b);
    }
    snprintf(a,sizeof a,WP_DIR_WALLPAPERS "/%s.clear",wp_session.id);
    if(rename(pc,a)!=0){wp_err(fd,"500 Internal Server Error","cannot commit wallpaper");return;}
    snprintf(a,sizeof a,WP_DIR_WALLPAPERS "/%s.frost",wp_session.id);
    if(rename(pf,a)!=0){wp_err(fd,"500 Internal Server Error","cannot commit wallpaper");return;}
    wp_revision++;
    cJSON *cur=cJSON_CreateObject();
    if(cur){
        cJSON_AddStringToObject(cur,"wallpaper_id",wp_session.id);
        cJSON_AddNumberToObject(cur,"revision",(double)wp_revision);
        cJSON_AddNumberToObject(cur,"preset",wp_session.preset);
        cJSON_AddStringToObject(cur,"digest_clear",wp_session.digest_clear);
        cJSON_AddStringToObject(cur,"digest_frost",wp_session.digest_frost);
        cJSON_AddNumberToObject(cur,"bytes_clear",WP_FRAME_BYTES);
        cJSON_AddNumberToObject(cur,"bytes_frost",WP_FRAME_BYTES);
    }
    if(!cur||WP_ATOMIC_JSON(WP_CURRENT_JSON,cur,1)!=0){   /* 原子切指针，唯一提交点 */
        cJSON_Delete(cur);
        wp_err(fd,"500 Internal Server Error","cannot save wallpaper pointer");return;
    }
    cJSON_Delete(cur);
    snprintf(wp_id,sizeof wp_id,"%s",wp_session.id);
    wp_preset=wp_session.preset;
    snprintf(wp_digest_clear,sizeof wp_digest_clear,"%s",wp_session.digest_clear);
    snprintf(wp_digest_frost,sizeof wp_digest_frost,"%s",wp_session.digest_frost);
    memset(&wp_session,0,sizeof wp_session);    /* 会话一次性 */
    if(WP_PERSIST()!=0){
        wp_err(fd,"500 Internal Server Error","cannot save settings");return;
    }
    wp_gc();
    WP_EXPORT_SNAPSHOT(s);
    fprintf(stderr,"wallpaper: activate id=%s revision=%ld bytes=%ld\n",
            wp_id,wp_revision,(long)WP_FRAME_BYTES*2);
    WP_SEND_STATE(fd,s);
}

/* ---- 2.4 恢复默认：current.json 先删（切换点），文件移入 previous ---- */
static void wallpaper_reset_handler(int fd,const char *req,home_state_t *s){
    (void)req;
    wp_staging_clean();
    memset(&wp_session,0,sizeof wp_session);
    if(strcmp(wp_id,"default")){
        char a[200],b[200];
        snprintf(a,sizeof a,WP_DIR_WALLPAPERS "/%s.clear",wp_id);
        snprintf(b,sizeof b,WP_DIR_WALLPAPERS "/previous.clear");
        unlink(b);rename(a,b);
        snprintf(a,sizeof a,WP_DIR_WALLPAPERS "/%s.frost",wp_id);
        snprintf(b,sizeof b,WP_DIR_WALLPAPERS "/previous.frost");
        unlink(b);rename(a,b);
    }
    unlink(WP_CURRENT_JSON);
    snprintf(wp_id,sizeof wp_id,"default");
    wp_preset=1;wp_digest_clear[0]=0;wp_digest_frost[0]=0;
    wp_revision++;
    if(WP_PERSIST()!=0){
        wp_err(fd,"500 Internal Server Error","cannot save settings");return;
    }
    wp_gc();
    WP_EXPORT_SNAPSHOT(s);
    fprintf(stderr,"wallpaper: reset revision=%ld\n",wp_revision);
    WP_SEND_STATE(fd,s);
}

static int wallpaper_route(int fd,const char *method,const char *path,
                           const char *req,home_state_t *s){
    (void)method;                               /* handle_remote 已保证 POST+PIN */
    const char *rest=path+sizeof("/api/v1/theme/wallpaper/")-1;
    if(!strcmp(rest,"session")){wallpaper_session_handler(fd,req);return 1;}
    if(!strcmp(rest,"activate")){wallpaper_activate_handler(fd,req,s);return 1;}
    if(!strcmp(rest,"reset")){wallpaper_reset_handler(fd,req,s);return 1;}
    wp_err(fd,"404 Not Found","unknown endpoint");
    return 1;
}

/* ---- 2.2 上传（handle_client 在 read_request 之前分派） ----
 * 头部预读仅 MSG_PEEK 不消费 socket：返回 1=是上传路由（头部已排空到 hdr），
 * 0=不是（socket 原样，read_request 正常读），-1=连接中断（上传放弃）。 */
static int wallpaper_peek_upload(int fd,char *hdr,size_t cap){
    int64_t deadline=WP_NOW()+1200;
    hdr[0]=0;
    while(WP_NOW()<deadline){
        ssize_t n=WP_RECV_PEEK(fd,hdr,cap-1);
        if(n<=0)return 0;                       /* 对端关闭/暂无数据：交回正常路径 */
        hdr[n]=0;
        if(strstr(hdr,"\r\n\r\n"))break;
    }
    char *end=strstr(hdr,"\r\n\r\n");
    if(!end)return 0;                           /* 头部不完整/超大：交回 read_request */
    char m[8],p[512];
    if(sscanf(hdr,"%7s %511s",m,p)!=2)return 0;
    if(strcmp(m,"POST")||strncmp(p,"/api/v1/theme/wallpaper/upload",30))return 0;
    size_t hlen=(size_t)(end-hdr)+4;
    size_t got=0;
    while(got<hlen){                            /* 排空已 peek 到的头部字节 */
        ssize_t n=PV2_RECV(fd,hdr+got,hlen-got);
        if(n<=0)return -1;
        got+=(size_t)n;
    }
    hdr[got]=0;
    return 1;
}
static void wallpaper_upload_stream(int fd,const char *hdr,home_state_t *s){
    char reqpath[512];
    if(sscanf(hdr,"%*s %511s",reqpath)!=1){wp_err(fd,"400 Bad Request","bad request");return;}
    if(!WP_AUTH(hdr,s)){wp_err(fd,"403 Forbidden","invalid control pin");return;}
    long cl=-1;int seen=0,te=0,ct_ok=0;
    const char *he=strstr(hdr,"\r\n\r\n");
    for(const char *p=strstr(hdr,"\r\n")+2;p&&p<he;){
        const char *e=strstr(p,"\r\n");
        if(!e||e==p)break;
        if(!WP_STRNCASECMP(p,"Content-Length:",15)){
            char *stop;long x=strtol(p+15,&stop,10);
            while(stop<e&&(*stop==' '||*stop=='\t'))stop++;
            if(++seen>1||stop!=e||x<0){wp_err(fd,"400 Bad Request","malformed headers");return;}
            cl=x;
        }else if(!WP_STRNCASECMP(p,"Transfer-Encoding:",18)){
            te=1;
        }else if(!WP_STRNCASECMP(p,"Content-Type:",13)){
            const char *v=p+13;
            while(v<e&&(*v==' '||*v=='\t'))v++;
            if((size_t)(e-v)>=strlen("application/octet-stream")&&
               !WP_STRNCASECMP(v,"application/octet-stream",strlen("application/octet-stream")))ct_ok=1;
        }
        p=e+2;
    }
    if(te){wp_err(fd,"400 Bad Request","chunked not supported");return;}
    if(cl!=WP_FRAME_BYTES){wp_err(fd,"413 Payload Too Large","frame must be 522240 bytes");return;}
    if(!ct_ok){wp_err(fd,"415 Unsupported Media Type","application/octet-stream required");return;}
    char frame[8],id[48];
    if(query_value(reqpath,"frame",frame,sizeof frame)||query_value(reqpath,"id",id,sizeof id)){
        wp_err(fd,"400 Bad Request","invalid parameters");return;
    }
    if(strcmp(frame,"clear")&&strcmp(frame,"frost")){
        wp_err(fd,"400 Bad Request","invalid frame");return;
    }
    if(!wp_session.id[0]||strcmp(id,wp_session.id)){
        wp_err(fd,"404 Not Found","no wallpaper session");return;
    }
    const char *digest=!strcmp(frame,"clear")?wp_session.digest_clear:wp_session.digest_frost;
    if(!wp_hex64(digest)){wp_err(fd,"400 Bad Request","invalid parameters");return;}

    char path[200];
    snprintf(path,sizeof path,WP_DIR_STAGING "/%s.%s.part",wp_session.id,frame);
    FILE *out=WP_FOPEN(path,"wb");
    if(!out){wp_err(fd,"500 Internal Server Error","storage failure");return;}
    wp_sha256 sha;wp_sha_init(&sha);
    unsigned char chunk[16384];
    long got=0;
    int64_t deadline=WP_NOW()+WP_UPLOAD_BUDGET_MS;
    int ioerr=0;                                /* 1=读断 2=写失败 */
    while(got<WP_FRAME_BYTES){
        ssize_t n=PV2_RECV(fd,chunk,sizeof chunk);
        if(n<=0){ioerr=1;break;}                /* 中断/超时：当前显示不受影响 */
        if(WP_NOW()>deadline){ioerr=1;break;}
        size_t take=(size_t)n;
        if(take>(size_t)(WP_FRAME_BYTES-got))take=(size_t)(WP_FRAME_BYTES-got);
        wp_sha_update(&sha,chunk,take);
        if(WP_FWRITE(chunk,1,take,out)!=take){ioerr=2;break;}   /* 磁盘满等 */
        got+=(long)take;
    }
    if(ioerr||got!=WP_FRAME_BYTES){
        fclose(out);unlink(path);
        fprintf(stderr,"wallpaper: upload id=%s frame=%s FAILED bytes=%ld\n",
                wp_session.id,frame,got);
        wp_err(fd,ioerr==2?"500 Internal Server Error":"408 Request Timeout",
               ioerr==2?"storage failure":"upload interrupted");
        return;
    }
    char hex[65];wp_sha_hex(&sha,hex);
    if(fflush(out)!=0||WP_FSYNC(fileno(out))!=0||fclose(out)!=0){
        unlink(path);
        fprintf(stderr,"wallpaper: upload id=%s frame=%s FAILED bytes=%ld\n",
                wp_session.id,frame,got);
        wp_err(fd,"500 Internal Server Error","storage failure");
        return;
    }
    if(strcmp(hex,digest)){                     /* 内容摘要不符→删帧拒绝 */
        unlink(path);
        fprintf(stderr,"wallpaper: upload id=%s frame=%s DIGEST-MISMATCH bytes=%ld\n",
                wp_session.id,frame,got);
        wp_err(fd,"422 Unprocessable Entity","digest mismatch");
        return;
    }
    char final[200];
    snprintf(final,sizeof final,WP_DIR_STAGING "/%s.%s",wp_session.id,frame);
    if(wp_rename_replace(path,final)!=0){
        wp_err(fd,"500 Internal Server Error","cannot commit staging file");return;
    }
    fprintf(stderr,"wallpaper: upload id=%s frame=%s OK bytes=%ld\n",
            wp_session.id,frame,got);
    char body[160];
    snprintf(body,sizeof body,"{\"ok\":true,\"wallpaper_id\":\"%s\",\"frame\":\"%s\",\"bytes\":%ld}",
             wp_session.id,frame,got);
    wp_ok_json(fd,body);
}
