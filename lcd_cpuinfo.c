#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <time.h>

#include <ifaddrs.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <net/if.h>
#include <arpa/inet.h>

#include <wiringPi.h>
#include <wiringPiI2C.h>
#include <wiringSerial.h>
#include <lcd.h>
#include <mosquitto.h>
#include <signal.h>
#include <pthread.h>
#include <sys/sysinfo.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>

 
#include <math.h>
#include "dpid.h"
#include "dmem.h"
#include "dlog.h"
#include "dfork.h"
#include "dsignal.h"
#include "version.h"
#include "nxjson.h"

//------------------------------------------------------------------------------------------------------------
//
// Global handle Define
//
//------------------------------------------------------------------------------------------------------------
#define HOSTNAME_SIZE 256
#define CDIR "./"
char * progname = NULL;
char * hostname = NULL;
char * pathname = NULL;
const char * const application = "lcd_cpuinfo";
static int do_exit = 0;
//------------------------------------------------------------------------------------------------------------
//
// LCD:
//
//------------------------------------------------------------------------------------------------------------
#define LCD_ROW             2   // 16 Char
#define LCD_COL             16  // 2 Line
#define LCD_BUS             4   // Interface 4 Bit mode
#define LCD_UPDATE_PERIOD   1000 // 1 sec
#define LCD_AUTO_SW_TIMEOUT 5000 // 5 sec
#define STATE_PUBLISH_INTERVAL 30000 // 30 sec

static unsigned char lcdFb[LCD_ROW][LCD_COL] = {0, };

static int lcdHandle  = 0;

//
// DispMode
// 0 = date & time, 1 = ethrenet ip addr, 2 = cpu temperature
// 3 = freq governor. 4 = Weather, 5 = Little core frequency
//
#define MAX_DISP_MODE 4
static int DispMode = 1;

#define PORT_LCD_RS     7   // GPX1.2(#18)
#define PORT_LCD_E      0   // GPA0.3(#174)
#define PORT_LCD_D4     2   // GPX1.5(#21)
#define PORT_LCD_D5     3   // GPX1.6(#22)
#define PORT_LCD_D6     1   // GPA0.2(#173)
#define PORT_LCD_D7     4   // GPX1.3(#19)

//------------------------------------------------------------------------------------------------------------
//
// Button:
//
//------------------------------------------------------------------------------------------------------------
#define PORT_BUTTON1    5   // GPX1.7(#23)
#define PORT_BUTTON2    6   // GPX2.0(#24)

//------------------------------------------------------------------------------------------------------------
//
// Mosquitto:
//
//------------------------------------------------------------------------------------------------------------
typedef struct _client_info_t {
    struct mosquitto *m;
    pid_t pid;
    uint32_t tick_ct;
} t_client_info;

char * mqtt_host = "localhost";
char * mqtt_username = "owntracks";
char * mqtt_password = "zhopa";
int mqtt_port = 8883;
int mqtt_keepalive = 60;

static struct mosquitto *mosq = NULL;
static t_client_info client_info;
static pthread_t mosq_th = 0;

//------------------------------------------------------------------------------------------------------------
//
// LED:
//
//------------------------------------------------------------------------------------------------------------

const int ledPorts[] = {
    21, // GPX2.4(#28)
    22, // GPX2.6(#30)
    23, // GPX2.7(#31)
    11, // GPX2.1(#25)
    26, // GPX2.5(#29)
    27, // GPX3.1(#33)
};

#define MAX_LED_CNT sizeof(ledPorts) / sizeof(ledPorts[0])

void publish_double(char *, double);

//------------------------------------------------------------------------------------------------------------
//
// Daemon commands callbacks:
//
//------------------------------------------------------------------------------------------------------------
pid_t main_pid;

enum command_int_t {
    CMD_NONE = 0,
    CMD_RECONFIGURE,
    CMD_SHUTDOWN,
    CMD_RESTART,
    CMD_CHECK,
    CMD_NOT_FOUND = -1,
};

typedef int (* daemon_command_callback_t)(void*);

typedef struct daemon_command_t {
    char * command_name;
    daemon_command_callback_t  command_callback;
    int command_int;
} DAEMON_COMMAND_T;

int check_callback(void * UNUSED(param)) {
    return(10);
}


int reconfigure_callback(void * UNUSED(param)) {

    if (daemon_pid_file_kill(SIGUSR1) < 0) {
        daemon_log(LOG_WARNING, "Failed to reconfiguring");
    } else {
        daemon_log(LOG_INFO, "OK");
    }
    return(10);
}

