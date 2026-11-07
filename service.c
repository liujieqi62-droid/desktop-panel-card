#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#include "cJSON.h"
#include "panel_v2.h"
#include <strings.h>

typedef struct {
    uint8_t *pixels;
    size_t size;
    int fd;
    struct fb_fix_screeninfo fix;
    struct fb_var_screeninfo var;
} framebuffer_t;

typedef struct {
    int fd;
    int raw_sysfs;
    int reg_dump_fd;
    int reg_write_fd;
    int raw_x;
    int raw_y;
    int down;
    int reported_down;
    int ignore_first;
    int have_sample;
    struct input_absinfo x_info;
    struct input_absinfo y_info;
} touch_input_t;

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} color_t;

typedef enum {
    CURTAIN_CLOSED,
    CURTAIN_OPEN,
    CURTAIN_STOPPED
} curtain_state_t;

typedef enum {
    SCENE_HOME,
    SCENE_AWAY,
    SCENE_SLEEP
} scene_state_t;

typedef enum {
    VIEW_DASHBOARD,
    VIEW_LIGHTS,
    VIEW_CURTAINS,
    VIEW_SCENES
} ui_view_t;

typedef struct {
    uint8_t light_mask;
    curtain_state_t curtain;
    scene_state_t scene;
    char ip[INET_ADDRSTRLEN];
    char light_names[4][32];
    char curtain_device_name[32];
    int gpio_requested;
    int gpio_enabled;
    int active_low;
    int curtain_timeout_seconds;
    char control_pin[13];
    int auth_enabled;       /* M9A: 密码鉴权开关，1=启用(默认)，0=关闭 */
    char panel_title[32];
    char location[32];
    char weather[24];
    char temperature[16];
    char humidity[16];
    char server_name[24];
    char server_address[64];
    char server_status[16];
    char nas_name[24];
    char nas_address[64];
    char nas_status[16];
    char notice[64];
    char forecast_today[40];
    char forecast_tomorrow[40];
    char forecast_after[40];
    char weather_updated[24];
    long uptime_seconds;
    int memory_used_percent;
    int storage_used_percent;
    long memory_used_kb;
    long memory_total_kb;
    unsigned long long storage_available_mb;
    int wifi_signal_dbm;
    char load_average[16];
    char system_alert[48];
    char indoor_temperature[16];
    char indoor_humidity[16];
    char air_quality[16];
    int illuminance_lux;
    char sensor_updated[24];
    int sensors_valid;
    int sensor_stale;
} home_state_t;

typedef struct {
    int requested;
    int enabled;
    int active_low;
    int value_fd[6];
} output_backend_t;

typedef struct {
    unsigned samples;
    unsigned downs;
    unsigned ups;
    unsigned taps;
    unsigned errors;
    uint32_t last_status;
    int raw_x;
    int raw_y;
    int screen_x;
    int screen_y;
    int mode;
} touch_debug_t;

#define HARDWARE_BUTTON_COUNT 5
#define HARDWARE_BUTTON_DEBOUNCE_MS 80
#define DISPLAY_PAGE_COUNT 4
typedef struct {
    int enabled;
    int led_enabled;
    int value_fd[HARDWARE_BUTTON_COUNT];
    int raw_value[HARDWARE_BUTTON_COUNT];
    int stable_value[HARDWARE_BUTTON_COUNT];
    int armed[HARDWARE_BUTTON_COUNT];
    int64_t changed_ms[HARDWARE_BUTTON_COUNT];
    int64_t cooldown_until_ms[HARDWARE_BUTTON_COUNT];
    int led_fd;
    int led_value;
    int64_t next_led_toggle_ms;
    int64_t input_ready_ms;
} hardware_input_t;

typedef struct {
    const char *key;
    const char *message;
    int active;
    int acknowledged;
    time_t since;
} alert_state_t;

#define LIGHT_COUNT 4
#define ALL_LIGHTS_MASK 0x0f
#define STATE_PATH "/mnt/UDISK/smart-panel-state.txt"
#define STATE_TMP_PATH "/mnt/UDISK/smart-panel-state.tmp"
#define CONFIG_PATH "/mnt/UDISK/smart-panel.conf"
#define DEVICE_CONFIG_PATH "/mnt/UDISK/smart-panel-devices.conf"
#define DEVICE_CONFIG_TMP_PATH "/mnt/UDISK/smart-panel-devices.tmp"
#define INFO_CONFIG_PATH "/mnt/UDISK/smart-panel-info.conf"
#define INFO_CONFIG_TMP_PATH "/mnt/UDISK/smart-panel-info.tmp"
#define DISPLAY_CONFIG_PATH "/mnt/UDISK/panel-v3/display-legacy.conf"
#define SENSOR_CONFIG_PATH "/mnt/UDISK/smart-panel-sensors.conf"
#define SENSOR_CONFIG_TMP_PATH "/mnt/UDISK/smart-panel-sensors.tmp"
#define EVENT_LOG_PATH "/mnt/UDISK/smart-panel-events.log"
#define APK_PATH "/mnt/UDISK/T113-S3-Control.apk"
#define APP_VERSION "3.1.0-lvgl"
#define DISCOVERY_PORT 8081
#define DISPLAY_ONLY 1
#define REG_DUMP_PATH "/sys/devices/virtual/misc/sunxi-reg/rw/dump"
#define REG_WRITE_PATH "/sys/devices/virtual/misc/sunxi-reg/rw/write"

#define CCU_TPADC_CLK_REG 0x02001c50u
#define CCU_TPADC_BGR_REG 0x02001c5cu
#define TP_CTRL0_REG      0x02009c00u
#define TP_CTRL1_REG      0x02009c04u
#define TP_CTRL2_REG      0x02009c08u
#define TP_CTRL3_REG      0x02009c0cu
#define TP_INT_FIFOC_REG  0x02009c10u
#define TP_INT_FIFOS_REG  0x02009c14u
#define TP_DATA_REG       0x02009c24u

#define TP_FIFO_DATA_PENDING (1u << 16)
#define TP_UP_PENDING        (1u << 1)
#define TP_DOWN_PENDING      (1u << 0)
#define TP_STATUS_CLEAR_MASK 0x00070003u
#define TP_FIFO_CONTROL      0x00070193u

static ui_view_t current_view = VIEW_DASHBOARD;
static touch_debug_t touch_debug;
static output_backend_t output_backend;
static unsigned output_gpios[6] = {138, 137, 136, 135, 134, 133};
static int64_t curtain_deadline_ms;
static int64_t sleep_timer_deadline_ms;
static time_t last_display_minute = (time_t)-1;
static int64_t next_device_check_ms;
static int64_t next_system_refresh_ms;
static int64_t next_page_switch_ms;
static int service_port = 8080;
/* M5-B: default rotation = 总览/天气/单词/简报 (bits 0,1,4,5 = 51, 实施方案 39
 * 行); system stays hand-open, devices (bit 3) is never in the default set. */
static int rotation_enabled = 1, rotation_interval = 12, rotation_mask = 51;
/* M5-B: briefing-page paging state, shared with the v1 display-page handler in
 * remote_state.inc (phone-side screen switch) and with briefing_snapshot. */
static int briefing_part, briefing_pinned, briefing_pages;
static char briefing_day[16];
static int64_t briefing_advance_ms;
/* N6: timetable/plans-page paging state, shared with the v1 display-page
 * handler in remote_state.inc and with the snapshot producers. */
static int timetable_part, timetable_pinned, timetable_pages;
static char timetable_day[16];
static int64_t timetable_advance_ms;
static int plans_part, plans_pinned, plans_pages;
static char plans_day[16];
static int64_t plans_advance_ms;
static unsigned long state_revision = 1;
static int display_page;
static int selected_light;
static int screen_brightness_percent = 100;
static int auto_brightness_enabled;
static int64_t brightness_manual_until_ms;
static int64_t next_sensor_refresh_ms;
static hardware_input_t hardware_input;
static int evdev_keys_active;
static int64_t last_evdev_key_ms;
static unsigned short last_evdev_key_code;
static char last_hardware_key[16] = "无";
static char ui_feedback[48] = "";
static int64_t ui_feedback_until_ms;

static const char *ui_view_name(void) {
    if (current_view == VIEW_LIGHTS) return "lights";
    if (current_view == VIEW_CURTAINS) return "curtains";
    if (current_view == VIEW_SCENES) return "scenes";
    return "dashboard";
}
static alert_state_t alert_states[] = {
    {"network", "网络连接中断", 0, 0, 0},
    {"server", "服务器离线", 0, 0, 0},
    {"nas", "NAS离线", 0, 0, 0},
    {"storage", "存储空间不足", 0, 0, 0},
    {"memory", "内存占用过高", 0, 0, 0},
    {"wifi", "Wi-Fi信号较弱", 0, 0, 0}
};
#define ALERT_COUNT ((int)(sizeof(alert_states) / sizeof(alert_states[0])))
static const unsigned HARDWARE_BUTTON_GPIOS[HARDWARE_BUTTON_COUNT] = {
    137, 136, 135, 37, 138
};
static const unsigned short HARDWARE_BUTTON_CODES[HARDWARE_BUTTON_COUNT] = {
    KEY_RIGHT, KEY_LEFT, KEY_UP, KEY_DOWN, KEY_BACK
};
#define STATUS_LED_GPIO 134

static int64_t monotonic_ms(void);
static void collect_system_metrics(home_state_t *state);

static const color_t C_BG = {13, 20, 35};
static const color_t C_PANEL = {25, 35, 55};
static const color_t C_PANEL_2 = {32, 45, 68};
static const color_t C_TEXT = {236, 242, 255};
static const color_t C_MUTED = {136, 153, 181};
static const color_t C_BLUE = {63, 132, 255};
static const color_t C_GREEN = {44, 202, 140};
static const color_t C_YELLOW = {255, 190, 70};
static const color_t C_PURPLE = {154, 105, 255};

static void update_ip(home_state_t *state) {
    struct ifreq request;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    snprintf(state->ip, sizeof(state->ip), "OFFLINE");
    if (fd < 0) return;
    memset(&request, 0, sizeof(request));
    snprintf(request.ifr_name, sizeof(request.ifr_name), "wlan0");
    if (ioctl(fd, SIOCGIFADDR, &request) == 0) {
        const struct sockaddr_in *address = (const struct sockaddr_in *)&request.ifr_addr;
        inet_ntop(AF_INET, &address->sin_addr, state->ip, sizeof(state->ip));
    }
    close(fd);
}

static int save_state(const home_state_t *state) {
    FILE *file = fopen(STATE_TMP_PATH, "w");
    if (!file) return -1;
    fprintf(file, "version=1\nlight_mask=%u\ncurtain=%d\nscene=%d\n",
            (unsigned)(state->light_mask & ALL_LIGHTS_MASK),
            (int)state->curtain, (int)state->scene);
    fflush(file);
    fsync(fileno(file));
    if (fclose(file) != 0 || rename(STATE_TMP_PATH, STATE_PATH) != 0) return -1;
    return 0;
}

static void load_state(home_state_t *state) {
    FILE *file = fopen(STATE_PATH, "r");
    if (!file) return;
    char line[64];
    while (fgets(line, sizeof(line), file)) {
        unsigned value;
        if (sscanf(line, "light_mask=%u", &value) == 1) {
            state->light_mask = (uint8_t)(value & ALL_LIGHTS_MASK);
        } else if (sscanf(line, "curtain=%u", &value) == 1 && value <= CURTAIN_STOPPED) {
            state->curtain = (curtain_state_t)value;
        } else if (sscanf(line, "scene=%u", &value) == 1 && value <= SCENE_SLEEP) {
            state->scene = (scene_state_t)value;
        }
    }
    fclose(file);
}

static void set_default_device_names(home_state_t *state) {
    static const char *defaults[LIGHT_COUNT] = {"客厅灯", "餐厅灯", "卧室灯", "走廊灯"};
    for (int index = 0; index < LIGHT_COUNT; ++index) {
        snprintf(state->light_names[index], sizeof(state->light_names[index]), "%s", defaults[index]);
    }
    snprintf(state->curtain_device_name, sizeof(state->curtain_device_name), "主窗帘");
}

static int valid_device_name(const char *name) {
    size_t length = strlen(name);
    if (length == 0 || length >= 32) return 0;
    for (size_t index = 0; index < length; ++index) {
        unsigned char c = (unsigned char)name[index];
        if (c < 0x20 || c == '"' || c == '\\') return 0;
    }
    return 1;
}

static void load_device_names(home_state_t *state) {
    FILE *file = fopen(DEVICE_CONFIG_PATH, "r");
    if (!file) return;
    char line[96];
    while (fgets(line, sizeof(line), file)) {
        char value[32] = {0};
        int slot = -1;
        if (sscanf(line, "light1=%31[^\r\n]", value) == 1) slot = 0;
        else if (sscanf(line, "light2=%31[^\r\n]", value) == 1) slot = 1;
        else if (sscanf(line, "light3=%31[^\r\n]", value) == 1) slot = 2;
        else if (sscanf(line, "light4=%31[^\r\n]", value) == 1) slot = 3;
        else if (sscanf(line, "curtain=%31[^\r\n]", value) == 1 && valid_device_name(value)) {
            snprintf(state->curtain_device_name, sizeof(state->curtain_device_name), "%s", value);
        }
        if (slot >= 0 && valid_device_name(value)) {
            snprintf(state->light_names[slot], sizeof(state->light_names[slot]), "%s", value);
        }
    }
    fclose(file);
}

static int save_device_names(const home_state_t *state) {
    FILE *file = fopen(DEVICE_CONFIG_TMP_PATH, "w");
    if (!file) return -1;
    for (int index = 0; index < LIGHT_COUNT; ++index) {
        fprintf(file, "light%d=%s\n", index + 1, state->light_names[index]);
    }
    fprintf(file, "curtain=%s\n", state->curtain_device_name);
    fflush(file);
    fsync(fileno(file));
    if (fclose(file) != 0 || rename(DEVICE_CONFIG_TMP_PATH, DEVICE_CONFIG_PATH) != 0) return -1;
    return 0;
}

static int valid_info_value(const char *value, size_t capacity) {
    size_t length = strlen(value);
    if (length == 0 || length >= capacity) return 0;
    for (size_t index = 0; index < length; ++index) {
        unsigned char c = (unsigned char)value[index];
        if (c < 0x20 || c == '"' || c == '\\' || c == '=') return 0;
    }
    return 1;
}

