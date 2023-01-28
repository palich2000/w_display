#ifndef FARADAY_SERIAL_H_INCLUDED
#define FARADAY_SERIAL_H_INCLUDED
typedef struct faraday_reply_t {
    uint8_t x55;
    uint8_t cmd;
    uint16_t voltage_batt;
    uint8_t current_batt;
    uint8_t current_out;
    uint8_t ac220;
    uint8_t assim_charge;
    uint8_t charge;
    uint8_t ps_model;
    uint8_t ps_week_prod;
    uint8_t ps_year_prod;
    uint16_t ps_sn;
    uint8_t firmware_ver;
    uint8_t crc;
} faraday_reply_t;

typedef enum faraday_psu_type_t {
    ft_unknown = 0,
    ft_normal = 1,
    ft_fire = 2,
    ft_normal_asc_off = 3,
    ft_normal_asc_on = 4,
} faraday_psu_type_t;

const faraday_reply_t * read_faraday_data(const char * portname, faraday_psu_type_t psu_type);

#endif // FARADAY_SERIAL_H_INCLUDED