int shutdown_callback(void * UNUSED(param)) {
    int ret;
    daemon_log(LOG_INFO, "Try to shutdown self....");
    if ((ret = daemon_pid_file_kill_wait(SIGINT, 10)) < 0) {
        daemon_log(LOG_WARNING, "Failed to shutdown daemon %d %s", errno, strerror(errno));
        daemon_log(LOG_WARNING, "Try to terminating self....");
        if (daemon_pid_file_kill_wait(SIGKILL, 0) < 0) {
            daemon_log(LOG_WARNING, "Failed to killing daemon %d %s", errno, strerror(errno));
        } else {
            daemon_log(LOG_WARNING, "Daemon terminated");
        }
    } else
        daemon_log(LOG_INFO, "OK");
    return(10);
}

int restart_callback(void * UNUSED(param)) {
    shutdown_callback(NULL);
    return(0);
}

DAEMON_COMMAND_T daemon_commands[] = {
    {command_name: "reconfigure", command_callback: reconfigure_callback, command_int: CMD_RECONFIGURE},
    {command_name: "shutdown", command_callback: shutdown_callback, command_int: CMD_SHUTDOWN},
    {command_name: "restart", command_callback: restart_callback, command_int: CMD_RESTART},
    {command_name: "check", command_callback: check_callback, command_int: CMD_CHECK},
};

//------------------------------------------------------------------------------------------------------------
//
// DispMode
// 0 = date & time, 1 = ethrenet ip addr, 2 = cpu temperature
// 3 = freq governor. 4 = Big core frequency, 5 = Little core frequency
//
//------------------------------------------------------------------------------------------------------------
//
// Get little core freq(CPU4)
//
//------------------------------------------------------------------------------------------------------------
#define FD_LITTLECORE_FREQ "/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq"

static void get_littlecore_freq(void) {
    int     n, fd, freq;
    char    buf[LCD_COL];

    memset(buf, ' ', sizeof(buf));

    if((fd = open(FD_LITTLECORE_FREQ, O_RDONLY)) < 0)   {
        daemon_log(LOG_ERR, "%s : file open error!", __func__);
    } else    {
        read(fd, buf, sizeof(buf));
        close(fd);
        freq = atoi(buf);
        n = sprintf(buf, "Little-Core Freq");
        strncpy((char*)&lcdFb[0][0], buf, n);
        n = sprintf(buf, "%d Mhz", freq / 1000);
        strncpy((char*)&lcdFb[1][4], buf, n);
    }
}

//------------------------------------------------------------------------------------------------------------
//
// Get big core freq(CPU4)
//
//------------------------------------------------------------------------------------------------------------
#define FD_BIGCORE_FREQ "/sys/devices/system/cpu/cpu4/cpufreq/scaling_cur_freq"

__attribute__((unused)) static void get_bigcore_freq(void) {
    int     n, fd, freq;
    char    buf[LCD_COL];

    memset(buf, ' ', sizeof(buf));

    if((fd = open(FD_BIGCORE_FREQ, O_RDONLY)) < 0)   {
        daemon_log(LOG_ERR, "%s : file open error!", __func__);
    } else    {
        n = read(fd, buf, sizeof(buf));
        close(fd);
        freq = atoi(buf);
        n = sprintf(buf, "Big-Core Freq");
        strncpy((char*)&lcdFb[0][0], buf, n);
        n = sprintf(buf, "%d Mhz", freq / 1000);
        strncpy((char*)&lcdFb[1][4], buf, n);
    }
}

//------------------------------------------------------------------------------------------------------------
//
// Get system governor(CPU0)
//
//------------------------------------------------------------------------------------------------------------
#define FD_SYSTEM_GOVERNOR  "/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor"

__attribute__((unused))static void get_system_governor(void) {
    int     n, fd;
    char    buf[LCD_COL];

    memset(buf, ' ', sizeof(buf));

    if((fd = open(FD_SYSTEM_GOVERNOR, O_RDONLY)) < 0)   {
        daemon_log(LOG_ERR, "%s : file open error!", __func__);
    } else    {
        n = read(fd, buf, sizeof(buf));
        close(fd);
        strncpy((char*)&lcdFb[1][2], buf, n - 1);
        n = sprintf(buf, "SYSTEM Governor");
        strncpy((char*)&lcdFb[0][0], buf, n);
    }
}

//------------------------------------------------------------------------------------------------------------
//
// Get ethernet ip addr
//
//------------------------------------------------------------------------------------------------------------
static void get_ethernet_ip(void) {
    struct  ifaddrs *ifa;
    int     n;
    char    buf[LCD_COL];

    memset(buf, ' ', sizeof(buf));

    getifaddrs(&ifa);

    while(ifa)  {
        if(ifa->ifa_addr && ifa->ifa_addr->sa_family == AF_INET)    {
            struct sockaddr_in *pAddr = (struct sockaddr_in *)ifa->ifa_addr;

            if(0 == strncmp(ifa->ifa_name, "eth", 2)) {
                n = sprintf(buf, "My IP Addr(%s)", ifa->ifa_name);
                strncpy((char*)&lcdFb[0][0], buf, n);

                n = sprintf(buf, "%s", inet_ntoa(pAddr->sin_addr));
                strncpy((char*)&lcdFb[1][1], buf, n);
            }
        }
        ifa = ifa->ifa_next;
    }
    freeifaddrs(ifa);
}