static void set_default_info(home_state_t *state) {
    snprintf(state->panel_title, sizeof(state->panel_title), "信息状态屏");
    snprintf(state->location, sizeof(state->location), "本地");
    snprintf(state->weather, sizeof(state->weather), "待更新");
    snprintf(state->temperature, sizeof(state->temperature), "--");
    snprintf(state->humidity, sizeof(state->humidity), "--");
    snprintf(state->server_name, sizeof(state->server_name), "服务器");
    snprintf(state->server_address, sizeof(state->server_address), "未设置");
    snprintf(state->server_status, sizeof(state->server_status), "待配置");
    snprintf(state->nas_name, sizeof(state->nas_name), "NAS");
    snprintf(state->nas_address, sizeof(state->nas_address), "未设置");
    snprintf(state->nas_status, sizeof(state->nas_status), "待配置");
    snprintf(state->notice, sizeof(state->notice), "系统正常");
    snprintf(state->forecast_today, sizeof(state->forecast_today), "等待更新");
    snprintf(state->forecast_tomorrow, sizeof(state->forecast_tomorrow), "等待更新");
    snprintf(state->forecast_after, sizeof(state->forecast_after), "等待更新");
    snprintf(state->weather_updated, sizeof(state->weather_updated), "等待更新");
}

static void load_info_config(home_state_t *state) {
    FILE *file = fopen(INFO_CONFIG_PATH, "r");
    if (!file) return;
    char line[128];
    while (fgets(line, sizeof(line), file)) {
        char *newline = strpbrk(line, "\r\n");
        if (newline) *newline = '\0';
        char *equals = strchr(line, '=');
        if (!equals) continue;
        *equals++ = '\0';
        if (strcmp(line, "title") == 0 && valid_info_value(equals, sizeof(state->panel_title)))
            snprintf(state->panel_title, sizeof(state->panel_title), "%s", equals);
        else if (strcmp(line, "location") == 0 && valid_info_value(equals, sizeof(state->location)))
            snprintf(state->location, sizeof(state->location), "%s", equals);
        else if (strcmp(line, "weather") == 0 && valid_info_value(equals, sizeof(state->weather)))
            snprintf(state->weather, sizeof(state->weather), "%s", equals);
        else if (strcmp(line, "temperature") == 0 && valid_info_value(equals, sizeof(state->temperature)))
            snprintf(state->temperature, sizeof(state->temperature), "%s", equals);
        else if (strcmp(line, "humidity") == 0 && valid_info_value(equals, sizeof(state->humidity)))
            snprintf(state->humidity, sizeof(state->humidity), "%s", equals);
        else if (strcmp(line, "server_name") == 0 && valid_info_value(equals, sizeof(state->server_name)))
            snprintf(state->server_name, sizeof(state->server_name), "%s", equals);
        else if (strcmp(line, "server_address") == 0 && valid_info_value(equals, sizeof(state->server_address)))
            snprintf(state->server_address, sizeof(state->server_address), "%s", equals);
        else if (strcmp(line, "server_status") == 0 && valid_info_value(equals, sizeof(state->server_status)))
            snprintf(state->server_status, sizeof(state->server_status), "%s", equals);
        else if (strcmp(line, "nas_name") == 0 && valid_info_value(equals, sizeof(state->nas_name)))
            snprintf(state->nas_name, sizeof(state->nas_name), "%s", equals);
        else if (strcmp(line, "nas_address") == 0 && valid_info_value(equals, sizeof(state->nas_address)))
            snprintf(state->nas_address, sizeof(state->nas_address), "%s", equals);
        else if (strcmp(line, "nas_status") == 0 && valid_info_value(equals, sizeof(state->nas_status)))
            snprintf(state->nas_status, sizeof(state->nas_status), "%s", equals);
        else if (strcmp(line, "notice") == 0 && valid_info_value(equals, sizeof(state->notice)))
            snprintf(state->notice, sizeof(state->notice), "%s", equals);
        else if (strcmp(line, "forecast_today") == 0 && valid_info_value(equals, sizeof(state->forecast_today)))
            snprintf(state->forecast_today, sizeof(state->forecast_today), "%s", equals);
        else if (strcmp(line, "forecast_tomorrow") == 0 && valid_info_value(equals, sizeof(state->forecast_tomorrow)))
            snprintf(state->forecast_tomorrow, sizeof(state->forecast_tomorrow), "%s", equals);
        else if (strcmp(line, "forecast_after") == 0 && valid_info_value(equals, sizeof(state->forecast_after)))
            snprintf(state->forecast_after, sizeof(state->forecast_after), "%s", equals);
        else if (strcmp(line, "weather_updated") == 0 && valid_info_value(equals, sizeof(state->weather_updated)))
            snprintf(state->weather_updated, sizeof(state->weather_updated), "%s", equals);
    }
    fclose(file);
}

static int save_info_config(const home_state_t *state) {
    FILE *file = fopen(INFO_CONFIG_TMP_PATH, "w");
    if (!file) return -1;
    fprintf(file,
            "title=%s\nlocation=%s\nweather=%s\ntemperature=%s\nhumidity=%s\n"
            "server_name=%s\nserver_address=%s\nserver_status=%s\n"
            "nas_name=%s\nnas_address=%s\nnas_status=%s\nnotice=%s\n"
            "forecast_today=%s\nforecast_tomorrow=%s\nforecast_after=%s\n"
            "weather_updated=%s\n",
            state->panel_title, state->location, state->weather,
            state->temperature, state->humidity, state->server_name,
            state->server_address, state->server_status, state->nas_name,
            state->nas_address, state->nas_status, state->notice,
            state->forecast_today, state->forecast_tomorrow,
            state->forecast_after, state->weather_updated);
    fflush(file);
    fsync(fileno(file));
    if (fclose(file) != 0 || rename(INFO_CONFIG_TMP_PATH, INFO_CONFIG_PATH) != 0) return -1;
    return 0;
}

static void apply_scene(home_state_t *state, scene_state_t scene) {
    state->scene = scene;
    if (scene == SCENE_HOME) {
        state->light_mask = 0x03;
        state->curtain = CURTAIN_OPEN;
    } else {
        state->light_mask = 0;
        state->curtain = CURTAIN_CLOSED;
    }
}


static void draw_ui(framebuffer_t *fb, const home_state_t *state) { (void)fb; (void)state; }
static void refresh_display(framebuffer_t *fb) { (void)fb; }

static const char *curtain_name(curtain_state_t state) {
    if (state == CURTAIN_OPEN) return "已打开";
    if (state == CURTAIN_STOPPED) return "已停止";
    return "已关闭";
}

static const char *scene_name(scene_state_t state) {
    if (state == SCENE_AWAY) return "离家";
    if (state == SCENE_SLEEP) return "睡眠";
    return "回家";
}

static int write_text_file(const char *path, const char *value) {
    int fd = open(path, O_WRONLY);
    if (fd < 0) return -1;
    size_t length = strlen(value);
    ssize_t written = write(fd, value, length);
    close(fd);
    return written == (ssize_t)length ? 0 : -1;
}

static void load_display_config(void) {
    FILE *file = fopen(DISPLAY_CONFIG_PATH, "r");
    if (!file) return;
    char line[64];
    while (fgets(line, sizeof(line), file)) {
        int value;
        if (sscanf(line, "brightness=%d", &value) == 1 && value >= 20 && value <= 100) {
            screen_brightness_percent = value;
        } else if (sscanf(line, "auto_brightness=%d", &value) == 1) {
            auto_brightness_enabled = value == 1;
        }
    }
    fclose(file);
}

static void save_display_config(void) {
    FILE *file = fopen(DISPLAY_CONFIG_PATH, "w");
    if (!file) return;
    fprintf(file, "brightness=%d\nauto_brightness=%d\n",
            screen_brightness_percent, auto_brightness_enabled ? 1 : 0);
    fflush(file);
    fsync(fileno(file));
    fclose(file);
}

static void set_default_sensors(home_state_t *state) {
    snprintf(state->indoor_temperature, sizeof(state->indoor_temperature), "--");
    snprintf(state->indoor_humidity, sizeof(state->indoor_humidity), "--");
    snprintf(state->air_quality, sizeof(state->air_quality), "未连接");
    state->illuminance_lux = -1;
    snprintf(state->sensor_updated, sizeof(state->sensor_updated), "等待更新");
    state->sensors_valid = 0;
    state->sensor_stale = 1;
}

static int valid_sensor_text(const char *value, size_t capacity) {
    return value && *value && valid_info_value(value, capacity);
}

static void load_sensor_config(home_state_t *state) {
    FILE *file = fopen(SENSOR_CONFIG_PATH, "r");
    if (!file) {
        state->sensors_valid = 0;
        state->sensor_stale = 1;
        return;
    }
    struct stat file_info;
    int stale = stat(SENSOR_CONFIG_PATH, &file_info) < 0 ||
                time(NULL) - file_info.st_mtime > 120;
    char line[128];
    int found = 0;
    while (fgets(line, sizeof(line), file)) {
        char *newline = strpbrk(line, "\r\n");
        if (newline) *newline = '\0';
        char *equals = strchr(line, '=');
        if (!equals) continue;
        *equals++ = '\0';
        if (strcmp(line, "temperature") == 0 &&
            valid_sensor_text(equals, sizeof(state->indoor_temperature))) {
            snprintf(state->indoor_temperature, sizeof(state->indoor_temperature), "%s", equals);
            found = 1;
        } else if (strcmp(line, "humidity") == 0 &&
                   valid_sensor_text(equals, sizeof(state->indoor_humidity))) {
            snprintf(state->indoor_humidity, sizeof(state->indoor_humidity), "%s", equals);
            found = 1;
        } else if (strcmp(line, "air_quality") == 0 &&
                   valid_sensor_text(equals, sizeof(state->air_quality))) {
            snprintf(state->air_quality, sizeof(state->air_quality), "%s", equals);
            found = 1;
        } else if (strcmp(line, "lux") == 0) {
            char *end = NULL;
            long lux = strtol(equals, &end, 10);
            if (end != equals && *end == '\0' && lux >= 0 && lux <= 200000) {
                state->illuminance_lux = (int)lux;
                found = 1;
            }
        } else if (strcmp(line, "updated") == 0 &&
                   valid_sensor_text(equals, sizeof(state->sensor_updated))) {
            snprintf(state->sensor_updated, sizeof(state->sensor_updated), "%s", equals);
            found = 1;
        }
    }
    fclose(file);
    state->sensors_valid = found;
    state->sensor_stale = stale;
}

static int save_sensor_config(const home_state_t *state) {
    FILE *file = fopen(SENSOR_CONFIG_TMP_PATH, "w");
    if (!file) return -1;
    fprintf(file, "temperature=%s\nhumidity=%s\nair_quality=%s\nlux=%d\nupdated=%s\n",
            state->indoor_temperature, state->indoor_humidity, state->air_quality,
            state->illuminance_lux < 0 ? 0 : state->illuminance_lux,
            state->sensor_updated);
    fflush(file);
    fsync(fileno(file));
    if (fclose(file) != 0 || rename(SENSOR_CONFIG_TMP_PATH, SENSOR_CONFIG_PATH) != 0) return -1;
    return 0;
}


static int open_gpio_output(unsigned gpio, int inactive_high) {
    char path[64];
    char number[16];
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%u", gpio);
    if (access(path, F_OK) != 0) {
        snprintf(number, sizeof(number), "%u", gpio);
        if (write_text_file("/sys/class/gpio/export", number) < 0 && errno != EBUSY) return -1;
        for (int attempt = 0; attempt < 50 && access(path, F_OK) != 0; ++attempt) usleep(10000);
    }
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%u/direction", gpio);
    if (write_text_file(path, inactive_high ? "high" : "low") < 0) return -1;
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%u/value", gpio);
    return open(path, O_WRONLY);
}


static void write_gpio_output(int fd, int value) {
    if (fd < 0) return;
    char c = value ? '1' : '0';
    lseek(fd, 0, SEEK_SET);
    write(fd, &c, 1);
}


static void set_output(int index, int active) {
    if (!output_backend.enabled || index < 0 || index >= 6 || output_backend.value_fd[index] < 0) return;
    const char value = (char)((active ^ output_backend.active_low) ? '1' : '0');
    lseek(output_backend.value_fd[index], 0, SEEK_SET);
    write(output_backend.value_fd[index], &value, 1);
}

static int output_pins_conflict(void) {
    for (int output = 0; output < 6; ++output) {
        if (output_gpios[output] == STATUS_LED_GPIO) return 1;
        for (int button = 0; button < HARDWARE_BUTTON_COUNT; ++button) {
            if (output_gpios[output] == HARDWARE_BUTTON_GPIOS[button]) return 1;
        }
        for (int other = output + 1; other < 6; ++other) {
            if (output_gpios[output] == output_gpios[other]) return 1;
        }
    }
    return 0;
}

static int64_t monotonic_ms(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) return 0;
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int sleep_timer_remaining_seconds(void) {
    if (sleep_timer_deadline_ms <= 0) return 0;
    int64_t remaining = sleep_timer_deadline_ms - monotonic_ms();
    if (remaining <= 0) return 0;
    return (int)((remaining + 999) / 1000);
}

static int auto_brightness_target(int lux) {
    if (lux <= 0) return 20;
    if (lux >= 500) return 100;
    return 20 + (lux * 80) / 500;
}

static int refresh_sensor_data(home_state_t *state) {
    int64_t now = monotonic_ms();
    if (now < next_sensor_refresh_ms) return 0;
    next_sensor_refresh_ms = now + 5000;
    int old_lux = state->illuminance_lux;
    int old_valid = state->sensors_valid;
    int old_stale = state->sensor_stale;
    char old_temperature[sizeof(state->indoor_temperature)];
    char old_humidity[sizeof(state->indoor_humidity)];
    snprintf(old_temperature, sizeof(old_temperature), "%s", state->indoor_temperature);
    snprintf(old_humidity, sizeof(old_humidity), "%s", state->indoor_humidity);
    load_sensor_config(state);
    int changed = old_lux != state->illuminance_lux || old_valid != state->sensors_valid ||
                  old_stale != state->sensor_stale ||
                  strcmp(old_temperature, state->indoor_temperature) != 0 ||
                  strcmp(old_humidity, state->indoor_humidity) != 0;
    if (auto_brightness_enabled && now >= brightness_manual_until_ms &&
        state->illuminance_lux >= 0) {
        int target = auto_brightness_target(state->illuminance_lux);
        if (target != screen_brightness_percent) {
            screen_brightness_percent = target;
            changed = 1;
        }
    }
    return changed;
}

static const char *backend_name(void) {
    if (output_backend.enabled) return "gpio";
    if (output_backend.requested) return "gpio-disarmed";
    return "simulation";
}

