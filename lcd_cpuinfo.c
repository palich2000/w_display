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
#include <limits.h>

#include <ifaddrs.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <net/if.h>
#include <arpa/inet.h>

#include <mosquitto.h>
#include <signal.h>
#include <pthread.h>
#include <sys/sysinfo.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <regex.h>
#include <sys/reboot.h>
#include <sys/mman.h>
#include <math.h>
#include <getopt.h>

#include "dpid.h"
#include "dmem.h"
#include "dlog.h"
#include "dfork.h"
#include "dsignal.h"
#include "version.h"
#include "nxjson.h"
#include "array.h"

#ifdef CHARLCD
    #include "CharLCD.h"
#endif
#ifdef INA219
    #include "ina219.h"
#endif

#include "faraday_serial.h"

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
#define d2r (M_PI / 180.0)
//------------------------------------------------------------------------------------------------------------
//
// LCD:
//
//------------------------------------------------------------------------------------------------------------
#define LCD_ROW             2       // 16 Char
#define LCD_COL             16      // 2 Line
#define LCD_BUS             1       // i2c bus
#define LCD_UPDATE_PERIOD   2000    // 1 sec
#define LCD_AUTO_SW_TIMEOUT 10000    // 10 sec
#define STATE_PUBLISH_INTERVAL 60000   // 60 sec
#define SENSORS_PUBLISH_INTERVAL 60000 // 60 sec
#define PIAWARE_PUBLISH_INTERVAL 60000 // 60 sec
static int thermal_zone = 0;
static int no_aircraft = 1;
static int no_weather = 0;
static int no_display = 0;
static char lcdFb[LCD_ROW][LCD_COL + 1] = {0};
static char lcdFbTD[LCD_ROW][LCD_COL + 1] = {0};

void fill_lcdFb(void) {
    memset(&lcdFb[0][0], ' ', LCD_COL);
    lcdFb[0][LCD_COL] = 0;
    memset(&lcdFb[1][0], ' ', LCD_COL);
    lcdFb[1][LCD_COL] = 0;
}

#ifdef CHARLCD
static CharLCD_t * lcd = NULL;
#endif

double station_lat = 50.371;
double station_lon = 30.389;

//
// DispMode
// 0 = date & time, 1 = weather, 2 = cpu temperature
// 3 = freq governor. 4 = Little core frequency. 5 = mqqt last event
//
#define MAX_DISP_MODE 5
static int DispMode = 1;

//------------------------------------------------------------------------------------------------------------
//
// Faraday power supply:
//
//------------------------------------------------------------------------------------------------------------
char * faraday_serial_port = NULL;
faraday_psu_type_t faraday_psu_type = ft_unknown;
const faraday_reply_t * faraday_reply = NULL;

//------------------------------------------------------------------------------------------------------------
//
// Mosquitto:
//
//------------------------------------------------------------------------------------------------------------
typedef struct _client_info_t {
    struct mosquitto * m;
    pid_t pid;
    uint32_t tick_ct;
} t_client_info;

char * mqtt_host = "localhost";
char * mqtt_username = "owntracks";
char * mqtt_password = "zhopa";
int mqtt_port = 8883;
int mqtt_keepalive = 60;

static struct mosquitto * mosq = NULL;
static t_client_info client_info;
static pthread_t mosq_th = 0;

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

void wd_sleep(int secs) {
    extern int do_exit;
    int s = secs;
    while (s > 0 && !do_exit) {
        sleep(1);
        s--;
    }
}

typedef int (* daemon_command_callback_t)(void *);

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

const DAEMON_COMMAND_T daemon_commands[] = {
    {.command_name = "reconfigure", .command_callback = reconfigure_callback, .command_int = CMD_RECONFIGURE},
    {.command_name = "shutdown",    .command_callback = shutdown_callback,    .command_int = CMD_SHUTDOWN},
    {.command_name = "restart",     .command_callback = restart_callback,     .command_int = CMD_RESTART},
    {.command_name = "check",       .command_callback = check_callback,       .command_int = CMD_CHECK},
};

uint64_t timeMillis(void) {
    struct timeval time;
    gettimeofday(&time, NULL);
    return time.tv_sec * 1000UL + time.tv_usec / 1000UL;
}

//------------------------------------------------------------------------------------------------------------
//
// ina 219 stuff
//
//------------------------------------------------------------------------------------------------------------

#ifdef INA219
ina_219_device * ina_219_dev = NULL;
double ina_voltage = NAN;
double ina_current = NAN;
void ina_done(void);

int ina_init(void) {
    ina_219_dev = ina_219_device_open("/dev/i2c-1", 0x40);
    if (ina_219_dev == NULL) {
        return -1;
    }
    if (ina_219_device_config(ina_219_dev, INA_219_DEVICE_BUS_VOLTAGE_RANGE_32 |
                              INA_219_DEVICE_GAIN_8 |
                              INA_219_DEVICE_MODE_SHUNT |
                              INA_219_DEVICE_MODE_BUS |
                              INA_219_DEVICE_BADC_12_BIT_4_AVERAGE |
                              INA_219_DEVICE_SADC_12_BIT_4_AVERAGE) < 0) {

        DLOG_ERR("Unable to setup device ina_219 close it");
        ina_done();
        return -1;
    }
    if (ina_219_device_calibrate(ina_219_dev, 0.05, 3.0) < 0) {
        DLOG_ERR("Unable to calibrate device ina_219 close it");
        ina_done();
        return -1;
    }
    return 0;
}

void ina_done(void) {
    if (ina_219_dev) {
        ina_219_device_close(ina_219_dev);
        ina_219_dev = NULL;
    }
}

void ina_get(void) {
    if (ina_219_dev) {
        ina_voltage = ina_219_device_get_bus_voltage(ina_219_dev);
        ina_current = ina_219_device_get_current(ina_219_dev);
    }
}
#endif
//------------------------------------------------------------------------------------------------------------
//
// Get little core freq(CPU4)
//
//------------------------------------------------------------------------------------------------------------
#define FD_LITTLECORE_FREQ "/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq"

static void get_littlecore_freq(void) {
    int     fd, freq;
    char    buf[LCD_COL + 1] = {0};

    fill_lcdFb();

    if((fd = open(FD_LITTLECORE_FREQ, O_RDONLY)) < 0)   {
        daemon_log(LOG_ERR, "%s : file open error!", __func__);
    } else    {
        read(fd, buf, sizeof(buf));
        close(fd);
        freq = atoi(buf);
        int n = sprintf(buf, "Little-Core Freq");
        memmove(&lcdFb[0][0], buf, (size_t)n);
        n = sprintf(buf, "%d Mhz", freq / 1000);
        memmove(&lcdFb[1][4], buf, (size_t)n);
    }
}