//------------------------------------------------------------------------------------------------------------
//
// Get date & time
//
//------------------------------------------------------------------------------------------------------------
static void get_date_time(void) {
    time_t      tm_time;
    struct tm   *st_time;
    char        buf[LCD_COL];
    int         n;

    memset(buf, ' ', sizeof(buf));

    time(&tm_time);
    st_time = localtime( &tm_time);

    n = strftime(buf, LCD_COL, "%Y/%m/%d %a", st_time);
    strncpy((char*)&lcdFb[0][0], buf, n);
    n = strftime(buf, LCD_COL, "%H:%M:%S %p", st_time);
    strncpy((char*)&lcdFb[1][2], buf, n);
}

//------------------------------------------------------------------------------------------------------------
//
// Get CPU Temperature
//
//------------------------------------------------------------------------------------------------------------
#define FD_SYSTEM_TEMP  "/sys/class/thermal/thermal_zone0/temp"

static void get_cpu_temperature(void) {
    int     fd, temp_C, temp_F, n;
    char    buf[LCD_COL];

    memset(buf, ' ', sizeof(buf));

    if((fd = open(FD_SYSTEM_TEMP, O_RDONLY)) < 0)    {
        daemon_log(LOG_ERR, "%s : file open error!", __func__);
    } else    {
        read(fd, buf, LCD_COL);
        close(fd);

        temp_C = atoi(buf) / 1000;
        temp_F = (temp_C * 18 + 320) / 10;

        n = sprintf(buf, "CPU Temperature");
        strncpy((char*)&lcdFb[0][0], buf, n);
        n = sprintf(buf, "%3d *C, %3d.%1d *F", temp_C, temp_F, temp_F % 10);
        strncpy((char*)&lcdFb[1][0], buf, n);
    }
}

//------------------------------------------------------------------------------------------------------------
//
// Get weather info
//
//------------------------------------------------------------------------------------------------------------
#define BUF_SIZE 1024

static char * get_last_line_from_file(char * filename) {
    char buffer[BUF_SIZE];
    char *ret = NULL;

    if ((!filename) || (!*filename)) return(ret);

    FILE *fp = fopen(filename, "r");

    if (fp == NULL) {
        daemon_log(LOG_ERR, "%s : file open error!", __func__);
        return(ret);
    }

    fseek(fp, -(BUF_SIZE + 1), SEEK_END);
    int len = fread(buffer, 1, BUF_SIZE, fp);
    if (len > 0) {
        int pos[2] = { -1, -1};
        int i = len - 1;
        int p = 0;
        while ((i >= 0) && (p < 2)) {
            if (buffer[i] == '\n') {
                pos[p] = i;
                p++;
            }
            i--;
        }
        if ((pos[0] != -1) && (pos[1] != -1)) {
            int l = pos[0] - pos[1];
            ret = calloc(1, l);
            memmove(ret, &buffer[pos[1] + 1], l - 1);
        }
    } else {
        daemon_log(LOG_ERR, "%s : file read error!", __func__);
    }
    fclose(fp);
    return(ret);
}


#define FD_EXT_SENSOR_FD  "/tmp/weather/rtl_433.log"
#define FD_INT_SENSOR_FD  "/tmp/weather/weather_board.log"

typedef struct _weather_t {
    char   location[20];
    double temperature_C;
    double pressure;
    double humidity;
    double uv_index;
    double visible_light;
    double ir_light;
    bool rain;
    double rain_mm;
    double wind;
    double wind_dir;
} weather_t;

void weather_init(weather_t * weather) {
    if (!weather) return;
    bzero(weather, sizeof(weather));
    strcpy(weather->location, "UNK");
    weather->temperature_C = NAN;
    weather->pressure = NAN;
    weather->humidity = NAN;
    weather->uv_index = NAN;
    weather->visible_light = NAN;
    weather->ir_light = NAN;
    weather->rain_mm = NAN;
    weather->wind = NAN;
    weather->wind_dir = NAN;
    weather->rain = 3;
}