static void arm_curtain_timeout(const home_state_t *state) {
    if (state->curtain == CURTAIN_STOPPED || state->curtain_timeout_seconds <= 0) {
        curtain_deadline_ms = 0;
    } else {
        curtain_deadline_ms = monotonic_ms() + (int64_t)state->curtain_timeout_seconds * 1000;
    }
}

static void log_output_state(const home_state_t *state) {
    FILE *file = fopen(EVENT_LOG_PATH, "a");
    if (!file) return;
    const char *output_name = output_backend.enabled ? "物理输出" :
                              output_backend.requested ? "物理输出未启用" : "仿真输出";
    fprintf(file, "启动后%lld秒 灯光=%u 窗帘=%s 场景=%s 输出=%s\n",
            (long long)(monotonic_ms() / 1000),
            (unsigned)(state->light_mask & ALL_LIGHTS_MASK), curtain_name(state->curtain),
            scene_name(state->scene), output_name);
    fclose(file);
}

static void log_alert_event(const alert_state_t *alert, const char *action) {
    FILE *file = fopen(EVENT_LOG_PATH, "a");
    if (!file) return;
    time_t now = time(NULL);
    struct tm local;
    char timestamp[24] = "时间未知";
    if (localtime_r(&now, &local)) {
        strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &local);
    }
    fprintf(file, "%s %s：%s\n", timestamp, action, alert->message);
    fclose(file);
}

static void set_alert_active(int index, int active) {
    if (index < 0 || index >= ALERT_COUNT || alert_states[index].active == active) return;
    alert_states[index].active = active;
    if (active) {
        alert_states[index].acknowledged = 0;
        alert_states[index].since = time(NULL);
        log_alert_event(&alert_states[index], "告警");
    } else {
        log_alert_event(&alert_states[index], "恢复");
        alert_states[index].acknowledged = 0;
        alert_states[index].since = 0;
    }
}

static void refresh_alerts(const home_state_t *state) {
    set_alert_active(0, strcmp(state->ip, "OFFLINE") == 0);
    set_alert_active(1, strcmp(state->server_status, "离线") == 0);
    set_alert_active(2, strcmp(state->nas_status, "离线") == 0);
    set_alert_active(3, state->storage_used_percent >= 90);
    set_alert_active(4, state->memory_used_percent >= 90);
    set_alert_active(5, state->wifi_signal_dbm != 0 && state->wifi_signal_dbm < -75);
}

static int active_alert_count(void) {
    int count = 0;
    for (int index = 0; index < ALERT_COUNT; ++index) count += alert_states[index].active;
    return count;
}

static void apply_outputs(home_state_t *state) {
    if (output_backend.enabled) {
        for (int index = 0; index < LIGHT_COUNT; ++index) {
            set_output(index, (state->light_mask & (1u << index)) != 0);
        }
        /* Interlock the two curtain directions before selecting either one. */
        set_output(4, 0);
        set_output(5, 0);
        if (state->curtain == CURTAIN_OPEN) set_output(4, 1);
        else if (state->curtain == CURTAIN_CLOSED) set_output(5, 1);
    }
    state->gpio_requested = output_backend.requested;
    state->gpio_enabled = output_backend.enabled;
    state->active_low = output_backend.active_low;
    log_output_state(state);
}

static void init_output_backend(home_state_t *state) {
    memset(&output_backend, 0, sizeof(output_backend));
    output_backend.active_low = 1;
    state->curtain_timeout_seconds = 20;
    for (int index = 0; index < 6; ++index) output_backend.value_fd[index] = -1;

    int armed = 0;
    FILE *file = fopen(CONFIG_PATH, "r");
    if (file) {
        char line[80];
        while (fgets(line, sizeof(line), file)) {
            int timeout_seconds;
            int gpio;
            int auth_flag;
            char pin[13];
            if (strncmp(line, "backend=gpio", 12) == 0) output_backend.requested = 1;
            else if (strncmp(line, "armed=1", 7) == 0) armed = 1;
            else if (strncmp(line, "active_low=0", 12) == 0) output_backend.active_low = 0;
            else if (strncmp(line, "active_low=1", 12) == 0) output_backend.active_low = 1;
            else if (sscanf(line, "curtain_timeout_seconds=%d", &timeout_seconds) == 1 &&
                     timeout_seconds >= 1 && timeout_seconds <= 120) {
                state->curtain_timeout_seconds = timeout_seconds;
            } else if (sscanf(line, "light1_gpio=%d", &gpio) == 1 && gpio >= 0 && gpio <= 1023) {
                output_gpios[0] = (unsigned)gpio;
            } else if (sscanf(line, "light2_gpio=%d", &gpio) == 1 && gpio >= 0 && gpio <= 1023) {
                output_gpios[1] = (unsigned)gpio;
            } else if (sscanf(line, "light3_gpio=%d", &gpio) == 1 && gpio >= 0 && gpio <= 1023) {
                output_gpios[2] = (unsigned)gpio;
            } else if (sscanf(line, "light4_gpio=%d", &gpio) == 1 && gpio >= 0 && gpio <= 1023) {
                output_gpios[3] = (unsigned)gpio;
            } else if (sscanf(line, "curtain_open_gpio=%d", &gpio) == 1 && gpio >= 0 && gpio <= 1023) {
                output_gpios[4] = (unsigned)gpio;
            } else if (sscanf(line, "curtain_close_gpio=%d", &gpio) == 1 && gpio >= 0 && gpio <= 1023) {
                output_gpios[5] = (unsigned)gpio;
            } else if (sscanf(line, "control_pin=%12s", pin) == 1) {
                size_t length = strlen(pin);
                int valid = length >= 4 && length <= 12;
                for (size_t index = 0; index < length && valid; ++index) {
                    if (pin[index] < '0' || pin[index] > '9') valid = 0;
                }
                if (valid) snprintf(state->control_pin, sizeof(state->control_pin), "%s", pin);
            } else if (sscanf(line, "auth_enabled=%d", &auth_flag) == 1) {
                state->auth_enabled = (auth_flag == 0) ? 0 : 1;   /* M9A: 非 0 一律视为启用 */
            }
        }
        fclose(file);
    }

    if (output_backend.requested && armed && output_pins_conflict()) {
        fprintf(stderr, "gpio output disabled: output pins conflict with buttons or status led\n");
        armed = 0;
    }
    if (output_backend.requested && armed) {
        for (int index = 0; index < 6; ++index) {
            output_backend.value_fd[index] = open_gpio_output(
                output_gpios[index], output_backend.active_low);
            if (output_backend.value_fd[index] < 0) {
                fprintf(stderr, "gpio %u: %s\n", output_gpios[index], strerror(errno));
                for (int close_index = 0; close_index < 6; ++close_index) {
                    if (output_backend.value_fd[close_index] >= 0) {
                        close(output_backend.value_fd[close_index]);
                        output_backend.value_fd[close_index] = -1;
                    }
                }
                armed = 0;
                break;
            }
        }
    }
    output_backend.enabled = output_backend.requested && armed;
    /* Never resume a motor direction automatically after a reboot. */
    if (output_backend.enabled) state->curtain = CURTAIN_STOPPED;
    apply_outputs(state);
    printf("output backend: %s, polarity=%s, curtain-timeout=%ds\n",
           backend_name(),
           output_backend.active_low ? "active-low" : "active-high",
           state->curtain_timeout_seconds);
}

static void close_output_backend(void) {
    if (output_backend.enabled) {
        for (int index = 0; index < 6; ++index) set_output(index, 0);
    }
    for (int index = 0; index < 6; ++index) {
        if (output_backend.value_fd[index] >= 0) close(output_backend.value_fd[index]);
        output_backend.value_fd[index] = -1;
    }
}


static int send_all(int fd, const char *data, size_t length) {
    while (length > 0) {
        ssize_t sent = send(fd, data, length, 0);
        if (sent <= 0) return -1;
        data += sent;
        length -= (size_t)sent;
    }
    return 0;
}

static void send_response(int fd, const char *status, const char *type, const char *body) {
    char header[256];
    int n = snprintf(header, sizeof(header),
                     "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %u\r\n"
                     "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
                     status, type, (unsigned)strlen(body));
    send_all(fd, header, (size_t)n);
    send_all(fd, body, strlen(body));
}

static void send_file_response(int fd, const char *path, const char *type,
                               const char *download_name) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        send_response(fd, "404 Not Found", "text/plain; charset=utf-8", "App not available");
        return;
    }
    struct stat info;
    if (fstat(fileno(file), &info) < 0 || info.st_size <= 0) {
        fclose(file);
        send_response(fd, "500 Internal Server Error", "text/plain; charset=utf-8", "Invalid app file");
        return;
    }
    char header[384];
    int length = snprintf(header, sizeof(header),
                          "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %lld\r\n"
                          "Content-Disposition: %s\r\n"
                          "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
                          type, (long long)info.st_size, download_name ? "attachment" : "inline");
    if (length <= 0 || length >= (int)sizeof(header) ||
        send_all(fd, header, (size_t)length) < 0) {
        fclose(file);
        return;
    }
    char buffer[8192];
    size_t bytes;
    while ((bytes = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        if (send_all(fd, buffer, bytes) < 0) break;
    }
    fclose(file);
}

/* TRAE-G3: wallpaper pipeline forward decls (impl in wallpaper.inc, included
 * right after remote_state.inc; state_json/persist_display/load_remote_config/
 * handle_remote above call these). */
static const char *wp_current_id(void);
static long wp_current_revision(void);
static int wp_current_preset(void);
static void wp_note_display_revision(long rev);
static int wallpaper_route(int fd, const char *method, const char *path,
                           const char *req, home_state_t *s);
static int wallpaper_peek_upload(int fd, char *hdr, size_t cap);
static void wallpaper_upload_stream(int fd, const char *hdr, home_state_t *s);
static void wallpaper_boot_init(void);
static int query_value(const char *path, const char *key, char *output, size_t capacity);

#include "remote_state.inc"
#include "wallpaper.inc"
#include "demo.inc"

static void send_diagnostics(int fd, const home_state_t *s) { send_state(fd, s); }
static void send_touch_debug(int fd) {
    send_response(fd, "200 OK", "application/json", "{\"mode\":\"disabled\"}");
}

static void send_history(int fd) {
    char lines[8][160];
    char body[1400];
    int count = 0;
    memset(lines, 0, sizeof(lines));
    FILE *file = fopen(EVENT_LOG_PATH, "r");
    if (file) {
        while (fgets(lines[count % 8], sizeof(lines[0]), file)) count++;
        fclose(file);
    }
    size_t used = 0;
    int available = count < 8 ? count : 8;
    int start = count >= 8 ? count % 8 : 0;
    for (int index = 0; index < available; ++index) {
        const char *line = lines[(start + index) % 8];
        size_t length = strlen(line);
        if (used + length >= sizeof(body) - 1) break;
        memcpy(body + used, line, length);
        used += length;
    }
    if (used == 0) {
        snprintf(body, sizeof(body), "No actions yet\n");
    } else {
        body[used] = '\0';
    }
    send_response(fd, "200 OK", "text/plain; charset=utf-8", body);
}

static void acknowledge_alerts(void) {
    for (int index = 0; index < ALERT_COUNT; ++index) {
        if (alert_states[index].active && !alert_states[index].acknowledged) {
            alert_states[index].acknowledged = 1;
            log_alert_event(&alert_states[index], "已确认");
        }
    }
}