//------------------------------------------------------------------------------------------------------------
//
// Get ethernet ip addr
//
//------------------------------------------------------------------------------------------------------------
static void get_ethernet_ip(void) {
    struct  ifaddrs * ifa;
    char    buf[LCD_COL] = {};

    fill_lcdFb();
    getifaddrs(&ifa);

    while(ifa)  {
        if(ifa->ifa_addr && ifa->ifa_addr->sa_family == AF_INET)    {
            struct sockaddr_in * pAddr = (struct sockaddr_in *)ifa->ifa_addr;

            if(0 == strncmp(ifa->ifa_name, "eth", 2)) {
                int n = sprintf(buf, "My IP Addr(%s)", ifa->ifa_name);
                memmove((char *)&lcdFb[0][0], buf, (size_t)n);

                n = sprintf(buf, "%s", inet_ntoa(pAddr->sin_addr));
                memmove((char *)&lcdFb[1][1], buf, (size_t)n);
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
    struct tm  * st_time;
    char        buf[LCD_COL] = {};

    fill_lcdFb();

    time(&tm_time);
    st_time = localtime(&tm_time);

    size_t n = strftime(buf, LCD_COL, "%Y/%m/%d %a", st_time);
    memmove(&lcdFb[0][0], buf, n);

    n = strftime(buf, LCD_COL, "%H:%M %p", st_time);
    memmove(&lcdFb[1][4], buf, n);
}

//------------------------------------------------------------------------------------------------------------
//
// Get CPU Temperature
//
//------------------------------------------------------------------------------------------------------------
//sensors coretemp-isa-0000 -j

#define NX_JSON_REPORT_ERROR(msg, ptr)\
    daemon_log(LOG_ERR,"%s",msg);

static double get_cpu_temperature_sensors(const char * sensor_name) {
    double ret = NAN;
    FILE * fp = NULL;
    char buf[1024] = {0};
    char * command = NULL;
    asprintf(&command, "/usr/bin/sensors -j %s", sensor_name);
    fp = popen(command, "r");
    if (fp == NULL) {
        daemon_log(LOG_ERR, "Failed to run command '%s'", command);
    } else {
        fread(buf, sizeof(buf) - 1, 1, fp);
        pclose(fp);
        const nx_json * json_temp = nx_json_parse(buf, NULL);

        nx_json_free(json_temp);
    }
    FREE(command);
    return ret;
}

#define FD_SYSTEM_TEMP_TMPL  "/sys/class/thermal/thermal_zone%d/temp"

static void get_cpu_temperature(int thermal_zone) {
    int     fd, temp_C, temp_F;
    char    buf[LCD_COL + 1] = {0};
    char   *  f_name = NULL;

    fill_lcdFb();
    asprintf(&f_name, FD_SYSTEM_TEMP_TMPL, thermal_zone);
    if((fd = open(f_name, O_RDONLY)) < 0)    {
        daemon_log(LOG_ERR, "%s : file open error!", __func__);
        daemon_log(LOG_INFO, "TEMP:%.2f", get_cpu_temperature_sensors("coretemp-isa-0000"));
    } else    {
        read(fd, buf, LCD_COL);
        close(fd);

        temp_C = atoi(buf) / 1000;
        temp_F = (temp_C * 18 + 320) / 10;

        int n = sprintf(buf, "CPU Temperature");
        memmove((char *)&lcdFb[0][0], buf, (size_t)n);
        n = sprintf(buf, "%3d *C, %3d.%1d *F", temp_C, temp_F, temp_F % 10);
        memmove((char *)&lcdFb[1][0], buf, (size_t)n);
    }
    FREE(f_name);
}

//------------------------------------------------------------------------------------------------------------
//
// Get weather info
//
//------------------------------------------------------------------------------------------------------------
#define BUF_SIZE 1024

static char * get_last_line_from_file(char * filename) {
    char buffer[BUF_SIZE];
    char * ret = NULL;

    if ((!filename) || (!*filename)) return(ret);

    FILE * fp = fopen(filename, "r");

    if (fp == NULL) {
        daemon_log(LOG_ERR, "%s : file:'%s' open error! %s", __func__, filename, strerror(errno));
        return(ret);
    }

    fseek(fp, -(BUF_SIZE), SEEK_END);
    size_t len = fread(buffer, 1, BUF_SIZE, fp);
    if (len > 0) {
        int pos[2] = { -1, -1};
        int i = (int)len - 1;
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
            ret = calloc(1, (size_t)l);
            memmove(ret, &buffer[pos[1] + 1], (size_t)(l - 1));
        }
    } else {
        daemon_log(LOG_ERR, "%s : file %s read error!", __func__, filename);
    }
    fclose(fp);
    return(ret);
}


//{"time" : "2018-01-17 15:56:37", "brand" : "OS", "model" : "THGR122N", "id" : 88, "channel" : 1, "battery" : "OK", "temperature_C" : -2.000, "humidity" : 77}

enum sensor_type_t {s_numeric, s_string};
typedef struct _sensor_t {
    char * name;
    double value_d;
    char * value_s;
    enum sensor_type_t sensor_type;
} sensor_t;

sensor_t * sensor_new(const char * name, const double value_d, const char * value_s) {
    sensor_t * ret = calloc(1, sizeof(*ret));
    ret->name = strdup(name);
    if (!isnan(value_d)) {
        ret->sensor_type = s_numeric;
        ret->value_d = value_d;
    } else {
        ret->sensor_type = s_string;
        ret->value_s = strdup(value_s);
        ret->value_d = NAN;
    }
    return ret;
}

void sensor_free(sensor_t ** sensor) {
    FREE((*sensor)->name);
    FREE((*sensor)->value_s);
    FREE(*sensor);
}

#define FD_EXT_SENSOR_FD  "/tmp/weather/rtl_433.log"
#define FD_INT_SENSOR_FD  "/tmp/weather/weather_board.log"

typedef struct _weather_t {
    char * location;
    array_t * sensors;
} weather_t;

weather_t * weather_new(const char * location) {
    weather_t * ret = calloc(1, sizeof(*ret));
    ret->location = strdup(location);
    ret->sensors = array_create();
    return ret;
}

void weather_free(weather_t ** weather) {
    if (!weather || !*weather) return;
    FREE((*weather)->location);
    array_destroy(&(*weather)->sensors, NULL, (freep_func_t)sensor_free);
    FREE(*weather);
}

void weather_clear(weather_t * weather) {
    if (weather) {
        array_clean(weather->sensors, NULL, (freep_func_t)sensor_free);
    }
}

sensor_t * sensor_by_name(weather_t * weather, const char * sensor_name) {
    if ((weather) && (weather->sensors) && (sensor_name) && (strlen(sensor_name) > 0)) {
        array_for_each(weather->sensors, i) {
            sensor_t * sensor = array_getitem(weather->sensors, i);
            if ((sensor) && (sensor->name) && (strcmp(sensor_name, sensor->name) == 0)) {
                return sensor;
            }
        }
    }
    return(NULL);
}

double sensor_value_d_by_name(weather_t * weather, const char * sensor_name) {
    sensor_t * sensor = sensor_by_name(weather, sensor_name);
    if (sensor) return sensor->value_d;
    return NAN;
}

__attribute__ ((unused)) static void json_print(int level, const nx_json * json) {
    daemon_log(LOG_INFO, "%*s%s", level, "", json->key);
    if (json->child) {
        json_print(level + 1, json->child);
    }
    if (json->next) {
        json_print(level, json->next);
    }
}

static void add_sensor_value(weather_t * weather, const nx_json * json) {
    if ((!json) || (!json->key) || (!strlen(json->key))) {
        return;
    }
    sensor_t * sensor = NULL;
    switch (json->type) {
    case NX_JSON_STRING:
        sensor = sensor_new(json->key, NAN, json->text_value);
        break;
    case NX_JSON_DOUBLE:
        sensor = sensor_new(json->key, json->dbl_value, NULL);
        break;
    case NX_JSON_INTEGER:
    case NX_JSON_BOOL:
        sensor = sensor_new(json->key, json->int_value, NULL);
        break;
    default:
        break;
    }
    if ((sensor) && (weather) && (weather->sensors)) {
        array_append(weather->sensors, sensor);
    }
}

static void json_recursive_add_sensors(weather_t * weather, const nx_json * json) {

    add_sensor_value(weather, json);
    if (json->child) {
        json_recursive_add_sensors(weather, json->child);
    }
    if (json->next) {
        json_recursive_add_sensors(weather, json->next);
    }
}

static void get_weather_from_json(weather_t * weather, char * txt_json) {
    if ((!txt_json) || (!*txt_json) || (!weather)) return;

    const nx_json * json = nx_json_parse_utf8(txt_json);

    if (json) {
        weather_clear(weather);
        json_recursive_add_sensors(weather, json);
        nx_json_free(json);
    }
}

__attribute__ ((unused)) static void publist_weather_topic(char * template, char * location, char * name, double value) {
    char topic[256];
    sprintf(topic, template, location, name);
    publish_double(topic, value);
}

static char * sensor_print(char * buffer, const sensor_t * sensor) {
    char * eb_buf = buffer + strlen(buffer);
    switch (sensor->sensor_type) {
    case s_numeric:
        sprintf(eb_buf, "\"%s\": %.4f", sensor->name, sensor->value_d);
        size_t l = strlen(eb_buf);
        char * tmp = eb_buf + l - 1;
        while ((*tmp == '0') && (tmp > eb_buf + 1)) {
            *tmp = 0;
            tmp--;
        }
        if (*tmp == '.')
            *tmp = 0;
        break;
    case s_string:
        sprintf(eb_buf, "\"%s\": \"%s\"", sensor->name, sensor->value_s);
        break;
    }
    return buffer;
}

#define MQTT_LWT_TOPIC "tele/%s/LWT"
#define MQTT_SENSOR_TOPIC "tele/%s/SENSOR"
#define MQTT_STATE_TOPIC "tele/%s/STATE"
#define MQTT_PIAWARE_TOPIC "tele/%s/PIAWARE"

const char * create_topic(const char * template) {
    static __thread char buf[255] = {0};
    snprintf(buf, sizeof(buf) - 1, template, hostname);
    return buf;
}

#define WEATHER_INT 0
#define WEATHER_EXT 1
#define WEATHER_COUNT 2
weather_t * weather[WEATHER_COUNT] = {0};

void publish_sensors(weather_t * cur_weather[]) {
    if (no_weather) {
        return;
    }
    static uint64_t timer_publish_state = 0;
    if (timer_publish_state >  timeMillis()) return;
    else {
        timer_publish_state = timeMillis() + SENSORS_PUBLISH_INTERVAL;
    }

    const char * topic = create_topic(MQTT_SENSOR_TOPIC);

    time_t timer;
    char tm_buffer[26] = {0};
    char buf[1024] = {0};
    struct tm * tm_info;

    int res;

    time(&timer);
    tm_info = localtime(&timer);
    strftime(tm_buffer, 26, "%Y-%m-%dT%H:%M:%S", tm_info);

    strcat(buf, "{\"Time\":\"");
    strcat(buf, tm_buffer);
    strcat(buf, "\",");

    for (int w = 0; w < WEATHER_COUNT; w++) {
        char wbuf[512] = "";
        int c = 0;
        strcat(strcat(strcat(wbuf, "\""), cur_weather[w]->location), "\":{");
        array_for_each(cur_weather[w]->sensors, i) {
            sensor_t * sensor = array_getitem(cur_weather[w]->sensors, i);
            sensor_print(wbuf, sensor);
            strcat(wbuf, ",");
            c++;
        }
        wbuf[strlen(wbuf) - 1] = 0;
        strcat(wbuf, "},");
        if (c) {
            strcat(buf, wbuf);
        }
    }
    size_t l = strlen(buf);
    if (l > 1) {
        buf[strlen(buf) - 1] = 0;
    }
    strcat(buf, "}");
    daemon_log(LOG_INFO, "%s %s", topic, buf);
    if ((res = mosquitto_publish (mosq, NULL, topic, (int)strlen (buf), buf, 0, false)) != 0) {
        daemon_log(LOG_ERR, "Can't publish to Mosquitto server %s", mosquitto_strerror(res));
    }
}
/*
"last1min": {
    "start": 1542814193.6,
    "end": 1542814253.6,
    "local": {
      "samples_processed": 144048128,
      "samples_dropped": 0,
      "modeac": 0,
      "modes": 1409430,
      "bad": 905099,
      "unknown_icao": 498685,
      "accepted": [
        5262,
        384
      ],
      "signal": -8,
      "noise": -24.4,
      "peak_signal": -0.9,
      "strong_signals": 165
    },
    "remote": {
      "modeac": 0,
      "modes": 81,
      "bad": 0,
      "unknown_icao": 0,
      "accepted": [
        81,
        0
      ]
    },
    "cpr": {
      "surface": 9,
      "airborne": 973,
      "global_ok": 948,
      "global_bad": 0,
      "global_range": 0,
      "global_speed": 0,
      "global_skipped": 9,
      "local_ok": 21,
      "local_aircraft_relative": 0,
      "local_receiver_relative": 0,
      "local_skipped": 13,
      "local_range": 0,
      "local_speed": 0,
      "filtered": 0
    },
    "altitude_suppressed": 0,
    "cpu": {
      "demod": 9192,
      "reader": 2427,
      "background": 461
    },
    "tracks": {
      "all": 5,
      "single_message": 6
    },
    "messages": 5727

*/


const nx_json * read_and_parse_json(char * filename) {
    const nx_json * json = NULL;
    int fd = open (filename, O_RDONLY);
    if (fd < 0) {
        daemon_log(LOG_ERR, "Unable to open file %s (%d) %s", filename, errno, strerror(errno));
    }
    struct stat s;
    if (!fstat (fd, &s)) {
        long int size = s.st_size;
        char * f = (char *) mmap (0, (size_t)size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (f) {
            json = nx_json_parse_utf8(strndupa(f, (size_t)size));
            if (!json) {
                daemon_log(LOG_ERR, "Unable to parse json from file %s %.*s", filename, (int)size, f);
            }
            munmap(f, (size_t)size);
        }
    } else {
        daemon_log(LOG_ERR, "Unable to stat file %s (%d) %s", filename, errno, strerror(errno));
    }
    close(fd);
    return json;
}

double get_mps() {
    double last_mps = NAN;
    const nx_json * root = read_and_parse_json("/tmp/dump1090/stats.json");
    if (root) {
        int messages = 0;
        double start = 0., end = 0.;
        const nx_json * json = nx_json_get(root, "last1min");
        if (json) {
            const nx_json * item = nx_json_get(json, "messages");
            if (item) {
                messages = item->int_value;
            }
            item = nx_json_get(json, "start");
            if (item) {
                start = item->dbl_value;
            }
            item = nx_json_get(json, "end");
            if (item) {
                end = item->dbl_value;
            }
            if ((end - start) != 0) {
                last_mps = messages / (end - start);
            }
        }
        nx_json_free(json);
    }
    return(last_mps);
}

//calculate haversine distance for linear distance
double haversine_km(double lat1, double long1, double lat2, double long2) {
    double dlong = (long2 - long1) * d2r;
    double dlat = (lat2 - lat1) * d2r;
    double a = pow(sin(dlat / 2.0), 2) + cos(lat1 * d2r) * cos(lat2 * d2r) * pow(sin(dlong / 2.0), 2);
    double c = 2 * atan2(sqrt(a), sqrt(1 - a));
    double d = 6367 * c;

    return d;
}

int get_aircrafts(double * md) {
    int last_aircrafts = 0;
    double max_dist = -1.0;
    const nx_json * root = read_and_parse_json("/tmp/dump1090/aircraft.json");
    if (root) {
        const nx_json * jaircrafts = nx_json_get(root, "aircraft");
        if ((jaircrafts) && (jaircrafts->type == NX_JSON_ARRAY)) {
            last_aircrafts = jaircrafts->length;
            for ( int i = 0; i < last_aircrafts; i++ ) {
                const nx_json * jaircraft = nx_json_item(jaircrafts, i);
                if (jaircraft) {
                    const nx_json * jlat =  nx_json_get(jaircraft, "lat");
                    const nx_json * jlon =  nx_json_get(jaircraft, "lon");
                    if (jlon && jlat) {
                        double lat = jlat->dbl_value;
                        double lon = jlon->dbl_value;
                        if (lat != 0.0 && lon != 0.0) {
                            double dist = haversine_km(station_lat, station_lon, lat, lon);
                            if (dist > max_dist) {
                                max_dist = dist;
                            }
                            const nx_json * jhex = nx_json_get(jaircraft, "hex");
                            DLOG_DEBUG("%-10s -> %6.2f lat: %2.4f lon: %2.4f", jhex ? jhex->text_value : "unk", dist, lat, lon);
                        }
                    }
                }
            }
        }
        nx_json_free(root);
    }
    DLOG_DEBUG("MAX DIST:%6.2f %p", max_dist, md);
    if (md) {
        *md = max_dist;
    }
    return(last_aircrafts);
}

void publish_piaware(void) {
    if (no_aircraft) {
        return;
    }
    static uint64_t timer_publish_state = 0;

    if (timer_publish_state >  timeMillis()) return;
    else {
        timer_publish_state = timeMillis() + PIAWARE_PUBLISH_INTERVAL;
    }

    const char * topic =  create_topic(MQTT_PIAWARE_TOPIC);

    time_t timer;
    char tm_buffer[26] = {};
    char buf[1024] = {};
    struct tm * tm_info;

    time(&timer);
    tm_info = localtime(&timer);
    strftime(tm_buffer, 26, "%Y-%m-%dT%H:%M:%S", tm_info);
    double md = 0;
    int ac = get_aircrafts(&md);
    snprintf(buf, sizeof(buf) - 1, "{\"Time\":\"%s\", \"Piaware\": {\"Aircraft\": %d, \"Messages\": %.2f, \"MaximumDistance\": %.2f}}",
             tm_buffer, ac, get_mps(), md);

    daemon_log(LOG_INFO, "%s %s", topic, buf);
    int res;
    if ((res = mosquitto_publish (mosq, NULL, topic, (int)strlen(buf), buf, 0, false)) != 0) {
        daemon_log(LOG_ERR, "Can't publish to Mosquitto server %s", mosquitto_strerror(res));
    }
}
#define ONLINE "Online"
#define OFFLINE "Offline"

void mqtt_publish_lwt(bool online) {
    const char * msg = online ? ONLINE : OFFLINE ;
    int res;
    const char * topic = create_topic(MQTT_LWT_TOPIC);
    daemon_log(LOG_INFO, "publish %s: %s", topic, msg);
    if ((res = mosquitto_publish (mosq, NULL, topic, (int)strlen(msg), msg, 0, true)) != 0) {
        DLOG_ERR("Can't publish to Mosquitto server %s", mosquitto_strerror(res));
    }
}

static void publish_state(void) {

    static uint64_t timer_publish_state = 0;

    if (timer_publish_state >  timeMillis()) return;
    else {
        timer_publish_state = timeMillis() + STATE_PUBLISH_INTERVAL;
    }

    time_t timer;
    char tm_buffer[26] = {};
    char buf[255] = {};
    struct tm * tm_info;
    struct sysinfo info;
    int res;

    time(&timer);
    tm_info = localtime(&timer);
    strftime(tm_buffer, 26, "%Y-%m-%dT%H:%M:%S", tm_info);

    if (!sysinfo(&info)) {
        int fd;
        char tmp_buf[20];
        memset(tmp_buf, ' ', sizeof(tmp_buf));
        char * f_name = NULL;
        asprintf(&f_name, FD_SYSTEM_TEMP_TMPL, thermal_zone);
        if((fd = open(f_name, O_RDONLY)) < 0)    {
            daemon_log(LOG_ERR, "%s : file open error!", __func__);
        } else    {
            read(fd, buf, sizeof(tmp_buf));
            close(fd);
        }
        FREE(f_name);
        int temp_C = atoi(buf) / 1000;
        const char * topic = create_topic(MQTT_STATE_TOPIC);

#ifdef INA219
        if (isnan(ina_current) || isnan(ina_voltage)) {
#endif
       faraday_reply = read_faraday_data(faraday_serial_port, faraday_psu_type);
       static double faraday_voltage_prev = NAN;
       static double faraday_current_prev = NAN;
       double faraday_voltage = faraday_voltage_prev;
       double faraday_current = faraday_current_prev;
       if (faraday_reply) {
    	    faraday_voltage = faraday_reply->voltage_batt/10.;
            faraday_current = faraday_reply->ac220!=0?faraday_reply->current_batt/10.:faraday_reply->current_batt/-10.;
       }
       if (isnan(faraday_voltage) || isnan(faraday_current)) {
           snprintf(buf, sizeof(buf) - 1, "{\"Time\":\"%s\", \"Uptime\": %ld, \"LoadAverage\":%.2f, \"CPUTemp\":%d}",
                    tm_buffer, info.uptime / 3600, info.loads[0] / 65536.0, temp_C);
       } else {
	   snprintf(buf, sizeof(buf) - 1,
	    "{\"Time\":\"%s\", \"Uptime\": %ld, \"LoadAverage\":%.2f, \"CPUTemp\":%d,\"Current\":%0.3f, \"Voltage\":%0.3f}",
                    tm_buffer, info.uptime / 3600, info.loads[0] / 65536.0, temp_C,
                    faraday_current, faraday_voltage);
	    faraday_voltage_prev = faraday_voltage;
            faraday_current_prev = faraday_current;
        }
#ifdef INA219
        } else {
    	   snprintf(buf, sizeof(buf) - 1, "{\"Time\":\"%s\", \"Uptime\": %ld, \"LoadAverage\":%.2f, \"CPUTemp\":%d, \"Current\":%0.3f, \"Voltage\":%0.3f}",
                    tm_buffer, info.uptime / 3600, info.loads[0] / 65536.0, temp_C, ina_current, ina_voltage);
        }
#endif
        daemon_log(LOG_INFO, "%s %s", topic, buf);
        if ((res = mosquitto_publish (mosq, NULL, topic, (int)strlen(buf), buf, 0, false)) != 0) {
            daemon_log(LOG_ERR, "Can't publish to Mosquitto server %s", mosquitto_strerror(res));
        }
    }
}



void get_weather_info(weather_t * weather[]) {

    char * txt_json;
    if (no_weather) {
        return;
    }
    txt_json = get_last_line_from_file(FD_EXT_SENSOR_FD);
    if (txt_json) {
        get_weather_from_json(weather[WEATHER_EXT], txt_json);
        free(txt_json);
    }


    txt_json = get_last_line_from_file(FD_INT_SENSOR_FD);
    if (txt_json) {
        get_weather_from_json(weather[WEATHER_INT], txt_json);
        free(txt_json);
    }

}

static void display_weather_info(weather_t * weather[]) {
    char    buf[LCD_COL + 1];
    int     n;
    memset(buf, 0, sizeof(buf));
    fill_lcdFb();
#ifdef INA219
    n = snprintf(buf, sizeof(buf), "%2s%5.1f %2.0f%% %0.2f",
                 weather[WEATHER_INT]->location,
                 sensor_value_d_by_name(weather[WEATHER_INT], "temperature_C"),
                 roundf(sensor_value_d_by_name(weather[WEATHER_INT], "humidity")),
                 ina_voltage);
#else
    if (faraday_reply) {
	n = snprintf(buf, sizeof(buf), "%2s%5.1f %2.0f%% %0.2f",
                    weather[WEATHER_INT]->location,
                    sensor_value_d_by_name(weather[WEATHER_INT], "temperature_C"),
                    roundf(sensor_value_d_by_name(weather[WEATHER_INT], "humidity")),
                    faraday_reply->voltage_batt/10.);
    } else {
	n = snprintf(buf, sizeof(buf), "%2s%5.1f %2.0f%%",
                    weather[WEATHER_INT]->location,
                    sensor_value_d_by_name(weather[WEATHER_INT], "temperature_C"),
                    roundf(sensor_value_d_by_name(weather[WEATHER_INT], "humidity")));
    }
#endif
    memmove((char *)&lcdFb[0][0], buf, (size_t)n);
    n = snprintf(buf, sizeof(buf), "%2s%5.1f %2.0f%% %3.0f",
                 weather[WEATHER_EXT]->location,
                 sensor_value_d_by_name(weather[WEATHER_EXT], "temperature_C"),
                 roundf(sensor_value_d_by_name(weather[WEATHER_EXT], "humidity")),
                 roundf(0.750062 * sensor_value_d_by_name(weather[WEATHER_INT], "pressure")));
    memmove((char *)&lcdFb[1][0], buf, (size_t)n);
}

//------------------------------------------------------------------------------------------------------------
//
// Display last mqqt ON OFF event
//
//------------------------------------------------------------------------------------------------------------
static char * mqtt_display1 = NULL;
static char * mqtt_display2 = NULL;

static void get_mqqt_last_event(void) {
    char    buf[LCD_COL];
    int     n;
    memset(buf, 0, sizeof(buf));
    fill_lcdFb();

    n = snprintf(buf, sizeof(buf), "%*s%*s", (int)(LCD_COL / 2 + strlen(mqtt_display1) / 2), mqtt_display1, (int)(LCD_COL / 2 - strlen(mqtt_display1) / 2), "");
    memmove((char *)&lcdFb[0][0], buf, (size_t)(n));
    n = snprintf(buf, sizeof(buf), "%*s%*s", (int)(LCD_COL / 2 + strlen(mqtt_display2) / 2), mqtt_display2, (int)(LCD_COL / 2 - strlen(mqtt_display2) / 2), "");
    memmove((char *)&lcdFb[1][0], buf, (size_t)(n));
}

//------------------------------------------------------------------------------------------------------------
//
// LCD Update Function:
//
//------------------------------------------------------------------------------------------------------------
pthread_mutex_t lcd_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t lcd_cond = PTHREAD_COND_INITIALIZER;

static void lcd_update (void) {

    // lcd fb update
    switch(DispMode)    {
    default  :
        DispMode = 0;
        break;
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
        get_cpu_temperature(thermal_zone);
        break;
    case    4:
        get_littlecore_freq();
        break;
    case    5:
        get_mqqt_last_event();
        break;
    }
    if (no_display) {
        return;
    }
    if (memcmp(lcdFbTD, lcdFb, sizeof(lcdFbTD))) {
        pthread_mutex_lock(&lcd_mutex);
        memmove(lcdFbTD, lcdFb, sizeof(lcdFbTD));
        pthread_cond_signal(&lcd_cond);
        pthread_mutex_unlock(&lcd_mutex);
    }
}

static void * lcd_updater(void * UNUSED(p)) {
    struct timespec ts;
    while (!do_exit) {
        pthread_mutex_lock(&lcd_mutex); // acquire the lock
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 1;
        if (pthread_cond_timedwait(&lcd_cond, &lcd_mutex, &ts) != ETIMEDOUT) {
#ifdef CHARLCD
            CharLCD_setCursor(lcd, 0, 0);
            CharLCD_print(lcd, &lcdFbTD[0][0]);
            CharLCD_setCursor(lcd, 0, 1);
            CharLCD_print(lcd, &lcdFbTD[1][0]);
#endif
        }
        pthread_mutex_unlock(&lcd_mutex);
    }
    return NULL;
}

#ifdef CHARLCD
void backlight(CharLCD_t * lcd, int  light_on) { // 1 -on, 0 - off, 2 - switch
    if (!lcd) return;

    static int clr = 1;
    if (light_on == 1) clr = 0;
    if (light_on == 0) clr = 1;
    clr ++;
    if (clr % 2 == 0) {
        CharLCD_setBacklight(lcd, BLACK);
    } else {
        CharLCD_setBacklight(lcd, WHITE);
    }
}
#endif

//------------------------------------------------------------------------------------------------------------
//
// system init
//
//------------------------------------------------------------------------------------------------------------
#ifdef CHARLCD
int lcd_and_buttons_init(void) {

    if (no_display) {
        return 0;
    }
    // LCD Init
    lcd = CharLCD_new(1, 0x20);

    if(lcd == NULL)   {
        daemon_log(LOG_ERR, "%s : CharLCD_new failed!", __func__);
        no_display = 1;
        return -1;
    }
    CharLCD_start(lcd, 16, 2);
    CharLCD_display(lcd);

    backlight(lcd, 1);
    return  0;
}
#endif

//------------------------------------------------------------------------------------------------------------
//
// board data update
//
//------------------------------------------------------------------------------------------------------------

bool boardDataUpdate(void) {

    int update_flag = 0;
#ifdef CHARLCD
    uint8_t button_data = CharLCD_readButtons(lcd);
    static bool BUTTON_UP_press = false;
    static bool BUTTON_DOWN_press = false;
    static bool BUTTON_SELECT_press = false;

    if (button_data & BUTTON_UP) {
        if (BUTTON_UP_press == false) {
            BUTTON_UP_press = true;
            update_flag = 1;
            DispMode ++;
            BUTTON_UP_press = true;
        }
    } else {
        BUTTON_UP_press = false;
    }

    if (button_data & BUTTON_DOWN) {
        if (BUTTON_DOWN_press == false) {
            BUTTON_DOWN_press = true;
            update_flag = 1;
            DispMode --;
            BUTTON_DOWN_press = true;
        }
    } else {
        BUTTON_DOWN_press = false;
    }

    if (button_data & BUTTON_SELECT) {
        if (!BUTTON_SELECT_press) {
            BUTTON_SELECT_press = true;
            backlight(lcd, 2);
        }
    } else {
        BUTTON_SELECT_press = false;
    }
#endif
    DispMode = DispMode % MAX_DISP_MODE;
    //daemon_log(LOG_INFO, "%d %d", DispMode, update_flag);
    return update_flag;

}


void on_log(struct mosquitto * UNUSED(mosq), void * UNUSED(userdata), int level, const char * str) {
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
void on_connect(struct mosquitto * m, void * UNUSED(udata), int res) {
    daemon_log(LOG_INFO,"%s",__FUNCTION__);
    switch (res) {
    case 0:
        mosquitto_subscribe(m, NULL, "stat/+/POWER", 0);
#ifdef CHARLCD
        mosquitto_subscribe(m, NULL, "tele/main-power/LWT", 0);
#else
    #ifdef ZIGBEEGATE
	mosquitto_subscribe(m, NULL, "tele/main-power/LWT", 0);
    #endif
#endif
        mqtt_publish_lwt(true);
        publish_state();
        break;
    case 1:
        DLOG_ERR("Connection refused (unacceptable protocol version).");
        break;
    case 2:
        DLOG_ERR("Connection refused (identifier rejected).");
        break;
    case 3:
        DLOG_ERR("Connection refused (broker unavailable).");
        break;
    default:
        DLOG_ERR("Unknown connection error. (%d)", res);
        break;
    }
    if (res != 0) {
        wd_sleep(10);
    }
}

static
void on_publish(struct mosquitto * UNUSED(m), void * UNUSED(udata), int UNUSED(m_id)) {
    //daemon_log(LOG_ERR, "-- published successfully");
}

static
void on_subscribe(struct mosquitto * UNUSED(m), void * UNUSED(udata), int UNUSED(mid),
                  int UNUSED(qos_count), const int * UNUSED(granted_qos)) {
    daemon_log(LOG_INFO, "-- subscribed successfully");
}

regex_t mqtt_topic_regex;


#ifdef CHARLCD
static void rtl_433_control(bool start) {
    int ret = 0;
    if (start) {
        daemon_log(LOG_INFO,"restart rtl_433");
	ret = system("systemctl restart rtl_433-daemon.service");
    } else {
        daemon_log(LOG_INFO,"stop rtl_433");
	ret = system("systemctl stop rtl_433-daemon.service");
    }
    daemon_log(LOG_INFO,"system systemctl result %d", ret);
}
#endif

#ifdef ZIGBEEGATE
static void zigbee2mqtt_service_restart(void) {
    int ret = 0;
    daemon_log(LOG_INFO,"restart zigbee2mqtt.service");
    ret = system("systemctl restart zigbee2mqtt.service");
    daemon_log(LOG_INFO,"system systemctl result %d", ret);
}
#endif

static
void on_message(struct mosquitto * UNUSED(m), void * UNUSED(udata),
                const struct mosquitto_message * msg) {
    if (msg == NULL) {
        return;
    }

    daemon_log(LOG_INFO, "-- got message @ %s: (%d, QoS %d, %s) '%s'",
               msg->topic, msg->payloadlen, msg->qos, msg->retain ? "R" : "!r",
               (char *)msg->payload);

#ifdef ZIGBEEGATE
    if (strcmp(msg->topic, "tele/main-power/LWT")==0) {
        struct sysinfo info = {0};
	bool flag = sysinfo(&info) == 0 && info.uptime > 120;
	if (flag && strcasecmp(msg->payload,"online")==0) {
	    zigbee2mqtt_service_restart();
        } else {
	     daemon_log(LOG_INFO, "not need to restart zigbee2mqtt"); 
	}
    }
#endif

#ifdef CHARLCD
    if (strcmp(msg->topic, "tele/main-power/LWT")==0) {
	if (strcasecmp(msg->payload,"online")==0) {
            backlight(lcd,1);
	    rtl_433_control(true);
        } else {
            backlight(lcd,0);
	    rtl_433_control(false);
	}
    }
#endif

    int ret = regexec(&mqtt_topic_regex, msg->topic, 0, NULL, 0);
    if (!ret) {
        daemon_log(LOG_INFO, "--  Accept message");
        char * topic_copy = strdupa(msg->topic);
        char * start = strchr(topic_copy, '/');
        char * end = strrchr(topic_copy, '/');
        if (start != end) {
            *end = 0;
            start++;
            asprintf(&mqtt_display1, "%s", start);
            asprintf(&mqtt_display2, "%s", (char *)msg->payload);
            DispMode = 5;
        }
    } else if (ret == REG_NOMATCH) {
        daemon_log(LOG_INFO, "--  Skip message");
    } else {
        char msgbuf[100] = {};
        regerror(ret, &mqtt_topic_regex, msgbuf, sizeof(msgbuf));
        daemon_log(LOG_ERR, "Regex match failed: %s", msgbuf);
    }
    //t_client_info *info = (t_client_info *)udata;
}

static
void * mosq_thread_loop(void * p) {
    t_client_info * info = (t_client_info *)p;
    daemon_log(LOG_INFO, "%s", __FUNCTION__);
    while (!do_exit) {
        int res = mosquitto_loop(info->m, 1000, 1);
        switch (res) {
        case MOSQ_ERR_SUCCESS:
            break;
        case MOSQ_ERR_NO_CONN: {
            int res = mosquitto_connect(mosq, mqtt_host, mqtt_port, mqtt_keepalive);
            if (res) {
                daemon_log(LOG_ERR, "Can't connect to Mosquitto server %s", mosquitto_strerror(res));
                sleep(30);
            }
            break;
        }
        case MOSQ_ERR_INVAL:
        case MOSQ_ERR_NOMEM:
        case MOSQ_ERR_CONN_LOST:
        case MOSQ_ERR_PROTOCOL:
        case MOSQ_ERR_ERRNO:
            daemon_log(LOG_ERR, "%s %s %s", __FUNCTION__, strerror(errno), mosquitto_strerror(res));
            mosquitto_disconnect(mosq);
            daemon_log(LOG_ERR, "%s disconnected", __FUNCTION__);
            sleep(10);
            daemon_log(LOG_ERR, "%s Try to reconnect", __FUNCTION__);
            int res = mosquitto_connect(mosq, mqtt_host, mqtt_port, mqtt_keepalive);
            if (res) {
                daemon_log(LOG_ERR, "%s Can't connect to Mosquitto server %s", __FUNCTION__, mosquitto_strerror(res));
            } else {
                daemon_log(LOG_ERR, "%s Connected", __FUNCTION__);
            }

            break;
        default:
            daemon_log(LOG_ERR, "%s unkown error (%d) from mosquitto_loop", __FUNCTION__, res);
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
    char * tmp = alloca(strlen(progname) + strlen(hostname) + 2);
    strcpy(tmp, progname);
    strcat(tmp, "@");
    strcat(tmp, hostname);
    mosq = mosquitto_new(tmp, clean_session, &client_info);
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
        mosquitto_will_set(mosq, create_topic(MQTT_LWT_TOPIC), strlen(OFFLINE), OFFLINE, 0, true);
        daemon_log(LOG_INFO, "Try connect to Mosquitto server as %s", tmp);
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
    if ((res = mosquitto_publish (mosq, NULL, topic, (int)strlen(text), text, 0, true)) != 0) {
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
void * main_loop (void * UNUSED(p)) {
    uint64_t timer_auto_sw = 0;
    uint64_t timer_display = 0;
    daemon_log(LOG_INFO, "%s", __FUNCTION__);


    weather[WEATHER_EXT] = weather_new("EX");
    weather[WEATHER_INT] = weather_new("IN");


    while(!do_exit)    {
        if (boardDataUpdate()) {
            timer_auto_sw = timeMillis() + LCD_AUTO_SW_TIMEOUT;
            timer_display = 0;
        } else {
            usleep(10000);
        }

        if (timeMillis() >= timer_auto_sw) {
            timer_auto_sw = timeMillis() + LCD_AUTO_SW_TIMEOUT;
            DispMode++;
            DispMode &= 1;
        }

        if (timeMillis() >= timer_display) {
            timer_display = timeMillis() + LCD_UPDATE_PERIOD;
            get_weather_info(weather);

#ifdef INA219
            ina_get();
#endif
            publish_sensors(weather);
            publish_state();
            publish_piaware();
            lcd_update();
        }
    }
    weather_free(&weather[WEATHER_EXT]);
    weather_free(&weather[WEATHER_INT]);
    daemon_log(LOG_INFO, "%s finished", __FUNCTION__);
    pthread_exit(NULL);
}


//------------------------------------------------------------------------------------------------------------
//
// Usage:
//
//------------------------------------------------------------------------------------------------------------
static void usage() {
}


//------------------------------------------------------------------------------------------------------------
//
// Start Program:
//
//------------------------------------------------------------------------------------------------------------
const char * mqtt_topic_regex_string = "^stat\\/.*\\/POWER$";

int
main (int argc, char * const * argv) {
    int flags;
    int daemonize = true;
    int debug = 0;
    char * command = NULL;
    pid_t pid;
    pthread_t main_th = 0;
    pthread_t updater_th = 0;

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
        strncpy(pathname, argv[0], (size_t)(strrchr(argv[0], '/') - argv[0]) + 1);
    }

    if (chdir(pathname) < 0) {
        daemon_log(LOG_ERR, "chdir error: %s", strerror(errno));
    }

    FREE(pathname);

    pathname = get_current_dir_name();

    hostname = calloc(1, HOSTNAME_SIZE);
    gethostname(hostname, HOSTNAME_SIZE - 1);

    daemon_log_upto(LOG_INFO);
    daemon_log(LOG_INFO, "%s %s", pathname, progname);

    static
    struct option long_options[] = {
        {"command",                     required_argument,  0, 'k'},
        {"ident",                       required_argument,  0, 'i'},
        {"foreground",                  no_argument,        0, 'f'},
        {"debug",                       no_argument,        0, 'd'},
        {"mqtt-host",                   required_argument,  0, 'h'},
        {"mqtt-port",                   required_argument,  0, 'p'},
        {"mqtt-user",                   required_argument,  0, 'u'},
        {"mqtt-password",               required_argument,  0, 'P'},
        {"lat",                         required_argument,  0, 'l'},
        {"lon",                         required_argument,  0, 'L'},
        {"no-display",                  no_argument,        0, 'D'},
        {"no-weather",                  no_argument,        0, 'V'},
        {"aircraft",                    no_argument,        0, 'A'},
        {"thremal-zone",                required_argument,  0, 'T'},
        {"faraday-serial",              required_argument,  0, 'F'},
        {"faraday-psu",                 required_argument,  0, 'S'},
        {0, 0, 0, 0}
    };


    while ((flags = getopt_long(argc, argv, "i:fdk:h:p:u:P:R:VDAl:L:T:F:S:", long_options, NULL)) != -1) {

        switch (flags) {
        case 'i': {
            daemon_pid_file_ident = daemon_log_ident = xstrdup(optarg);
            break;
        }
        case 'f' : {
            daemonize = false;
            break;
        }
        case 'A': {
            no_aircraft = 0;
            break;
        }
        case 'V': {
            no_weather++;
            break;
        }
        case 'D': {
            no_display++;
            break;
        }
        case 'd': {
            debug++;
            daemon_log_upto(LOG_DEBUG);
            break;
        }
        case 'R': {
            mqtt_topic_regex_string = xstrdup(optarg);
            break;
        }
        case 'k': {
            command = xstrdup(optarg);
            break;
        }
        case 'p': {
            mqtt_port = atoi(optarg);
            break;
        }
        case 'u': {
            mqtt_username = xstrdup(optarg);
            break;
        }
        case 'P': {
            mqtt_password = xstrdup(optarg);
            break;
        }
        case 'h': {
            mqtt_host = xstrdup(optarg);
            break;
        }
        case 'l': {
            char * tmp_s;
            double tmp = strtod(optarg, &tmp_s);
            if (tmp_s != optarg) {
                station_lat = tmp;
            } else {
                usage();
            }
            break;
        }
        case 'L': {
            char * tmp_s;
            double tmp = strtod(optarg, &tmp_s);
            if (tmp_s != optarg) {
                station_lon = tmp;
            } else {
                usage();
            }
            break;
        }
        case 'T': {
            thermal_zone = atoi(optarg);
            break;
        }
        case 'F': {
            faraday_serial_port = strdup(optarg);
            break;
        }
        case 'S': {
            faraday_psu_type = atoi(optarg);
	    if (faraday_psu_type == 0) {
		usage();
	    }
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
            core_lim.rlim_cur = ULONG_MAX;
            core_lim.rlim_max = ULONG_MAX;
            if (setrlimit(RLIMIT_CORE, &core_lim) < 0) {
                daemon_log(LOG_ERR, "setrlimit RLIMIT_CORE error:%s", strerror(errno));
            } else {
                daemon_log(LOG_INFO, "core limit set cur:%2ld max:%2ld", core_lim.rlim_cur, core_lim.rlim_max );
            }
        }
        main_pid = syscall(SYS_gettid);
#ifdef CHARLCD
        if (lcd_and_buttons_init() < 0) {
            daemon_log(LOG_ERR, "%s: DISPLAY Init failed", __func__);
            goto finish;
        }
#endif
        daemon_log(LOG_INFO, "Compiling regexp: '%s'", mqtt_topic_regex_string);
        int reti = regcomp(&mqtt_topic_regex, mqtt_topic_regex_string, 0);
        if (reti) {
            daemon_log(LOG_ERR, "Could not compile regex '%s'", mqtt_topic_regex_string);
            goto finish;
        }

        mosq_init();
#ifdef INA219
        ina_init();
#endif
        sleep(1);

        pthread_create( &main_th, NULL, main_loop, NULL);
        pthread_create( &updater_th, NULL, lcd_updater, NULL);
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
#ifdef CHARLCD
                    backlight(lcd,2);
#endif
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
    mqtt_publish_lwt(false);
    daemon_log(LOG_INFO, "Exiting...");
    regfree(&mqtt_topic_regex);
    mosq_destroy();
    pthread_join(main_th, NULL);
    pthread_join(updater_th, NULL);
#ifdef CHARLCD
    CharLCD_destroy(&lcd);
#endif
#ifdef INA219
    ina_done();
#endif
    FREE(hostname);
    FREE(pathname);
    daemon_retval_send(-1);
    daemon_signal_done();
    daemon_pid_file_remove();
#ifdef CHARLCD
    backlight(lcd,0);
#endif
    daemon_log(LOG_INFO, "Exit");
    exit(0);
}
//------------------------------------------------------------------------------------------------------------
//------------------------------------------------------------------------------------------------------------