static void get_weather_from_json(char * location, weather_t * weather, char * txt_json) {
    if ((!txt_json) || (!*txt_json) || (!weather)) return;
    if ((location) && (*location)) {
        strcpy(weather->location, location);
    }
    const nx_json* json = nx_json_parse_utf8(txt_json);

    if (json) {
        const nx_json * json1;
        json1  = nx_json_get(json, "temperature_C");
        if (json1) {
            weather->temperature_C = json1->dbl_value;
        }

        json1  = nx_json_get(json, "humidity");
        if (json1) {
            weather->humidity = json1->dbl_value;
        }

        json1  = nx_json_get(json, "pressure");
        if (json1) {
            weather->pressure = json1->dbl_value;
        }

        json1  = nx_json_get(json, "uv_index");
        if (json1) {
            weather->uv_index = json1->dbl_value;
        }

        json1  = nx_json_get(json, "visible");
        if (json1) {
            weather->visible_light = json1->dbl_value;
        }
        json1  = nx_json_get(json, "ir");
        if (json1) {
            weather->ir_light = json1->dbl_value;
        }
        nx_json_free(json);
    }
}

static bool is_need_publish(double curr, double old) {
    if (isnan(curr)) return(false);
    if (!isnan(curr) && isnan(old)) return(true);
    return roundf(curr * 10.) / 10. != roundf(old * 10.) / 10.;
}

//"home/%s/weather/%s"

static void publist_weather_topic(char * template, char * location, char * name, double value) {
    char topic[256];
    sprintf(topic, template, location, name);
    publish_double(topic, value);
}

#define WEATHER_INT 0
#define WEATHER_EXT 1
#define WEATHER_COUNT 2
weather_t _weather_int = {};
weather_t _weather_ext = {};
weather_t * weather[WEATHER_COUNT] = {&_weather_int, &_weather_ext};

weather_t _old_weather_int = {};
weather_t _old_weather_ext = {};
weather_t * old_weather[WEATHER_COUNT] = {&_old_weather_int, &_old_weather_ext};

static void publish_weather(weather_t * old_weather, weather_t * cur_weather, char * topic_template) {
    if ((!old_weather) || (!cur_weather)) return;

    if (!strlen(old_weather->location)) {
        weather_init(old_weather);
    }

    if (is_need_publish(cur_weather->temperature_C, old_weather->temperature_C)) {
        publist_weather_topic(topic_template, cur_weather->location, "temperature_C", cur_weather->temperature_C);
    }

    if (is_need_publish(cur_weather->humidity, old_weather->humidity)) {
        publist_weather_topic(topic_template, cur_weather->location, "humidity", cur_weather->humidity);
    }

    if (is_need_publish(cur_weather->pressure, old_weather->pressure)) {
        publist_weather_topic(topic_template, cur_weather->location, "pressure", cur_weather->pressure);
    }

    if (is_need_publish(cur_weather->visible_light, old_weather->visible_light)) {
        publist_weather_topic(topic_template, cur_weather->location, "visible_light", cur_weather->visible_light);
    }

    if (is_need_publish(cur_weather->ir_light, old_weather->ir_light)) {
        publist_weather_topic(topic_template, cur_weather->location, "ir_light", cur_weather->ir_light);
    }

    if (is_need_publish(cur_weather->uv_index, old_weather->uv_index)) {
        publist_weather_topic(topic_template, cur_weather->location, "uv_index", cur_weather->uv_index);
    }

    *old_weather = *cur_weather;
}

static void publish_weathers(weather_t * old_weather[], weather_t * cur_weather[], char * topic_template) {
/*
    for (int i = 0; i < WEATHER_COUNT; i++) {
        publish_weather(old_weather[i], cur_weather[i], topic_template);
    }
*/
}

static void publish_sensors(weather_t * cur_weather[],char * topic_template)
{
    char topic[128] = {};
    snprintf(topic,sizeof(topic)-1,topic_template,hostname);
}

static void publish_state(char * topic_template)
{
    // "Time":"2017-03-04T13:37:24" "Uptime":  "Cputemp":
    static int timer_publish_state = 0;

    if (timer_publish_state >  millis()) return;
    else {
	timer_publish_state = millis () + STATE_PUBLISH_INTERVAL;
    }

    time_t timer;
    char tm_buffer[26]={};
    char buf[255] = {};
    char topic[128] = {};
    struct tm* tm_info;
    struct sysinfo info;
    int res;

    time(&timer);
    tm_info = localtime(&timer);
    strftime(tm_buffer, 26, "%Y-%m-%dT%H:%M:%S", tm_info);

    if (!sysinfo(&info)) {
	int fd;
	char tmp_buf[20];
	memset(tmp_buf, ' ', sizeof(tmp_buf));
	if((fd = open(FD_SYSTEM_TEMP, O_RDONLY)) < 0)    {
    	    daemon_log(LOG_ERR, "%s : file open error!", __func__);
	} else    {
    	    read(fd, buf, sizeof(tmp_buf));
    	    close(fd);
	}
        int temp_C = atoi(buf) / 1000;
 

	snprintf(buf,sizeof(buf)-1,"{\"Time\":\"%s\", \"Uptime\": %ld, \"LoadAverage\":%.2f, \"CPUTemp\":%d}",
		tm_buffer, info.uptime/3600, info.loads[0]/65536.0, temp_C);
	snprintf(topic, sizeof(topic)-1, topic_template, hostname);
	daemon_log(LOG_INFO,"%s %s", topic, buf);
	if ((res = mosquitto_publish (mosq, NULL, topic, strlen (buf), buf, 0, false)) != 0) {
    	    daemon_log(LOG_ERR, "Can't publish to Mosquitto server %s", mosquitto_strerror(res));
	}
    }
}