static void send_alerts(int fd) {
    char body[2200];
    size_t used = 0;
    int length = snprintf(body, sizeof(body), "当前告警（%d）：\n", active_alert_count());
    if (length < 0 || (size_t)length >= sizeof(body)) return;
    used = (size_t)length;
    int active = 0;
    for (int index = 0; index < ALERT_COUNT; ++index) {
        if (!alert_states[index].active) continue;
        active = 1;
        length = snprintf(body + used, sizeof(body) - used, "[%s] %s\n",
                          alert_states[index].acknowledged ? "已确认" : "未确认",
                          alert_states[index].message);
        if (length < 0 || (size_t)length >= sizeof(body) - used) break;
        used += (size_t)length;
    }
    if (!active) {
        length = snprintf(body + used, sizeof(body) - used, "无活动告警\n");
        if (length > 0 && (size_t)length < sizeof(body) - used) used += (size_t)length;
    }
    length = snprintf(body + used, sizeof(body) - used, "\n最近记录：\n");
    if (length > 0 && (size_t)length < sizeof(body) - used) used += (size_t)length;

    char lines[12][160];
    int count = 0;
    memset(lines, 0, sizeof(lines));
    FILE *file = fopen(EVENT_LOG_PATH, "r");
    if (file) {
        char line[160];
        while (fgets(line, sizeof(line), file)) {
            if (!strstr(line, "告警") && !strstr(line, "恢复") && !strstr(line, "已确认")) continue;
            snprintf(lines[count % 12], sizeof(lines[0]), "%s", line);
            count++;
        }
        fclose(file);
    }
    int available = count < 12 ? count : 12;
    int start = count >= 12 ? count % 12 : 0;
    for (int index = 0; index < available; ++index) {
        const char *line = lines[(start + index) % 12];
        length = snprintf(body + used, sizeof(body) - used, "%s", line);
        if (length < 0 || (size_t)length >= sizeof(body) - used) break;
        used += (size_t)length;
    }
    body[used < sizeof(body) ? used : sizeof(body) - 1] = '\0';
    send_response(fd, "200 OK", "text/plain; charset=utf-8", body);
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int query_value(const char *path, const char *key, char *output, size_t capacity) {
    const char *cursor = strchr(path, '?');
    if (!cursor) return -1;
    cursor++;
    size_t key_length = strlen(key);
    while (*cursor) {
        const char *end = strchr(cursor, '&');
        if (!end) end = cursor + strlen(cursor);
        if ((size_t)(end - cursor) > key_length &&
            strncmp(cursor, key, key_length) == 0 && cursor[key_length] == '=') {
            const char *source = cursor + key_length + 1;
            size_t used = 0;
            while (source < end) {
                unsigned char value;
                if (*source == '%' && source + 2 < end) {
                    int high = hex_value(source[1]);
                    int low = hex_value(source[2]);
                    if (high < 0 || low < 0) return -1;
                    value = (unsigned char)((high << 4) | low);
                    source += 3;
                } else {
                    value = (unsigned char)(*source == '+' ? ' ' : *source);
                    source++;
                }
                if (value == 0 || used + 1 >= capacity) return -1;
                output[used++] = (char)value;
            }
            output[used] = '\0';
            return 0;
        }
        cursor = *end ? end + 1 : end;
    }
    return -1;
}

static int update_device_names(home_state_t *state, const char *path) {
    static const char *keys[5] = {"l1", "l2", "l3", "l4", "curtain"};
    char values[5][32];
    for (int index = 0; index < 5; ++index) {
        if (query_value(path, keys[index], values[index], sizeof(values[index])) < 0 ||
            !valid_device_name(values[index])) return -1;
    }
    for (int index = 0; index < LIGHT_COUNT; ++index) {
        snprintf(state->light_names[index], sizeof(state->light_names[index]), "%s", values[index]);
    }
    snprintf(state->curtain_device_name, sizeof(state->curtain_device_name), "%s", values[4]);
    return save_device_names(state);
}

static int update_info_config(home_state_t *state, const char *path) {
    static const char *keys[12] = {
        "title", "location", "weather", "temperature", "humidity",
        "server_name", "server_address", "server_status",
        "nas_name", "nas_address", "nas_status", "notice"
    };
    size_t capacities[12] = {
        sizeof(state->panel_title), sizeof(state->location), sizeof(state->weather),
        sizeof(state->temperature), sizeof(state->humidity), sizeof(state->server_name),
        sizeof(state->server_address), sizeof(state->server_status), sizeof(state->nas_name),
        sizeof(state->nas_address), sizeof(state->nas_status), sizeof(state->notice)
    };
    char values[12][64];
    for (int index = 0; index < 12; ++index) {
        if (query_value(path, keys[index], values[index], sizeof(values[index])) < 0 ||
            !valid_info_value(values[index], capacities[index])) return -1;
    }
    snprintf(state->panel_title, sizeof(state->panel_title), "%s", values[0]);
    snprintf(state->location, sizeof(state->location), "%s", values[1]);
    snprintf(state->weather, sizeof(state->weather), "%s", values[2]);
    snprintf(state->temperature, sizeof(state->temperature), "%s", values[3]);
    snprintf(state->humidity, sizeof(state->humidity), "%s", values[4]);
    snprintf(state->server_name, sizeof(state->server_name), "%s", values[5]);
    snprintf(state->server_address, sizeof(state->server_address), "%s", values[6]);
    snprintf(state->server_status, sizeof(state->server_status), "%s", values[7]);
    snprintf(state->nas_name, sizeof(state->nas_name), "%s", values[8]);
    snprintf(state->nas_address, sizeof(state->nas_address), "%s", values[9]);
    snprintf(state->nas_status, sizeof(state->nas_status), "%s", values[10]);
    snprintf(state->notice, sizeof(state->notice), "%s", values[11]);
    return save_info_config(state);
}

static int update_weather_config(home_state_t *state, const char *path) {
    static const char *keys[8] = {
        "location", "weather", "temperature", "humidity",
        "today", "tomorrow", "after", "updated"
    };
    size_t capacities[8] = {
        sizeof(state->location), sizeof(state->weather),
        sizeof(state->temperature), sizeof(state->humidity),
        sizeof(state->forecast_today), sizeof(state->forecast_tomorrow),
        sizeof(state->forecast_after), sizeof(state->weather_updated)
    };
    char values[8][64];
    for (int index = 0; index < 8; ++index) {
        if (query_value(path, keys[index], values[index], sizeof(values[index])) < 0 ||
            !valid_info_value(values[index], capacities[index])) return -1;
    }
    snprintf(state->location, sizeof(state->location), "%s", values[0]);
    snprintf(state->weather, sizeof(state->weather), "%s", values[1]);
    snprintf(state->temperature, sizeof(state->temperature), "%s", values[2]);
    snprintf(state->humidity, sizeof(state->humidity), "%s", values[3]);
    snprintf(state->forecast_today, sizeof(state->forecast_today), "%s", values[4]);
    snprintf(state->forecast_tomorrow, sizeof(state->forecast_tomorrow), "%s", values[5]);
    snprintf(state->forecast_after, sizeof(state->forecast_after), "%s", values[6]);
    snprintf(state->weather_updated, sizeof(state->weather_updated), "%s", values[7]);
    return save_info_config(state);
}

static int update_sensor_config(home_state_t *state, const char *path) {
    char temperature[sizeof(state->indoor_temperature)];
    char humidity[sizeof(state->indoor_humidity)];
    char air_quality[sizeof(state->air_quality)];
    char updated[sizeof(state->sensor_updated)];
    char lux_text[16];
    if (query_value(path, "temperature", temperature, sizeof(temperature)) < 0 ||
        query_value(path, "humidity", humidity, sizeof(humidity)) < 0 ||
        query_value(path, "air_quality", air_quality, sizeof(air_quality)) < 0 ||
        query_value(path, "lux", lux_text, sizeof(lux_text)) < 0 ||
        query_value(path, "updated", updated, sizeof(updated)) < 0 ||
        !valid_sensor_text(temperature, sizeof(temperature)) ||
        !valid_sensor_text(humidity, sizeof(humidity)) ||
        !valid_sensor_text(air_quality, sizeof(air_quality)) ||
        !valid_sensor_text(updated, sizeof(updated))) return -1;
    char *end = NULL;
    long lux = strtol(lux_text, &end, 10);
    if (end == lux_text || *end != '\0' || lux < 0 || lux > 200000) return -1;
    snprintf(state->indoor_temperature, sizeof(state->indoor_temperature), "%s", temperature);
    snprintf(state->indoor_humidity, sizeof(state->indoor_humidity), "%s", humidity);
    snprintf(state->air_quality, sizeof(state->air_quality), "%s", air_quality);
    state->illuminance_lux = (int)lux;
    snprintf(state->sensor_updated, sizeof(state->sensor_updated), "%s", updated);
    state->sensors_valid = 1;
    state->sensor_stale = 0;
    return save_sensor_config(state);
}

static int update_display_config(const char *path) {
    char value[8];
    if (query_value(path, "auto_brightness", value, sizeof(value)) < 0 ||
        (strcmp(value, "0") != 0 && strcmp(value, "1") != 0)) return -1;
    auto_brightness_enabled = strcmp(value, "1") == 0;
    if (!auto_brightness_enabled) brightness_manual_until_ms = 0;
    save_display_config();
    return 0;
}

static int is_command_path(const char *path) {
    return strncmp(path, "/api/light/", 11) == 0 ||
           strncmp(path, "/api/curtain/", 13) == 0 ||
           strncmp(path, "/api/scene/", 11) == 0 ||
           strncmp(path, "/api/timer/", 11) == 0 ||
           strncmp(path, "/api/alerts/", 12) == 0 ||
           strncmp(path, "/api/config/sensors", 19) == 0 ||
           strncmp(path, "/api/config/", 12) == 0;
}

static int control_pin_valid(const char *request, const home_state_t *state) {
    if (!state->auth_enabled) return 1;   /* M9A: 鉴权关闭 → 一律放行 */
    const char *header = strstr(request, "\r\nX-Control-Pin:");
    if (!header) return 0;
    header += strlen("\r\nX-Control-Pin:");
    while (*header == ' ' || *header == '\t') header++;
    size_t length = strcspn(header, "\r\n");
    return length == strlen(state->control_pin) &&
           strncmp(header, state->control_pin, length) == 0;
}

static void commit_state(framebuffer_t *fb, home_state_t *state) {
    update_ip(state);
    apply_outputs(state);
    draw_ui(fb, state);
    refresh_display(fb);
    if (save_state(state) < 0) {
        fprintf(stderr, "save state: %s\n", strerror(errno));
    }
}

static void refresh_network_status(framebuffer_t *fb, home_state_t *state) {
    char previous[sizeof(state->ip)];
    snprintf(previous, sizeof(previous), "%s", state->ip);
    update_ip(state);
    refresh_alerts(state);
    time_t current_minute = time(NULL) / 60;
    if (strcmp(previous, state->ip) != 0 || current_minute != last_display_minute) {
        last_display_minute = current_minute;
        draw_ui(fb, state);
        refresh_display(fb);
    }
}

static int valid_host_address(const char *address) {
    if (!address || !*address || strcmp(address, "未设置") == 0) return 0;
    for (const unsigned char *p = (const unsigned char *)address; *p; ++p) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
            (*p >= '0' && *p <= '9') || *p == '.' || *p == '-') continue;
        return 0;
    }
    return 1;
}

static int ping_host(const char *address) {
    if (!valid_host_address(address)) return -1;
    char command[160];
    snprintf(command, sizeof(command), "ping -c 1 -W 1 %s >/dev/null 2>&1", address);
    return system(command) == 0 ? 1 : 0;
}

static void refresh_device_status(framebuffer_t *fb, home_state_t *state) {
    int64_t now = monotonic_ms();
    if (now < next_device_check_ms) return;
    next_device_check_ms = now + 30000;

    char server_before[sizeof(state->server_status)];
    char nas_before[sizeof(state->nas_status)];
    snprintf(server_before, sizeof(server_before), "%s", state->server_status);
    snprintf(nas_before, sizeof(nas_before), "%s", state->nas_status);

    int server_online = ping_host(state->server_address);
    int nas_online = ping_host(state->nas_address);
    snprintf(state->server_status, sizeof(state->server_status), "%s",
             server_online < 0 ? "待配置" : (server_online ? "在线" : "离线"));
    snprintf(state->nas_status, sizeof(state->nas_status), "%s",
             nas_online < 0 ? "待配置" : (nas_online ? "在线" : "离线"));
    refresh_alerts(state);

    if (strcmp(server_before, state->server_status) != 0 ||
        strcmp(nas_before, state->nas_status) != 0) {
        draw_ui(fb, state);
        refresh_display(fb);
    }
}

static void collect_system_metrics(home_state_t *state) {
    FILE *file = fopen("/proc/uptime", "r");
    if (file) {
        double seconds = 0;
        if (fscanf(file, "%lf", &seconds) == 1) state->uptime_seconds = (long)seconds;
        fclose(file);
    }

    long total = 0;
    long available = 0;
    file = fopen("/proc/meminfo", "r");
    if (file) {
        char line[128];
        while (fgets(line, sizeof(line), file)) {
            if (sscanf(line, "MemTotal: %ld kB", &total) == 1) continue;
            if (sscanf(line, "MemAvailable: %ld kB", &available) == 1) continue;
        }
        fclose(file);
    }
    if (total > 0) {
        state->memory_total_kb = total;
        state->memory_used_kb = total - available;
        state->memory_used_percent = (int)((state->memory_used_kb * 100L) / total);
    }

    struct statvfs disk;
    if (statvfs("/mnt/UDISK", &disk) == 0 && disk.f_blocks > 0) {
        unsigned long long total_bytes =
            (unsigned long long)disk.f_blocks * (unsigned long long)disk.f_frsize;
        unsigned long long available_bytes =
            (unsigned long long)disk.f_bavail * (unsigned long long)disk.f_frsize;
        state->storage_available_mb = available_bytes / (1024ULL * 1024ULL);
        state->storage_used_percent = (int)(((total_bytes - available_bytes) * 100ULL) /
                                            total_bytes);
    }

    file = fopen("/proc/loadavg", "r");
    if (file) {
        double load = 0;
        if (fscanf(file, "%lf", &load) == 1)
            snprintf(state->load_average, sizeof(state->load_average), "%.2f", load);
        fclose(file);
    }

    state->wifi_signal_dbm = 0;
    FILE *pipe = popen("iw dev wlan0 link 2>/dev/null", "r");
    if (pipe) {
        char line[128];
        while (fgets(line, sizeof(line), pipe)) {
            int signal;
            if (sscanf(line, " signal: %d dBm", &signal) == 1) {
                state->wifi_signal_dbm = signal;
                break;
            }
        }
        pclose(pipe);
    }

    if (strcmp(state->ip, "OFFLINE") == 0)
        snprintf(state->system_alert, sizeof(state->system_alert), "网络连接中断");
    else if (state->storage_used_percent >= 90)
        snprintf(state->system_alert, sizeof(state->system_alert), "存储空间不足");
    else if (state->memory_used_percent >= 90)
        snprintf(state->system_alert, sizeof(state->system_alert), "内存占用过高");
    else if (state->wifi_signal_dbm < -75)
        snprintf(state->system_alert, sizeof(state->system_alert), "Wi-Fi信号较弱");
    else
        snprintf(state->system_alert, sizeof(state->system_alert), "运行正常");
    refresh_alerts(state);
}

static void refresh_system_status(framebuffer_t *fb, home_state_t *state) {
    int64_t now = monotonic_ms();
    int redraw = 0;
    if (refresh_sensor_data(state)) redraw = 1;
    if (now >= next_system_refresh_ms) {
        next_system_refresh_ms = now + 5000;
        collect_system_metrics(state);
        if (display_page == 2) redraw = 1;
    }

    if (rotation_enabled && now >= next_page_switch_ms) {
        /* Bounded advance. A mask with no usable page is now reachable from
         * configuration (hiding the smart-home entry clears its bit, and a
         * hand-edited display.json can be empty), and the old do/while would
         * spin forever on it. Staying put is the safe degradation. */
        int next = display_page;
        for (int step = 0; step < PAGE_COUNT; step++) {
            next = (next + 1) % PAGE_COUNT;
            if (rotation_mask & (1 << next)) break;
        }
        if (rotation_mask & (1 << next)) display_page = next;
        current_view = VIEW_DASHBOARD;
        next_page_switch_ms = now + rotation_interval * 1000;
        state_revision++;
        redraw = 1;
    }

    /* M5-B: briefing auto-paging (实施方案 41 行) — only when the day's
     * briefing spans more than one screen and the phone has not pinned one. */
    if (briefing_pages > 1 && !briefing_pinned && now >= briefing_advance_ms) {
        briefing_advance_ms = now + 8000;
        briefing_part = (briefing_part + 1) % briefing_pages;
    }

    /* N6: timetable/plans auto-paging (same rule as briefing — only when the
     * page spans more than one screen and the phone has not pinned one). */
    if (timetable_pages > 1 && !timetable_pinned && now >= timetable_advance_ms) {
        timetable_advance_ms = now + 8000;
        timetable_part = (timetable_part + 1) % timetable_pages;
    }
    if (plans_pages > 1 && !plans_pinned && now >= plans_advance_ms) {
        plans_advance_ms = now + 8000;
        plans_part = (plans_part + 1) % plans_pages;
    }

    if (redraw) {
        draw_ui(fb, state);
        refresh_display(fb);
    }
}

/* ---------------------------------------------------- v2 存储桥接（M3-D）
 * /api/v2/ 前缀的请求交给 pv2 协议/存储层应答；初始化失败只降级（503），绝不影响
 * v1 路由与显示管线（方案判据"故障不阻塞显示"）。生产库落盘根目录来自
 * storage-design.md:480（0700 root）。单线程 select 模型与 pv2 的
 * SQLITE_THREADSAFE=0/Threads=1 约定天然匹配。 */