static void get_weather_info(weather_t * weather[]) {

    char * txt_json;
    weather_init(weather[WEATHER_EXT]);
    txt_json = get_last_line_from_file(FD_EXT_SENSOR_FD);
    if (txt_json) {
	get_weather_from_json("EX", weather[WEATHER_EXT], txt_json);
	free(txt_json);
    }

    weather_init(weather[WEATHER_INT]);
    txt_json = get_last_line_from_file(FD_INT_SENSOR_FD);
    if (txt_json) {
	get_weather_from_json("IN", weather[WEATHER_INT], txt_json);
	free(txt_json);
    }

}

static void display_weather_info(weather_t * weather[]) {
    char    buf[LCD_COL+1];
    int     n;
    memset(buf, ' ', sizeof(buf));
    n = snprintf(buf,sizeof(buf)-1,"%2s%5.1f %2.0f%%",
                weather[WEATHER_INT]->location, weather[WEATHER_INT]->temperature_C, roundf(weather[WEATHER_INT]->humidity));
    strncpy((char*)&lcdFb[0][0], buf, n);
    n = snprintf(buf,sizeof(buf)-1, "%2s%5.1f %2.0f%% %3.0f",
                weather[WEATHER_EXT]->location, weather[WEATHER_EXT]->temperature_C, roundf(weather[WEATHER_EXT]->humidity), roundf(0.750062 * weather[WEATHER_INT]->pressure));
    strncpy((char*)&lcdFb[1][0], buf, n);
}

//------------------------------------------------------------------------------------------------------------
//
// LCD Update Function:
//
//------------------------------------------------------------------------------------------------------------
static void lcd_update (void) {
    int i, j;

    // lcd fb clear
    memset((void *)&lcdFb, ' ', sizeof(lcdFb));

    // lcd fb update
    switch(DispMode)    {
    default  :
        DispMode = 0;
    case    0:
        get_date_time();
        break;
    case    1:
        display_weather_info(weather);
        break;
    case    2:
        get_ethernet_ip();
        break;
    case    3:
        get_cpu_temperature();
        break;
    case    4:
        get_littlecore_freq();
        break;
    }

    for(i = 0; i < LCD_ROW; i++)    {
        lcdPosition (lcdHandle, 0, i);
        for(j = 0; j < LCD_COL; j++)    lcdPutchar(lcdHandle, lcdFb[i][j]);
    }
}

//------------------------------------------------------------------------------------------------------------
//
// system init
//
//------------------------------------------------------------------------------------------------------------
int system_init(void) {
    int i;

    // LCD Init
    lcdHandle = lcdInit (LCD_ROW, LCD_COL, LCD_BUS,
                         PORT_LCD_RS, PORT_LCD_E,
                         PORT_LCD_D4, PORT_LCD_D5, PORT_LCD_D6, PORT_LCD_D7, 0, 0, 0, 0);

    if(lcdHandle < 0)   {
        daemon_log(LOG_ERR, "%s : lcdInit failed!", __func__);
        return -1;
    }

    // GPIO Init(LED Port ALL Output)
    for(i = 0; i < MAX_LED_CNT; i++)    {
        pinMode (ledPorts[i], OUTPUT);
        pullUpDnControl (PORT_BUTTON1, PUD_OFF);
    }

    // Button Pull Up Enable.
    pinMode (PORT_BUTTON1, INPUT);
    pullUpDnControl (PORT_BUTTON1, PUD_UP);
    pinMode (PORT_BUTTON2, INPUT);
    pullUpDnControl (PORT_BUTTON2, PUD_UP);

    return  0;
}

//------------------------------------------------------------------------------------------------------------
//
// board data update
//
//------------------------------------------------------------------------------------------------------------
bool boardDataUpdate(void) {
    bool flag_mod = false;
    // button status read
    if(!digitalRead (PORT_BUTTON1)) {
        if(DispMode)        DispMode--;
        else                DispMode = MAX_DISP_MODE;
        flag_mod = true;
    }
    if(!digitalRead (PORT_BUTTON2)) {
        if(DispMode < MAX_DISP_MODE)    DispMode++;
        else                DispMode = 0;
        flag_mod = true;
    }

    //  LED Control
    for(int i = 0; i < MAX_LED_CNT; i++)    digitalWrite (ledPorts[i], 0); // LED All Clear
    digitalWrite(ledPorts[DispMode], 1);
    return(flag_mod);
}