#define V2_DATA_DIR   "/mnt/UDISK/panel-v3/data"
#define V2_BOARD_ID   "20080411"
#define V2_LIBRARY_ID "main"

static pv2_ctx g_v2;
static int g_v2_ready = 0;

/* M6-B1：AI 队列桥（提交 + 回传），实现见 ai_tick.c；2s 节流，单线程调用。 */
void ai_tick(pv2_ctx *ctx, const char *queue_dir);
static int64_t ai_tick_last_ms = 0;

static const char *v2_http_text(int code) {
    switch (code) {
    case 200: return "200 OK";
    case 201: return "201 Created";
    case 400: return "400 Bad Request";
    case 401: return "401 Unauthorized";
    case 403: return "403 Forbidden";
    case 404: return "404 Not Found";
    case 405: return "405 Method Not Allowed";
    case 409: return "409 Conflict";
    case 413: return "413 Payload Too Large";
    case 422: return "422 Unprocessable Entity";
    case 429: return "429 Too Many Requests";
    case 503: return "503 Service Unavailable";
    default:  return "500 Internal Server Error";
    }
}

/* 从请求头里按行取出 name 的值（大小写不敏感；token 只走头，绝不进 URL）。 */
static void v2_header_value(const char *request, const char *name, char *out, size_t cap) {
    out[0] = 0;
    size_t nlen = strlen(name);
    const char *p = request;
    const char *found = NULL;
    while ((p = strstr(p, "\r\n")) != NULL && !found) {
        p += 2;
        if (strncasecmp(p, name, nlen) == 0) found = p + nlen;
    }
    if (!found) return;
    while (*found == ' ' || *found == '\t') found++;
    size_t i = 0;
    while (found[i] && found[i] != '\r' && i + 1 < cap) { out[i] = found[i]; i++; }
    out[i] = 0;
}

/* server_epoch 持久化：<dir>/server_epoch 缺失时用 OS CSPRNG 生成并 0600 落盘。
 * 写盘失败必须拒绝初始化——重启换新 epoch 会让全部游标失效（强迫全量重同步）。 */
static int v2_epoch_prepare(char *epoch, size_t n) {
    char path[576];
    snprintf(path, sizeof path, V2_DATA_DIR "/server_epoch");
    int fd = open(path, O_RDONLY);
    if (fd >= 0) {
        char buf[64] = {0};
        ssize_t got = read(fd, buf, sizeof buf - 1);
        close(fd);
        size_t len = 0;
        while (len < sizeof buf && buf[len] > ' ') len++;
        if (got > 0 && len >= 8 && len < n) { memcpy(epoch, buf, len); epoch[len] = 0; return 0; }
        fprintf(stderr, "v2: server_epoch file invalid\n");
        return -1;
    }
    if (errno != ENOENT) return -1;
    if (pv2_random_hex(epoch, 16) != 0) return -1;
    fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return -1;
    size_t len2 = strlen(epoch);
    int ok = write(fd, epoch, len2) == (ssize_t)len2;
    if (close(fd) != 0) ok = 0;
    if (!ok) { unlink(path); return -1; }
    return 0;
}

/* v2 初始化：失败只记录并保持 g_v2_ready=0（/api/v2 统一 503），不 return 进程。 */
static void v2_init(const char *control_pin, int auth_enabled) {
    char err[256] = "";
    char epoch[48];
    if (mkdir(V2_DATA_DIR, 0700) != 0 && errno != EEXIST) {
        fprintf(stderr, "v2: mkdir %s: %s\n", V2_DATA_DIR, strerror(errno));
        return;
    }
    char authdir[576];
    snprintf(authdir, sizeof authdir, V2_DATA_DIR "/auth");
    if (mkdir(authdir, 0700) != 0 && errno != EEXIST) {
        fprintf(stderr, "v2: mkdir %s: %s\n", authdir, strerror(errno));
        return;
    }
    if (v2_epoch_prepare(epoch, sizeof epoch) != 0) {
        fprintf(stderr, "v2: server epoch unavailable; /api/v2 disabled\n");
        return;
    }
    /* sync_mode=1 → PRAGMA synchronous=FULL（T06A 验证过的生产姿态） */
    if (pv2_ctx_init(&g_v2, V2_DATA_DIR, PV2_IMPL_SQLITE, V2_BOARD_ID, V2_LIBRARY_ID,
                     epoch, control_pin, 1, err, sizeof err) != 0) {
        fprintf(stderr, "v2: storage init failed: %s\n", err);
        return;
    }
    g_v2.auth_enabled = auth_enabled;   /* M9A: 配置开关透传到 v2 鉴权层 */
    g_v2_ready = 1;
    printf("v2 storage ready: %s (epoch %s)\n", V2_DATA_DIR, epoch);
    fflush(stdout);
}

/* N2: 业务日 = CST-8 本地日历日（启动已 setenv TZ=CST-8，与 ai_tick/display
 * 同约定）。不用 pv2_date_of（UTC）——UTC 口径在 CST 00:00–08:00 仍是前一天
 * （TRAE-F1 教训），会把昨日清单误判成今日。 */
static void service_local_date(char *buf, size_t n)
{
    time_t t = time(NULL);
    struct tm local_tm;
    if (localtime_r(&t, &local_tm))
        snprintf(buf, n, "%04d-%02d-%02d",
                 local_tm.tm_year + 1900, local_tm.tm_mon + 1, local_tm.tm_mday);
    else
        snprintf(buf, n, "1970-01-01");
}

/* M5-A: 每日单词精简快照。N2 起数据源换成手机推送的当日清单（pv2 实体
 * "daily_plan"，pv2_planlist_pick 物化），不再按日期哈希选总库词——板卡只
 * 显示手机当天清单，绝不自行造词（续工方案 §4.1）。
 * 当前词 = 清单里第一个待学（pending），无待学取第一个待复习（review），
 * 再取首行；文本 join word 实体，TRAE-F2: 注释三字段取自 annotation 实体
 * （id=word_id），缺失/损坏 → 空串，页面「—」占位，绝不编造。
 * 进度字段（真进度）：total=实际清单数、done=已记住数、review=待复习数、
 * goal=清单行内携带的目标（取最大）、stale=清单非今日（昨日缓存，页面注明
 * 日期）。任何读失败/无清单：total=0 且无 word_today 字段，页面走等待同步
 * 占位；清单日期落到 datebuf（无清单时=业务日）供页面标注。 */
static size_t utf8_fit(const char *s, size_t cap);
static void words_today_snapshot(char *text, size_t tcap, int *total, int *index,
                                 char *datebuf, size_t dcap,
                                 char *phon, size_t phcap, char *mean, size_t mcap,
                                 char *ex, size_t xcap,
                                 int *done, int *review, int *goal, int *stale) {
    pv2_plan_row rows[200];
    char today[16], ldate[16];
    char payload[1024];
    size_t plen = 0;
    long rev = 0;
    int del = 0;
    int cap = (int)(sizeof rows / sizeof rows[0]);
    int n, i, cur = -1, ndone = 0, nreview = 0, goalv = 0;
    *total = 0;
    *index = 0;
    *done = 0;
    *review = 0;
    *goal = 0;
    *stale = 0;
    if (text && tcap) text[0] = 0;
    if (phon && phcap) phon[0] = 0;
    if (mean && mcap) mean[0] = 0;
    if (ex && xcap) ex[0] = 0;
    service_local_date(today, sizeof today);
    if (datebuf && dcap) snprintf(datebuf, dcap, "%s", today);
    if (!g_v2_ready) return;
    n = pv2_planlist_pick(&g_v2.store, today, rows, cap, ldate, sizeof ldate, &goalv);
    if (n <= 0) return;   /* 无当天数据：页面走「等待同步」占位 */
    if (datebuf && dcap) snprintf(datebuf, dcap, "%s", ldate);
    *stale = strcmp(ldate, today) != 0;
    for (i = 0; i < n && i < cap; i++) {
        if (!strcmp(rows[i].state, "remembered")) ndone++;
        else if (!strcmp(rows[i].state, "review")) nreview++;
        if (cur < 0 && !strcmp(rows[i].state, "pending")) cur = i;
    }
    if (cur < 0)
        for (i = 0; i < n && i < cap; i++)
            if (!strcmp(rows[i].state, "review")) { cur = i; break; }
    if (cur < 0) cur = 0;
    *total = n;              /* 实际清单数（完成数/实际清单数 的分母） */
    *index = cur + 1;
    *done = ndone;
    *review = nreview;
    *goal = goalv;
    if (g_v2.store.p_get_payload(g_v2.store.h, PV2_ENT_WORD, rows[cur].word_id,
                                 payload, sizeof payload - 1, &plen, &rev, &del) != 1)
        return;              /* 词未同步/已删：进度仍真，单词卡由页面占位 */
    payload[plen < sizeof payload - 1 ? plen : sizeof payload - 1] = 0;
    cJSON *o = cJSON_Parse(payload);
    const char *t = o ? cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, "text")) : NULL;
    if (t && *t) {
        snprintf(text, tcap, "%s", t);
        /* F2: 注释实体与词同 id；读到即按快照字段装配（utf8_fit 截断保码点完整，
         * 尾加 … 提示截断）；无注释/损坏 → 恒空串占位。 */
        /* R1: 手机端 NotePayload 总长上限 3072B（TRAE-F2），读入缓冲必须对齐，
         * 否则长注释在板端读 payload 时就被截断（此前 1024B）。 */
        char np[3200];
        size_t nl = 0;
        long nrev = 0;
        int ndel = 0;
        if (g_v2.store.p_get_payload &&
            g_v2.store.p_get_payload(g_v2.store.h, PV2_ENT_ANNOTATION, rows[cur].word_id,
                                     np, sizeof np - 1, &nl, &nrev, &ndel) == 1 && !ndel) {
            np[nl < sizeof np - 1 ? nl : sizeof np - 1] = 0;
            cJSON *an = cJSON_Parse(np);
            if (an) {
                const char *jph = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(an, "phonetic"));
                const char *jmn = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(an, "meaning"));
                const char *jex = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(an, "example"));
                if (jph && *jph) { size_t f = utf8_fit(jph, phcap ? phcap - 1 : 0);
                    if (f < strlen(jph)) { memcpy(phon, jph, f); snprintf(phon + f, phcap - f, "…"); }
                    else snprintf(phon, phcap, "%s", jph); }
                if (jmn && *jmn) { size_t f = utf8_fit(jmn, mcap ? mcap - 1 : 0);
                    if (f < strlen(jmn)) { memcpy(mean, jmn, f); snprintf(mean + f, mcap - f, "…"); }
                    else snprintf(mean, mcap, "%s", jmn); }
                if (jex && *jex) { size_t f = utf8_fit(jex, xcap ? xcap - 1 : 0);
                    if (f < strlen(jex)) { memcpy(ex, jex, f); snprintf(ex + f, xcap - f, "…"); }
                    else snprintf(ex, xcap, "%s", jex); }
            }
            cJSON_Delete(an);
        }
    }
    cJSON_Delete(o);
}

/* M5-B: briefing-page snapshot producer (read-only over pv2 briefing
 * entities). The phone pushes one record per paragraph slot with the exact id
 * "brief-<YYYY-MM-DD>-<slot>" (slot 0..3 = plan/review/weather/tip), so
 * accepting only that exact 18-byte form keeps conflict copies ("...@op_id"
 * suffix) and other days out without touching the pv2 pull route. Empty
 * paragraphs are skipped; each item is capped (UTF-8-safe) to keep the whole
 * snapshot small — the full text stays on the phone (实施方案 41 行). */
#define BRIEF_ITEM_CAP 768
static const char *brief_labels[4] = {"今日安排", "回顾", "天气", "小贴士"};
/* Longest prefix of s made of complete UTF-8 sequences and at most cap bytes. */
static size_t utf8_fit(const char *s, size_t cap) {
    size_t i = 0;
    while (s[i]) {
        unsigned char c = (unsigned char)s[i];
        size_t len = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 :
                     (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 1;
        if (i + len > cap) break;
        for (size_t k = 1; k < len; k++)
            if (((unsigned char)s[i + k] & 0xC0) != 0x80) return i;  /* 截断码点 */
        i += len;
    }
    return i;
}
static int briefing_snapshot(cJSON *br) {
    if (!g_v2_ready) return 0;
    char today[16];
    pv2_date_of(time(NULL), today, sizeof today);
    if (strcmp(today, briefing_day) != 0) {          /* 业务日切换：重置分页 */
        snprintf(briefing_day, sizeof briefing_day, "%s", today);
        briefing_part = 0; briefing_pinned = 0;
        briefing_advance_ms = monotonic_ms() + 8000;
    }
    int n = pv2_store_count(&g_v2.store, PV2_ENT_BRIEFING, 0);
    if (n <= 0) { briefing_pages = 0; return 0; }
    static char texts[4][BRIEF_ITEM_CAP];
    int have[4] = {0, 0, 0, 0}, trunc[4] = {0, 0, 0, 0};
    pv2_entity_info info;
    for (long off = 0; off < n; off++) {
        if (g_v2.store.p_list(g_v2.store.h, PV2_ENT_BRIEFING, off, &info, 1) != 1) break;
        if (info.deleted || strlen(info.id) != 18 || strncmp(info.id, "brief-", 6) ||
            strncmp(info.id + 6, today, 10) || info.id[16] != '-' ||
            info.id[17] < '0' || info.id[17] > '3') continue;
        int slot = info.id[17] - '0';
        if (have[slot]) continue;
        char payload[PV2_OP_PAYLOAD_BYTES];
        size_t plen = 0; long rev = 0; int del = 0;
        if (g_v2.store.p_get_payload(g_v2.store.h, PV2_ENT_BRIEFING, info.id,
                                     payload, sizeof payload - 1, &plen, &rev, &del) != 1) continue;
        payload[plen < sizeof payload - 1 ? plen : sizeof payload - 1] = 0;
        cJSON *o = cJSON_Parse(payload);
        const char *p = o ? cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, "p")) : NULL;
        if (p && *p) {
            size_t bl = strlen(p);
            if (bl >= BRIEF_ITEM_CAP - 4) {           /* 留 "…"(3B)+NUL；全文在手机 */
                size_t fit = utf8_fit(p, BRIEF_ITEM_CAP - 4);
                memcpy(texts[slot], p, fit);
                memcpy(texts[slot] + fit, "…", 4);
                trunc[slot] = 1;
            } else {
                memcpy(texts[slot], p, bl + 1);
            }
            have[slot] = 1;
        }
        cJSON_Delete(o);
    }
    int count = have[0] + have[1] + have[2] + have[3];
    if (!count) { briefing_pages = 0; return 0; }
    briefing_pages = (count + 2) / 3;                 /* 每屏 2–3 条（41 行） */
    if (briefing_part < 0) briefing_part = 0;
    if (briefing_part >= briefing_pages) briefing_part = briefing_pages - 1;
    cJSON_AddStringToObject(br, "date", today);
    cJSON_AddNumberToObject(br, "pages", briefing_pages);
    cJSON_AddNumberToObject(br, "part", briefing_part);
    cJSON_AddNumberToObject(br, "total", count);      /* 全部条数（分页窗口外也要给对） */
    cJSON *items = cJSON_AddArrayToObject(br, "items");
    int base = briefing_part * 3, idx = 0;
    for (int slot = 0; slot < 4; slot++) {
        if (!have[slot]) continue;
        if (idx >= base && idx < base + 3) {
            cJSON *it = cJSON_CreateObject();
            cJSON_AddNumberToObject(it, "slot", slot);
            cJSON_AddStringToObject(it, "label", brief_labels[slot]);
            cJSON_AddStringToObject(it, "text", texts[slot]);
            cJSON_AddBoolToObject(it, "truncated", trunc[slot]);
            cJSON_AddItemToArray(items, it);
        }
        idx++;
    }
    return 1;
}

/* N6: timetable-page snapshot producer (read-only over pv2 course entities,
 * 续工方案 §4.2). The phone materializes course_occurrence rows for the 14-day
 * window plus one schedule_window row (id "schedule-window"); the generation is
 * ACTIVE only when the live row count matches the declared window count
 * (pv2_window_state). Active generation rows are shown while the window is
 * complete; while syncing the older-generation rows stay displayable (never a
 * half-new table).  Today's rows are pre-paged (≤3 per screen) the same way as
 * the briefing page; the big "current" row is the first today row not yet over
 * (end > now), or the earliest future-date row when today has no classes, or a
 * status text. Absent (no live window row) -> page shows its waiting placeholder. */
typedef struct {
    char date[12];
    char name[96];
    char start[8];
    char end[8];
    char room[48];
} tt_row_t;
#define TT_ROW_MAX 512
#define TT_PAGE_N 3
static int tt_row_cmp(const void *a, const void *b) {
    const tt_row_t *x = a, *y = b;
    int c = strcmp(x->date, y->date);
    if (c) return c;
    return strcmp(x->start, y->start);
}
/* "YYYY-MM-DD" -> "MM.DD 周X"（panel_font_zh16 覆盖；非法输入原样回退） */
static void format_md_dow(const char *ymd, char *out, size_t n) {
    int y = 0, m = 0, d = 0;
    if (sscanf(ymd, "%d-%d-%d", &y, &m, &d) == 3 && y >= 1900) {
        struct tm t;
        memset(&t, 0, sizeof t);
        t.tm_year = y - 1900; t.tm_mon = m - 1; t.tm_mday = d;
        time_t tt = mktime(&t);
        struct tm lt;
        if (localtime_r(&tt, &lt) && lt.tm_wday >= 0 && lt.tm_wday <= 6) {
            static const char *wd = "日一二三四五六";
            snprintf(out, n, "%02d.%02d 周%c", lt.tm_mon + 1, lt.tm_mday, wd[lt.tm_wday]);
            return;
        }
    }
    snprintf(out, n, "%s", ymd);
}
static int timetable_snapshot(cJSON *tt) {
    if (!g_v2_ready) return 0;
    pv2_window_info win;
    if (pv2_window_state(&g_v2.store, &win) != 1 || !win.have_window) return 0;
    char today[16];
    service_local_date(today, sizeof today);
    if (strcmp(today, timetable_day) != 0) {          /* 业务日切换：重置分页 */
        snprintf(timetable_day, sizeof timetable_day, "%s", today);
        timetable_part = 0; timetable_pinned = 0;
        timetable_advance_ms = monotonic_ms() + 8000;
    }
    static char pbuf[PV2_OP_PAYLOAD_BYTES + 1];
    pv2_entity_info infos[64];
    static tt_row_t rows[TT_ROW_MAX];
    int nrows = 0;
    long scanned = 0;
    for (;;) {
        int got = g_v2.store.p_list(g_v2.store.h, PV2_ENT_COURSE, scanned, infos, 64);
        if (got <= 0) break;
        for (int i = 0; i < got; i++) {
            if (infos[i].deleted) continue;           /* F2-D1: p_list 物理游标，墓碑跳过 */
            size_t plen = 0; long rev = 0; int del = 0;
            if (g_v2.store.p_get_payload(g_v2.store.h, PV2_ENT_COURSE, infos[i].id,
                                         pbuf, sizeof pbuf - 1, &plen, &rev, &del) != 1 || del)
                continue;
            pbuf[plen < sizeof pbuf - 1 ? plen : sizeof pbuf - 1] = 0;
            cJSON *o = cJSON_Parse(pbuf);
            const char *g = o ? cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, "generation")) : NULL;
            int keep = g && *g && (win.active ? strcmp(g, win.generation) == 0
                                               : strcmp(g, win.generation) != 0);
            if (keep && nrows < TT_ROW_MAX) {
                const char *d = o ? cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, "date")) : NULL;
                const char *s = o ? cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, "start")) : NULL;
                const char *e = o ? cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, "end")) : NULL;
                const char *n = o ? cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, "name")) : NULL;
                const char *r = o ? cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, "room")) : NULL;
                if (d && *d && s && *s) {
                    snprintf(rows[nrows].date, sizeof rows[0].date, "%s", d);
                    snprintf(rows[nrows].name, sizeof rows[0].name, "%s", n ? n : "");
                    snprintf(rows[nrows].start, sizeof rows[0].start, "%s", s);
                    snprintf(rows[nrows].end, sizeof rows[0].end, "%s", e ? e : "");
                    snprintf(rows[nrows].room, sizeof rows[0].room, "%s", r ? r : "");
                    nrows++;
                }
            }
            cJSON_Delete(o);
        }
        scanned += got;
    }
    cJSON_AddStringToObject(tt, "date", today);
    cJSON_AddStringToObject(tt, "table", win.table);
    cJSON_AddNumberToObject(tt, "active", win.active ? 1 : 0);
    cJSON_AddNumberToObject(tt, "syncing", win.active ? 0 : 1);
    if (nrows <= 0) {                                  /* 整窗无行：同步中/今日无课占位 */
        timetable_pages = 1;
        cJSON_AddNumberToObject(tt, "today", 0);
        cJSON_AddNumberToObject(tt, "pages", 1);
        cJSON_AddNumberToObject(tt, "part", 0);
        cJSON *cur = cJSON_CreateObject();
        if (cur) {
            cJSON_AddStringToObject(cur, "name", win.active ? "今日无课" : "课程同步中");
            cJSON_AddStringToObject(cur, "start", "");
            cJSON_AddStringToObject(cur, "end", "");
            cJSON_AddStringToObject(cur, "room", "");
            cJSON_AddStringToObject(cur, "label", "");
            cJSON_AddItemToObject(tt, "current", cur);
        }
        cJSON_AddItemToObject(tt, "items", cJSON_CreateArray());
        return 1;
    }
    qsort(rows, (size_t)nrows, sizeof rows[0], tt_row_cmp);
    int ntoday = 0;
    for (int i = 0; i < nrows; i++) if (strcmp(rows[i].date, today) == 0) ntoday++;
    timetable_pages = ntoday > 0 ? (ntoday + 2) / TT_PAGE_N : 1;
    if (timetable_part < 0) timetable_part = 0;
    if (timetable_part >= timetable_pages) timetable_part = timetable_pages - 1;
    cJSON_AddNumberToObject(tt, "today", ntoday);
    cJSON_AddNumberToObject(tt, "pages", timetable_pages);
    cJSON_AddNumberToObject(tt, "part", timetable_part);
    /* 大卡 current：今天首个未下课（end>now）行；今天全过→状态文案；
     * 今天无课→最早未来日期首行（label=日期）；无任何行→占位文案。 */
    char nowt[8];
    time_t tnow = time(NULL);
    struct tm lnow;
    if (localtime_r(&tnow, &lnow)) strftime(nowt, sizeof nowt, "%H:%M", &lnow);
    else snprintf(nowt, sizeof nowt, "00:00");
    cJSON *cur = cJSON_CreateObject();
    int cur_set = 0;
    for (int i = 0; i < nrows && strcmp(rows[i].date, today) <= 0; i++) {
        if (strcmp(rows[i].date, today) != 0) continue;   /* 昨天及以前：跳过 */
        if (strcmp(rows[i].end, nowt) > 0) {
            cJSON_AddStringToObject(cur, "name", rows[i].name);
            cJSON_AddStringToObject(cur, "start", rows[i].start);
            cJSON_AddStringToObject(cur, "end", rows[i].end);
            cJSON_AddStringToObject(cur, "room", rows[i].room);
            int seq = 0;
            for (int k = 0; k < i; k++) if (strcmp(rows[k].date, today) == 0) seq++;
            char lab[24];
            snprintf(lab, sizeof lab, "第%d节", seq + 1);
            cJSON_AddStringToObject(cur, "label", lab);
            cur_set = 1;
            break;
        }
    }
    if (!cur_set) {
        if (ntoday > 0) {                                 /* 今天全过 */
            cJSON_AddStringToObject(cur, "name", "今日课程已结束");
            cur_set = 1;
        } else {                                          /* 今天无课：最早未来日期首行 */
            for (int i = 0; i < nrows; i++) {
                if (strcmp(rows[i].date, today) > 0) {
                    cJSON_AddStringToObject(cur, "name", rows[i].name);
                    cJSON_AddStringToObject(cur, "start", rows[i].start);
                    cJSON_AddStringToObject(cur, "end", rows[i].end);
                    cJSON_AddStringToObject(cur, "room", rows[i].room);
                    char lab[16];
                    format_md_dow(rows[i].date, lab, sizeof lab);
                    cJSON_AddStringToObject(cur, "label", lab);
                    cur_set = 1;
                    break;
                }
            }
            if (!cur_set) {
                cJSON_AddStringToObject(cur, "name", "暂无课程安排");
            }
        }
    }
    if (cur_set && !cJSON_GetObjectItemCaseSensitive(cur, "start")) {
        cJSON_AddStringToObject(cur, "start", "");
        cJSON_AddStringToObject(cur, "end", "");
        cJSON_AddStringToObject(cur, "room", "");
        cJSON_AddStringToObject(cur, "label", "");
    }
    cJSON_AddItemToObject(tt, "current", cur);
    /* items：今日行分页窗口 ≤3 */
    cJSON *items = cJSON_AddArrayToObject(tt, "items");
    int base = timetable_part * TT_PAGE_N, idx = 0;
    for (int i = 0; i < nrows && idx < base + TT_PAGE_N; i++) {
        if (strcmp(rows[i].date, today) != 0) continue;
        if (idx >= base) {
            cJSON *it = cJSON_CreateObject();
            if (it) {
                cJSON_AddStringToObject(it, "name", rows[i].name);
                cJSON_AddStringToObject(it, "start", rows[i].start);
                cJSON_AddStringToObject(it, "end", rows[i].end);
                cJSON_AddStringToObject(it, "room", rows[i].room);
                cJSON_AddItemToArray(items, it);
            } else cJSON_Delete(it);
        }
        idx++;
    }
    return 1;
}

/* N6: plans-page snapshot producer (read-only over pv2 plan_item entities,
 * 续工方案 §4.2). The board only shows dated unfinished items that are due
 * today or overdue (undated / future / finished items stay on the phone's
 * "全部" list). Pre-paged (≤3 per screen); overdue items sort first. Absent
 * (no plan_item rows at all) -> page shows its waiting placeholder. */