void on_log(struct mosquitto *mosq, void *userdata, int level, const char *str) {
    switch(level) {
//    case MOSQ_LOG_DEBUG:
//    case MOSQ_LOG_INFO:
//    case MOSQ_LOG_NOTICE:
    case MOSQ_LOG_WARNING:
    case MOSQ_LOG_ERR: {
        daemon_log(LOG_ERR, "%i:%s", level, str);
    }
    }
}

static
void on_connect(struct mosquitto *m, void *udata, int res) {
    if (res == 0) {             /* success */
        //t_client_info *info = (t_client_info *)udata;
        mosquitto_subscribe(m, NULL, "home/+/weather/#", 0);
        mosquitto_subscribe(m, NULL, "stat/+/POWER", 0);
    } else {
        daemon_log(LOG_ERR, "connection refused error");
    }
}

static
void on_publish(struct mosquitto *m, void *udata, int m_id) {
    //daemon_log(LOG_ERR, "-- published successfully");
}

static
void on_subscribe(struct mosquitto *m, void *udata, int mid,
                  int qos_count, const int *granted_qos) {
    daemon_log(LOG_INFO, "-- subscribed successfully");
}

static
void on_message(struct mosquitto *m, void *udata,
                const struct mosquitto_message *msg) {
    if (msg == NULL) {
        return;
    }
    daemon_log(LOG_INFO, "-- got message @ %s: (%d, QoS %d, %s) '%s'",
            msg->topic, msg->payloadlen, msg->qos, msg->retain ? "R" : "!r",
            (char*)msg->payload);
    //t_client_info *info = (t_client_info *)udata;
}

static
void * mosq_thread_loop(void * p) {
    t_client_info *info = (t_client_info *)p;
    daemon_log(LOG_INFO, "%s", __FUNCTION__);
    while (!do_exit) {
        int res = mosquitto_loop(info->m, 1000, 1);
        switch (res) {
        case MOSQ_ERR_SUCCESS:
            break;
        case MOSQ_ERR_NO_CONN: {
            int res = mosquitto_connect (mosq, mqtt_host, mqtt_port, mqtt_keepalive);
            if (res) {
                daemon_log(LOG_ERR, "Can't connect to Mosquitto server %s", mosquitto_strerror(res));
            }
            break;
        }
        case MOSQ_ERR_INVAL:
        case MOSQ_ERR_NOMEM:
        case MOSQ_ERR_CONN_LOST:
        case MOSQ_ERR_PROTOCOL:
        case MOSQ_ERR_ERRNO:
            daemon_log(LOG_ERR, "%s %s %s", __FUNCTION__, strerror(errno), mosquitto_strerror(res));
            break;
        }
    }
    daemon_log(LOG_INFO, "%s finished", __FUNCTION__);
    pthread_exit(NULL);
}

static
void mosq_init() {

    bool clean_session = true;

    mosquitto_lib_init();

    mosq = mosquitto_new(progname, clean_session, &client_info);
    if(!mosq) {
        daemon_log(LOG_ERR, "mosq Error: Out of memory.");
    } else {
        client_info.m = mosq;
        mosquitto_log_callback_set(mosq, on_log);

        mosquitto_connect_callback_set(mosq, on_connect);
        mosquitto_publish_callback_set(mosq, on_publish);
        mosquitto_subscribe_callback_set(mosq, on_subscribe);
        mosquitto_message_callback_set(mosq, on_message);

        mosquitto_username_pw_set (mosq, mqtt_username, mqtt_password);

        daemon_log(LOG_INFO, "Try connect to Mosquitto server ");
        int res = mosquitto_connect (mosq, mqtt_host, mqtt_port, mqtt_keepalive);
        if (res) {
            daemon_log(LOG_ERR, "Can't connect to Mosquitto server %s", mosquitto_strerror(res));
        }
	pthread_create(&mosq_th, NULL, mosq_thread_loop, &client_info);
    }
    
}

static
void mosq_destroy() {
    pthread_join(mosq_th, NULL);
    if (mosq) {
        mosquitto_disconnect(mosq);
        mosquitto_destroy(mosq);
    }
    mosquitto_lib_cleanup();
}


void publish_double(char * topic, double value) {
    int res;
    if (!mosq) return;
    char text[20];
    sprintf(text, "%.2f", value);
    if ((res = mosquitto_publish (mosq, NULL, topic, strlen (text), text, 0, true)) != 0) {
        if (res) {
            daemon_log(LOG_ERR, "Can't publish to Mosquitto server %s", mosquitto_strerror(res));
        }
    }
}

//------------------------------------------------------------------------------------------------------------
//
// main loop:
//
//------------------------------------------------------------------------------------------------------------

static
void * main_loop (void * p) {
    int timer = 0;
    int timer_auto_sw = 0;
    daemon_log(LOG_INFO, "%s", __FUNCTION__);
    while(!do_exit)    {
        if (millis () < timer)  {
            usleep(100000);    // 100ms sleep state
            if (!boardDataUpdate()) {
                continue;
            } else {
                timer_auto_sw = millis () + LCD_AUTO_SW_TIMEOUT;
            }
        }

        if (millis () >= timer_auto_sw) {
            timer_auto_sw = millis () + LCD_AUTO_SW_TIMEOUT;
            DispMode ++;
            DispMode &= 1;
        }

        timer = millis () + LCD_UPDATE_PERIOD;
        get_weather_info(weather);
        //publish_weathers(old_weather, weather, "home/%s/weather/%s");
	publish_sensors(weather,"tele/%s/SENSOR");
	publish_state("tele/%s/STATE");
        lcd_update();
    }
    daemon_log(LOG_INFO, "%s finished", __FUNCTION__);
    pthread_exit(NULL);
}

//------------------------------------------------------------------------------------------------------------
//
// Usage:
//
//------------------------------------------------------------------------------------------------------------
static void usage()
{
}

//------------------------------------------------------------------------------------------------------------
//
// Start Program:
//
//------------------------------------------------------------------------------------------------------------