typedef struct {
    char title[96];
    char due_date[12];
    char due_time[8];
    int priority;
    int overdue;
} pl_item_t;
#define PL_ROW_MAX 256
#define PL_PAGE_N 3
static int pl_row_cmp(const void *a, const void *b) {
    const pl_item_t *x = a, *y = b;
    int c = strcmp(x->due_date, y->due_date);   /* 逾期（due<today）自然在前 */
    if (c) return c;
    c = strcmp(x->due_time, y->due_time);
    if (c) return c;
    return y->priority - x->priority;           /* 重要在前 */
}
static int plans_snapshot(cJSON *pl) {
    if (!g_v2_ready) return 0;
    char today[16];
    service_local_date(today, sizeof today);
    if (strcmp(today, plans_day) != 0) {          /* 业务日切换：重置分页 */
        snprintf(plans_day, sizeof plans_day, "%s", today);
        plans_part = 0; plans_pinned = 0;
        plans_advance_ms = monotonic_ms() + 8000;
    }
    static char pbuf[PV2_OP_PAYLOAD_BYTES + 1];
    pv2_entity_info infos[64];
    static pl_item_t rows[PL_ROW_MAX];
    int nrows = 0, total = 0, ndue_over = 0, ndone_today = 0, any_rows = 0;
    long scanned = 0;
    for (;;) {
        int got = g_v2.store.p_list(g_v2.store.h, PV2_ENT_PLAN_ITEM, scanned, infos, 64);
        if (got <= 0) break;
        for (int i = 0; i < got; i++) {
            if (infos[i].deleted) continue;           /* F2-D1: p_list 物理游标，墓碑跳过 */
            size_t plen = 0; long rev = 0; int del = 0;
            if (g_v2.store.p_get_payload(g_v2.store.h, PV2_ENT_PLAN_ITEM, infos[i].id,
                                         pbuf, sizeof pbuf - 1, &plen, &rev, &del) != 1 || del)
                continue;
            pbuf[plen < sizeof pbuf - 1 ? plen : sizeof pbuf - 1] = 0;
            cJSON *o = cJSON_Parse(pbuf);
            if (!o) continue;
            any_rows = 1;
            const char *dd = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, "due_date"));
            cJSON *jd = cJSON_GetObjectItemCaseSensitive(o, "done");
            int it_done = jd && cJSON_IsNumber(jd) ? jd->valueint : 0;
            cJSON *jc = cJSON_GetObjectItemCaseSensitive(o, "completed_at");
            long long comp = jc && cJSON_IsNumber(jc) ? (long long)jc->valuedouble : 0;
            if (it_done && comp > 0) {                /* 今日完成的项（任意日期） */
                time_t ct = (time_t)comp;
                struct tm cm;
                if (localtime_r(&ct, &cm)) {
                    char cdate[16];
                    snprintf(cdate, sizeof cdate, "%04d-%02d-%02d",
                             cm.tm_year + 1900, cm.tm_mon + 1, cm.tm_mday);
                    if (strcmp(cdate, today) == 0) ndone_today++;
                }
            }
            /* 显示集：dated 且未完成且 due<=today（今天或逾期） */
            if (!it_done && dd && *dd && strcmp(dd, today) <= 0) {
                total++;
                if (strcmp(dd, today) < 0) ndue_over++;
                if (nrows < PL_ROW_MAX) {
                    pl_item_t *r = &rows[nrows];
                    snprintf(r->title, sizeof r->title, "%s",
                             cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, "title")) ?: "");
                    snprintf(r->due_date, sizeof r->due_date, "%s", dd);
                    snprintf(r->due_time, sizeof r->due_time, "%s",
                             cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, "due_time")) ?: "");
                    cJSON *jp = cJSON_GetObjectItemCaseSensitive(o, "priority");
                    r->priority = jp && cJSON_IsNumber(jp) ? jp->valueint : 0;
                    r->overdue = strcmp(dd, today) < 0 ? 1 : 0;
                    nrows++;
                }
            }
            cJSON_Delete(o);
        }
        scanned += got;
    }
    if (!any_rows) return 0;                          /* 无任何待办行：页面走等待同步占位 */
    cJSON_AddStringToObject(pl, "date", today);
    cJSON_AddNumberToObject(pl, "total", total);
    cJSON_AddNumberToObject(pl, "done", ndone_today);
    cJSON_AddNumberToObject(pl, "overdue", ndue_over);
    plans_pages = total > 0 ? (total + 2) / PL_PAGE_N : 1;
    if (plans_part < 0) plans_part = 0;
    if (plans_part >= plans_pages) plans_part = plans_pages - 1;
    cJSON_AddNumberToObject(pl, "pages", plans_pages);
    cJSON_AddNumberToObject(pl, "part", plans_part);
    qsort(rows, (size_t)nrows, sizeof rows[0], pl_row_cmp);
    cJSON *arr = cJSON_AddArrayToObject(pl, "items");
    int base = plans_part * PL_PAGE_N;
    for (int i = base; i < nrows && i < base + PL_PAGE_N; i++) {
        cJSON *it = cJSON_CreateObject();
        if (it) {
            cJSON_AddStringToObject(it, "title", rows[i].title);
            cJSON_AddStringToObject(it, "due_date", rows[i].due_date);
            cJSON_AddStringToObject(it, "due_time", rows[i].due_time);
            cJSON_AddNumberToObject(it, "done", 0);
            cJSON_AddNumberToObject(it, "priority", rows[i].priority);
            cJSON_AddNumberToObject(it, "overdue", rows[i].overdue);
            cJSON_AddItemToArray(arr, it);
        } else cJSON_Delete(it);
    }
    return 1;
}

/* push 请求体 → pv2_op 数组（线上格式= pv2_pack_push/enc_op 的输出：
 * {"client_id","ops":[{"op_id","entity","id","base_revision","deleted",
 * "retry_count","payload"}]}）。payload 用 cJSON 重新序列化，副本指针记入
 * payloads[] 由调用方在 dispatch 后释放。数组容量=上限+1，超量自然触发
 * api 层 "too many ops" 400，与契约一致。 */
static void v2_parse_push(const char *body, char *client_id, size_t cid_cap,
                          pv2_op *ops, int max_ops, char **payloads, int *n_out) {
    cJSON *root = cJSON_Parse(body);
    if (!root) return;
    cJSON *cid = cJSON_GetObjectItem(root, "client_id");
    if (cid && cJSON_IsString(cid) && cid->valuestring)
        snprintf(client_id, cid_cap, "%s", cid->valuestring);
    cJSON *arr = cJSON_GetObjectItem(root, "ops");
    int n = 0;
    if (cJSON_IsArray(arr)) {
        cJSON *item;
        cJSON_ArrayForEach(item, arr) {
            if (n >= max_ops) break;
            pv2_op *op = &ops[n];
            memset(op, 0, sizeof *op);
            cJSON *v;
            if ((v = cJSON_GetObjectItem(item, "op_id")) && cJSON_IsString(v) && v->valuestring)
                snprintf(op->op_id, sizeof op->op_id, "%s", v->valuestring);
            if ((v = cJSON_GetObjectItem(item, "id")) && cJSON_IsString(v) && v->valuestring)
                snprintf(op->entity_id, sizeof op->entity_id, "%s", v->valuestring);
            if ((v = cJSON_GetObjectItem(item, "entity")) && cJSON_IsString(v) && v->valuestring)
                op->entity = pv2_entity_from_name(v->valuestring);
            if ((v = cJSON_GetObjectItem(item, "base_revision")) && cJSON_IsNumber(v))
                op->base_revision = (long)v->valuedouble;
            if ((v = cJSON_GetObjectItem(item, "deleted"))) op->deleted = cJSON_IsTrue(v) ? 1 : 0;
            if ((v = cJSON_GetObjectItem(item, "retry_count")) && cJSON_IsNumber(v))
                op->retry_count = (int)v->valuedouble;
            if ((v = cJSON_GetObjectItem(item, "payload")) && cJSON_IsObject(v)) {
                char *txt = cJSON_PrintUnformatted(v);
                if (txt) {
                    op->payload = txt;
                    op->payload_len = strlen(txt);
                    payloads[n] = txt;
                }
            }
            n++;
        }
    }
    cJSON_Delete(root);
    *n_out = n;
}

/* /api/v2/ 请求处理：路径→route 枚举 → pv2_dispatch 单入口 → send_response。 */
static void handle_v2(int client, const char *method, const char *target, const char *request) {
    char out[PV2_MAX_RESP_JSON + 256];
    char status_text[32];
    char route_path[768] = {0};
    char auth_header[256] = {0};
    char client_id[64] = {0};
    char cursor[128] = {0}, date[32] = {0}, job_id[96] = {0};
    int limit = 0, part = 0;
    pv2_request req;
    pv2_api_err err;
    long rc;

    /* 路由路径与 query 拆分（target 形如 /api/v2/sync/pull?cursor=...） */
    const char *q = strchr(target, '?');
    snprintf(route_path, sizeof route_path, "%.*s",
             (int)(q ? (size_t)(q - target) : strlen(target)), target);

    /* /api/v2/health：本卡新增的轻量心跳探针，not-ready 时也可观测 */
    if (!strcmp(route_path, "/api/v2/health")) {
        char probe[192];
        if (g_v2_ready) {
            long last = pv2_store_last_seq(&g_v2.store);
            long live = pv2_store_count(&g_v2.store, PV2_ENT_WORD, 0);
            snprintf(probe, sizeof probe, "{\"ok\":true,\"v2\":true,\"version\":\"%s\","
                     "\"last_seq\":%ld,\"words\":%ld}", APP_VERSION, last, live);
        } else {
            snprintf(probe, sizeof probe, "{\"ok\":true,\"v2\":false}");
        }
        send_response(client, "200 OK", "application/json; charset=utf-8", probe);
        return;
    }

    if (!g_v2_ready) {   /* 故障不阻塞显示：v2 降级统一 503，v1 不受影响 */
        send_response(client, "503 Service Unavailable", "application/json; charset=utf-8",
                      "{\"ok\":false,\"code\":\"storage_failure\",\"message\":"
                      "\"v2 storage unavailable\",\"http_status\":503,\"detail\":{},"
                      "\"retryable\":true,\"resync_required\":false}");
        return;
    }

    /* query 参数（复用 v1 的 query_value：找到=0 并已 URL 解码，缺失=-1） */
    if (q) {
        char num[16];
        if (query_value(target, "cursor", cursor, sizeof cursor) != 0) cursor[0] = 0;
        if (query_value(target, "date", date, sizeof date) != 0) date[0] = 0;
        if (query_value(target, "job_id", job_id, sizeof job_id) != 0) job_id[0] = 0;
        if (query_value(target, "limit", num, sizeof num) == 0) limit = atoi(num);
        if (query_value(target, "part", num, sizeof num) == 0) part = atoi(num);
    }
    /* ai/jobs/<job_id> 路径参数（query 之外的第二来源） */
    if (!strncmp(route_path, "/api/v2/ai/jobs/", 16)) {
        const char *id = route_path + 16;
        const char *slash = strchr(id, '/');
        if (slash && !strcmp(slash, "/cancel")) {
            snprintf(job_id, sizeof job_id, "%.*s", (int)(slash - id), id);
        } else if (!slash) {
            snprintf(job_id, sizeof job_id, "%s", id);
        }
    }

    /* 路径 → route 枚举 */
    memset(&req, 0, sizeof req);
    if (!strcmp(route_path, "/api/v2/capabilities"))          req.route = PV2_ROUTE_CAPABILITIES;
    else if (!strcmp(route_path, "/api/v2/pair"))             req.route = PV2_ROUTE_PAIR;
    else if (!strcmp(route_path, "/api/v2/sync/status"))      req.route = PV2_ROUTE_SYNC_STATUS;
    else if (!strcmp(route_path, "/api/v2/sync/push"))        req.route = PV2_ROUTE_SYNC_PUSH;
    else if (!strcmp(route_path, "/api/v2/sync/pull"))        req.route = PV2_ROUTE_SYNC_PULL;
    else if (!strcmp(route_path, "/api/v2/words"))            req.route = PV2_ROUTE_WORDS;
    else if (!strcmp(route_path, "/api/v2/daily-plan"))       req.route = PV2_ROUTE_DAILY_PLAN;
    else if (!strcmp(route_path, "/api/v2/briefings"))        req.route = PV2_ROUTE_BRIEFINGS;
    else if (!strcmp(route_path, "/api/v2/settings"))
        req.route = !strcmp(method, "POST") ? PV2_ROUTE_SETTINGS_POST : PV2_ROUTE_SETTINGS_GET;
    else if (!strcmp(route_path, "/api/v2/auth/token/revoke")) req.route = PV2_ROUTE_TOKEN_REVOKE;
    else if (!strcmp(route_path, "/api/v2/ai/jobs") && !strcmp(method, "POST"))
        req.route = PV2_ROUTE_AI_JOB_CREATE;
    else if (!strncmp(route_path, "/api/v2/ai/jobs/", 16))
        req.route = strstr(route_path + 16, "/cancel") ? PV2_ROUTE_AI_JOB_CANCEL : PV2_ROUTE_AI_JOB_GET;
    if (req.route == 0) {
        send_response(client, "404 Not Found", "application/json; charset=utf-8",
                      "{\"ok\":false,\"code\":\"not_found\",\"message\":\"unknown v2 route\","
                      "\"http_status\":404,\"detail\":{},\"retryable\":false,"
                      "\"resync_required\":false}");
        return;
    }

    memset(&err, 0, sizeof err);
    const char *body = strstr(request, "\r\n\r\n");
    body = body ? body + 4 : NULL;
    v2_header_value(request, "Authorization:", auth_header, sizeof auth_header);

    req.method = method;
    req.uri = target;                       /* 完整 target：auth 层做 token-in-URL 检查 */
    req.auth_header = auth_header[0] ? auth_header : NULL;
    req.body = body;
    req.body_len = body ? strlen(body) : 0;
    req.cursor = cursor[0] ? cursor : NULL;
    req.limit = limit;
    req.part = part;
    req.date = date[0] ? date : NULL;
    req.job_id = job_id[0] ? job_id : NULL;

    /* push：api 层要求 ops 预解析（panel_v2_api.c r_sync_push），这里解析线上体 */
    pv2_op ops[PV2_PACK_MAX_OPS + 1];
    char *payloads[PV2_PACK_MAX_OPS + 1] = {0};
    int n_ops = 0;
    if (req.route == PV2_ROUTE_SYNC_PUSH) {
        if (!body || !*body) {
            send_response(client, "400 Bad Request", "application/json; charset=utf-8",
                          "{\"ok\":false,\"code\":\"bad_request\",\"message\":"
                          "\"push body required\",\"http_status\":400,\"detail\":{},"
                          "\"retryable\":false,\"resync_required\":false}");
            return;
        }
        v2_parse_push(body, client_id, sizeof client_id, ops, (int)PV2_PACK_MAX_OPS + 1,
                      payloads, &n_ops);
        req.client_id = client_id[0] ? client_id : NULL;
        req.ops = ops;
        req.n_ops = n_ops;
    }

    rc = pv2_dispatch(&g_v2, &req, out, sizeof out, &err);
    for (int i = 0; i < n_ops; i++) free(payloads[i]);
    if (rc < 0) {   /* 响应超 32 KiB 等编码失败：R2 红线，按 500 拒答 */
        send_response(client, "500 Internal Server Error", "application/json; charset=utf-8",
                      "{\"ok\":false,\"code\":\"storage_failure\",\"message\":"
                      "\"response encoding failed\",\"http_status\":500,\"detail\":{},"
                      "\"retryable\":true,\"resync_required\":false}");
        return;
    }
    out[rc] = '\0';   /* 缓冲留有 +256 余量，安全 */
    snprintf(status_text, sizeof status_text, "%s", v2_http_text(err.http ? err.http : 200));
    send_response(client, status_text, "application/json; charset=utf-8", out);
}