int
main (int argc, char *const *argv) {
    int flags;
    int daemonize = true;
    int debug = 0;
    char * command = NULL;
    pid_t pid;
    pthread_t main_th = 0;

    int    fd, sel_res;

    daemon_pid_file_ident = daemon_log_ident = application;

    tzset();

    if ((progname = strrchr(argv[0], '/')) == NULL)
        progname = argv[0];
    else
        ++progname;

    if (strrchr(argv[0], '/') == NULL)
        pathname = xstrdup(CDIR);
    else {
        pathname = xmalloc(strlen(argv[0]) + 1);
        strncpy(pathname, argv[0], (strrchr(argv[0], '/') - argv[0]) + 1);
    }

    if (chdir(pathname) < 0) {
        daemon_log(LOG_ERR, "chdir error: %s", strerror(errno));
    }

    FREE(pathname);

    pathname = get_current_dir_name();

    hostname = calloc(1,HOSTNAME_SIZE);
    gethostname(hostname, HOSTNAME_SIZE-1);

    daemon_log_upto(LOG_INFO);
    daemon_log(LOG_INFO, "%s %s", pathname, progname);

    while ((flags = getopt(argc, argv, "i:fdk:")) != -1) {

        switch (flags) {
        case 'i': {
            daemon_pid_file_ident = daemon_log_ident = xstrdup(optarg);
            break;
        }
        case 'f' : {
            daemonize = false;
            break;
        }
        case 'd': {
            debug++;
            daemon_log_upto(LOG_DEBUG);
            break;
        }
        case 'k': {
            command = xstrdup(optarg);
            break;
        }
        default: {
            usage();
            break;
        }

        }
    }

    if (debug) {
        daemon_log(LOG_DEBUG,    "**************************");
        daemon_log(LOG_DEBUG,    "* WARNING !!! Debug mode *");
        daemon_log(LOG_DEBUG,    "**************************");
    }

    daemon_log(LOG_INFO, "%s ver %s [%s %s %s] started", application,  git_version, git_branch, __DATE__, __TIME__);
    daemon_log(LOG_INFO, "***************************************************************************");
    daemon_log(LOG_INFO, "pid file: %s", daemon_pid_file_proc());
    if (command) {
        int r = CMD_NOT_FOUND;
        for (unsigned int i = 0; i < (sizeof(daemon_commands) / sizeof(daemon_commands[0])); i++) {
            if ((strcasecmp(command, daemon_commands[i].command_name) == 0) && (daemon_commands[i].command_callback)) {
                if ((r = daemon_commands[i].command_callback(pathname)) != 0) exit(abs(r - 10));
            }
        }
        if (r == CMD_NOT_FOUND) {
            daemon_log(LOG_ERR, "command \"%s\" not found.", command);
            usage();
        }
    }
    FREE(command);

    /* initialize PRNG */
    srand ((unsigned int) time (NULL));

    if ((pid = daemon_pid_file_is_running()) >= 0) {
        daemon_log(LOG_ERR, "Daemon already running on PID file %u", pid);
        return 1;
    }

    daemon_log(LOG_INFO, "Make a daemon");

    daemon_retval_init();
    if ((daemonize) && ((pid = daemon_fork()) < 0)) {
        return 1;
    } else if ((pid) && (daemonize)) {
        int ret;
        if ((ret = daemon_retval_wait(20)) < 0) {
            daemon_log(LOG_ERR, "Could not recieve return value from daemon process.");
            return 255;
        }
        if (ret == 0) {
            daemon_log(LOG_INFO, "Daemon started.");
        } else {
            daemon_log(LOG_ERR, "Daemon dont started, returned %i as return value.", ret);
        }
        return ret;
    } else {

        if (daemon_pid_file_create() < 0) {
            daemon_log(LOG_ERR, "Could not create PID file (%s).", strerror(errno));
            daemon_retval_send(1);
            goto finish;
        }

        if (daemon_signal_init(/*SIGCHLD,*/SIGINT, SIGTERM, SIGQUIT, SIGHUP, SIGUSR1, SIGUSR2, SIGHUP, /*SIGSEGV,*/ 0) < 0) {
            daemon_log(LOG_ERR, "Could not register signal handlers (%s).", strerror(errno));
            daemon_retval_send(1);
            goto finish;
        }

        daemon_retval_send(0);
        daemon_log(LOG_INFO, "%s ver %s [%s %s %s] started", application,  git_version, git_branch, __DATE__, __TIME__);

        struct rlimit core_lim;

        if (getrlimit(RLIMIT_CORE, &core_lim) < 0) {
            daemon_log(LOG_ERR, "getrlimit RLIMIT_CORE error:%s", strerror(errno));
        } else {
            daemon_log(LOG_INFO, "core limit is cur:%2ld max:%2ld", core_lim.rlim_cur, core_lim.rlim_max );
            core_lim.rlim_cur = -1;
            core_lim.rlim_max = -1;
            if (setrlimit(RLIMIT_CORE, &core_lim) < 0) {
                daemon_log(LOG_ERR, "setrlimit RLIMIT_CORE error:%s", strerror(errno));
            } else {
                daemon_log(LOG_INFO, "core limit set cur:%2ld max:%2ld", core_lim.rlim_cur, core_lim.rlim_max );
            }
        }
        main_pid = syscall(SYS_gettid);

	wiringPiSetup ();

	if (system_init() < 0) {
	    daemon_log(LOG_ERR, "%s: System Init failed", __func__);
	    goto finish;
	}

	mosq_init();
	sleep(1);

	pthread_create( &main_th, NULL, main_loop, NULL);
// main 

        fd_set fds;
        FD_ZERO(&fds);
        fd = daemon_signal_fd();
        FD_SET(fd,  &fds);

        while (!do_exit) {
            struct timeval tv;
            tv.tv_sec  = 0;
            tv.tv_usec = 100000;
            fd_set fds2 = fds;
            if ((sel_res = select(FD_SETSIZE, &fds2, 0, 0, &tv)) < 0) {

                if (errno == EINTR)
                    continue;

                daemon_log(LOG_ERR, "select() error:%d %s", errno,  strerror(errno));
                break;
            }
            if (FD_ISSET(fd, &fds2)) {
                int sig;

                if ((sig = daemon_signal_next()) <= 0) {
                    daemon_log(LOG_ERR, "daemon_signal_next() failed.");
                    break;
                }

                switch (sig) {
                case SIGCHLD: {
                    int ret = 0;
                    daemon_log(LOG_INFO, "SIG_CHLD");
                    wait(&ret);
                    daemon_log(LOG_INFO, "RET=%d", ret);
                }
                break;

                case SIGINT:
                case SIGQUIT:
                case SIGTERM:
                    daemon_log(LOG_WARNING, "Got SIGINT, SIGQUIT or SIGTERM");
                    do_exit = true;
                    break;

                case SIGUSR1: {
                    daemon_log(LOG_WARNING, "Got SIGUSR1");
                    daemon_log(LOG_WARNING, "Enter in debug mode, to stop send me USR2 signal");
                    daemon_log_upto(LOG_DEBUG);
                    break;
                }
                case SIGUSR2: {
                    daemon_log(LOG_WARNING, "Got SIGUSR2");
                    daemon_log(LOG_WARNING, "Leave debug mode");
                    daemon_log_upto(LOG_INFO);
                    break;
                }
                case SIGHUP:
                    daemon_log(LOG_WARNING, "Got SIGHUP");
                    break;

                case SIGSEGV:
                    daemon_log(LOG_ERR, "Seg fault. Core dumped to /tmp/core.");
                    if (chdir("/tmp") < 0) {
                        daemon_log(LOG_ERR, "Chdir to /tmp error: %s", strerror(errno));
                    }
                    signal(sig, SIG_DFL);
                    kill(getpid(), sig);
                    break;

                default:
                    daemon_log(LOG_ERR, "UNKNOWN SIGNAL:%s", strsignal(sig));
                    break;

                }
            }
        }

    }

finish:
    daemon_log(LOG_INFO, "Exiting...");
    mosq_destroy();
    pthread_join(main_th, NULL);
    FREE(hostname);
    FREE(pathname);
    daemon_retval_send(-1);
    daemon_signal_done();
    daemon_pid_file_remove();
    daemon_log(LOG_INFO, "Exit");
    exit(0);
}
//------------------------------------------------------------------------------------------------------------
//------------------------------------------------------------------------------------------------------------