static void handle_client(int client, framebuffer_t *fb, home_state_t *state) {
    /* G3: wallpaper upload bypasses the 4096 B JSON reader (PLAN §7). The peek
     * does not consume the socket for any other request; on a match the
     * headers are drained and the body is streamed straight to staging. */
    {
        char wp_hdr[2048];
        if (wallpaper_peek_upload(client, wp_hdr, sizeof(wp_hdr)) == 1) {
            wallpaper_upload_stream(client, wp_hdr, state);
            return;
        }
    }
    char request[8192];
    char method[8] = {0};
    char path[768] = {0};
    const char *rr_reason = "closed";
    long rr_decl = -1;
    ssize_t count = read_request(client, request, sizeof(request), &rr_reason, &rr_decl);
    if (count <= 0) {
        /* T03B2 / ACT-01: never drop an invalid request silently — answer
         * 413/400 with the unified error shape so a client can tell capacity
         * errors apart from network failures. Timeouts and peer closes still
         * just release the connection. */
        if (rr_reason && !strcmp(rr_reason, "too_large")) {
            char body[256];
            long b = rr_decl > 0 ? rr_decl : (long)sizeof(request);
            snprintf(body, sizeof(body),
                     "{\"ok\":false,\"code\":\"payload_too_large\",\"message\":"
                     "\"request body exceeds 4096 B limit\",\"http_status\":413,"
                     "\"detail\":{\"bytes\":%ld,\"bytes_max\":4096},"
                     "\"retryable\":true,\"resync_required\":false}", b);
            send_response(client, "413 Payload Too Large",
                          "application/json; charset=utf-8", body);
        } else if (rr_reason && !strcmp(rr_reason, "bad_chunked")) {
            send_response(client, "400 Bad Request", "application/json; charset=utf-8",
                          "{\"ok\":false,\"code\":\"bad_request\",\"message\":"
                          "\"Transfer-Encoding: chunked is not supported\",\"http_status\":400,"
                          "\"detail\":{},\"retryable\":false,\"resync_required\":false}");
        } else if (rr_reason && !strcmp(rr_reason, "bad_headers")) {
            send_response(client, "400 Bad Request", "application/json; charset=utf-8",
                          "{\"ok\":false,\"code\":\"bad_request\",\"message\":"
                          "\"malformed Content-Length header\",\"http_status\":400,"
                          "\"detail\":{},\"retryable\":false,\"resync_required\":false}");
        }
        return;
    }
    request[count] = '\0';
    if (sscanf(request, "%7s %767s", method, path) != 2) return;
    if (demo_route(client, method, path, request, state)) return;

    if (!strcmp(path,"/app.css") || !strcmp(path,"/app.js")) {
        send_file_response(client, !strcmp(path,"/app.css") ? "/mnt/UDISK/panel-v3/app.css" : "/mnt/UDISK/panel-v3/app.js",
            !strcmp(path,"/app.css") ? "text/css; charset=utf-8" : "application/javascript; charset=utf-8", NULL);
        return;
    }
    if (handle_remote(client, method, path, request, state)) return;

    /* M3-D：/api/v2/ 前缀走 pv2 存储桥接；v1 其余路由零改动。 */
    if (strncmp(path, "/api/v2/", 8) == 0) {
        handle_v2(client, method, path, request);
        return;
    }

    if (is_command_path(path)) {
        if (strcmp(method, "POST") != 0) {
            send_response(client, "405 Method Not Allowed", "application/json; charset=utf-8",
                          "{\"error\":\"POST required\"}");
            return;
        }
        if (!control_pin_valid(request, state)) {
            send_response(client, "403 Forbidden", "application/json; charset=utf-8",
                          "{\"error\":\"invalid control pin\"}");
            return;
        }
    }

    if (strncmp(path, "/api/config/names?", 18) == 0) {
        if (update_device_names(state, path) < 0) {
            send_response(client, "400 Bad Request", "application/json; charset=utf-8",
                          "{\"error\":\"invalid device names\"}");
        } else {
            send_state(client, state);
        }
        return;
    }

    if (strncmp(path, "/api/config/info?", 17) == 0) {
        if (update_info_config(state, path) < 0) {
            send_response(client, "400 Bad Request", "application/json; charset=utf-8",
                          "{\"error\":\"invalid information\"}");
        } else {
            draw_ui(fb, state);
            refresh_display(fb);
            send_state(client, state);
        }
        return;
    }

    if (strncmp(path, "/api/config/weather?", 20) == 0) {
        if (update_weather_config(state, path) < 0) {
            send_response(client, "400 Bad Request", "application/json; charset=utf-8",
                          "{\"error\":\"invalid weather\"}");
        } else {
            draw_ui(fb, state);
            refresh_display(fb);
            send_state(client, state);
        }
        return;
    }

    if (strncmp(path, "/api/config/sensors?", 20) == 0) {
        if (update_sensor_config(state, path) < 0) {
            send_response(client, "400 Bad Request", "application/json; charset=utf-8",
                          "{\"error\":\"invalid sensors\"}");
        } else {
            send_state(client, state);
        }
        return;
    }

    if (strncmp(path, "/api/config/display?", 20) == 0) {
        if (update_display_config(path) < 0) {
            send_response(client, "400 Bad Request", "application/json; charset=utf-8",
                          "{\"error\":\"invalid display config\"}");
        } else {
            draw_ui(fb, state);
            refresh_display(fb);
            send_state(client, state);
        }
        return;
    }

    if (strcmp(path, "/api/alerts/ack") == 0) {
        acknowledge_alerts();
        send_alerts(client);
        return;
    }

    int changed = 0;
    int curtain_command = 0;
    int timer_changed = 0;
    int light_id = 0;
    int timer_minutes = 0;
    if (strcmp(path, "/api/light/on") == 0) { state->light_mask = ALL_LIGHTS_MASK; changed = 1; }
    else if (strcmp(path, "/api/light/off") == 0) { state->light_mask = 0; changed = 1; }
    else if (sscanf(path, "/api/light/%d/toggle", &light_id) == 1 && light_id >= 1 && light_id <= LIGHT_COUNT) {
        state->light_mask ^= (uint8_t)(1u << (light_id - 1));
        changed = 1;
    }
    else if (strcmp(path, "/api/curtain/open") == 0) { state->curtain = CURTAIN_OPEN; changed = 1; curtain_command = 1; }
    else if (strcmp(path, "/api/curtain/close") == 0) { state->curtain = CURTAIN_CLOSED; changed = 1; curtain_command = 1; }
    else if (strcmp(path, "/api/curtain/stop") == 0) { state->curtain = CURTAIN_STOPPED; changed = 1; curtain_command = 1; }
    else if (strcmp(path, "/api/scene/home") == 0) { apply_scene(state, SCENE_HOME); changed = 1; curtain_command = 1; }
    else if (strcmp(path, "/api/scene/away") == 0) { apply_scene(state, SCENE_AWAY); changed = 1; curtain_command = 1; }
    else if (strcmp(path, "/api/scene/sleep") == 0) { apply_scene(state, SCENE_SLEEP); changed = 1; curtain_command = 1; }
    else if (sscanf(path, "/api/timer/sleep/%d", &timer_minutes) == 1 &&
             timer_minutes >= 1 && timer_minutes <= 240) {
        sleep_timer_deadline_ms = monotonic_ms() + (int64_t)timer_minutes * 60 * 1000;
        timer_changed = 1;
    }
    else if (strcmp(path, "/api/timer/cancel") == 0) {
        sleep_timer_deadline_ms = 0;
        timer_changed = 1;
    }

    if (changed) {
        if (curtain_command) arm_curtain_timeout(state);
        if (strcmp(path, "/api/light/on") == 0) {
            snprintf(ui_feedback, sizeof(ui_feedback), "网页已开启全部灯光");
        } else if (strcmp(path, "/api/light/off") == 0) {
            snprintf(ui_feedback, sizeof(ui_feedback), "网页已关闭全部灯光");
        } else if (light_id >= 1 && light_id <= LIGHT_COUNT) {
            snprintf(ui_feedback, sizeof(ui_feedback), "网页已更新第%d路灯", light_id);
        } else if (strcmp(path, "/api/curtain/open") == 0) {
            snprintf(ui_feedback, sizeof(ui_feedback), "网页已打开窗帘");
        } else if (strcmp(path, "/api/curtain/close") == 0) {
            snprintf(ui_feedback, sizeof(ui_feedback), "网页已关闭窗帘");
        } else if (strcmp(path, "/api/curtain/stop") == 0) {
            snprintf(ui_feedback, sizeof(ui_feedback), "网页已停止窗帘");
        } else if (strncmp(path, "/api/scene/", 11) == 0) {
            snprintf(ui_feedback, sizeof(ui_feedback), "网页已切换场景：%s", scene_name(state->scene));
        }
        ui_feedback_until_ms = monotonic_ms() + 3000;
        commit_state(fb, state);
        send_state(client, state);
    } else if (timer_changed) {
        snprintf(ui_feedback, sizeof(ui_feedback), "%s睡眠定时",
                 sleep_timer_deadline_ms > 0 ? "已设置" : "已取消");
        ui_feedback_until_ms = monotonic_ms() + 3000;
        draw_ui(fb, state);
        refresh_display(fb);
        send_state(client, state);
    } else if (strcmp(path, "/api/state") == 0) {
        update_ip(state);
        send_state(client, state);
    } else if (strcmp(path, "/api/touch") == 0) {
        send_touch_debug(client);
    } else if (strcmp(path, "/api/diagnostics") == 0) {
        update_ip(state);
        collect_system_metrics(state);
        send_diagnostics(client, state);
    } else if (strcmp(path, "/api/history") == 0) {
        send_history(client);
    } else if (strcmp(path, "/api/alerts") == 0) {
        send_alerts(client);
    } else if (strcmp(path, "/api/health") == 0) {
        char body[96];
        snprintf(body, sizeof(body), "{\"ok\":true,\"version\":\"%s\"}", APP_VERSION);
        send_response(client, "200 OK", "application/json; charset=utf-8", body);
    } else if (strcmp(path, "/app.apk") == 0) {
        send_file_response(client, APK_PATH, "application/vnd.android.package-archive",
                           "T113-S3-Control.apk");
    } else if (strcmp(path, "/") == 0) {
        send_file_response(client, "/mnt/UDISK/panel-v3/index.html", "text/html; charset=utf-8", NULL);
    } else {
        send_response(client, "404 Not Found", "text/plain; charset=utf-8", "Not found");
    }
}

static int open_discovery_socket(void) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(DISCOVERY_PORT);
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void handle_discovery(int fd, home_state_t *state) {
    char request[128];
    struct sockaddr_in peer;
    socklen_t peer_length = sizeof(peer);
    ssize_t count = recvfrom(fd, request, sizeof(request) - 1, 0,
                             (struct sockaddr *)&peer, &peer_length);
    if (count <= 0) return;
    request[count] = '\0';
    if (strncmp(request, "T113_DISCOVER", 13) != 0) return;
    update_ip(state);
    char response[192];
    int length = snprintf(response, sizeof(response),
                          "{\"device\":\"T113-S3\",\"version\":\"%s\","
                          "\"ip\":\"%s\",\"port\":8080}",
                          APP_VERSION, state->ip);
    if (length > 0 && length < (int)sizeof(response)) {
        sendto(fd, response, (size_t)length, 0,
               (struct sockaddr *)&peer, peer_length);
    }
}


static int serve(framebuffer_t *fb, home_state_t *state) {
    int server = socket(AF_INET, SOCK_STREAM, 0);
    if (server < 0) return -1;
    int yes = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(service_port);
    if (bind(server, (struct sockaddr *)&address, sizeof(address)) < 0 || listen(server, 8) < 0) {
        close(server);
        return -1;
    }
    int discovery = service_port == 8080 ? open_discovery_socket() : -1;
    if (discovery < 0) {
        fprintf(stderr, "discovery socket: %s\n", strerror(errno));
    }
    printf("remote-only service on port %d; all local input disabled\n", service_port);
    fflush(stdout);
    for (;;) {
        fd_set reads;
        FD_ZERO(&reads);
        FD_SET(server, &reads);
        int highest = server;
        if (discovery >= 0) {
            FD_SET(discovery, &reads);
            if (discovery > highest) highest = discovery;
        }

        struct timeval timeout;
        struct timeval *timeout_ptr = &timeout;
        timeout.tv_sec = 0; timeout.tv_usec = 30000;
        int ready = select(highest + 1, &reads, NULL, NULL, timeout_ptr);
        if (ready < 0) {
            if (errno == EINTR) continue;
            break;
        }

        refresh_network_status(fb, state);
        refresh_device_status(fb, state);
        refresh_system_status(fb, state);
        export_snapshot(state);
        /* M6-B1：AI 队列桥节流驱动（提交/回传均非阻塞，见 ai_tick.c） */
        if (g_v2_ready && monotonic_ms() - ai_tick_last_ms >= 2000) {
            ai_tick_last_ms = monotonic_ms();
            ai_tick(&g_v2, V2_DATA_DIR "/ai/queue");
        }
        if (curtain_deadline_ms > 0 && monotonic_ms() >= curtain_deadline_ms) {
            curtain_deadline_ms = 0;
            state->curtain = CURTAIN_STOPPED;
            commit_state(fb, state);
        }
        if (sleep_timer_deadline_ms > 0 && monotonic_ms() >= sleep_timer_deadline_ms) {
            sleep_timer_deadline_ms = 0;
            apply_scene(state, SCENE_SLEEP);
            arm_curtain_timeout(state);
            commit_state(fb, state);
        }
        if (discovery >= 0 && FD_ISSET(discovery, &reads)) {
            handle_discovery(discovery, state);
        }
        if (FD_ISSET(server, &reads)) {
            int client = accept(server, NULL, NULL);
            if (client >= 0) {
                struct timeval limit = {1, 0};
                setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &limit, sizeof(limit));
                setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &limit, sizeof(limit));
                handle_client(client, fb, state);
                export_snapshot(state);
                close(client);
            }
        }
    }

    if (discovery >= 0) close(discovery);
    close(server);
    return -1;
}

int main(int argc, char **argv) {
    if (argc > 1) service_port = atoi(argv[1]);
    if (service_port < 1024 || service_port > 65535) return 2;
    setenv("TZ", "CST-8", 1);
    tzset();
    framebuffer_t fb;
    memset(&fb, 0, sizeof(fb));
    fb.fd = -1;
    home_state_t state;
    memset(&state, 0, sizeof(state));
    state.auth_enabled = 0;   /* 取消配对密码：出厂默认免鉴权；显式 auth_enabled=1 才恢复 */
    set_default_device_names(&state);
    set_default_info(&state);
    set_default_sensors(&state);
    state.light_mask = 0x03;
    state.curtain = CURTAIN_CLOSED;
    state.scene = SCENE_HOME;
    snprintf(state.control_pin, sizeof(state.control_pin), "731905");
    load_state(&state);
    load_device_names(&state);
    load_info_config(&state);
    load_sensor_config(&state);
    load_display_config();
    init_output_backend(&state);

    update_ip(&state);
    collect_system_metrics(&state);
    load_remote_config();
    demo_load();
    wallpaper_boot_init();   /* G3: 主题目录/staging 清理/current.json 装载（revision 下限已在 load_remote_config 回填） */
    next_page_switch_ms = monotonic_ms() + rotation_interval * 1000;
    export_snapshot(&state);
    signal(SIGPIPE, SIG_IGN);
    v2_init(state.control_pin, state.auth_enabled);   /* M3-D：v2 存储初始化（失败只降级，不影响下面一切） */
    draw_ui(&fb, &state);
    refresh_display(&fb);
    if (serve(&fb, &state) < 0) {
        fprintf(stderr, "web server: %s\n", strerror(errno));
    }
    if (g_v2_ready) pv2_ctx_close(&g_v2);
    close_output_backend();

    return 0;
}
