#define _GNU_SOURCE
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <stdint.h>
#include "faraday_serial.h"
#include "dlog.h"

static int
set_interface_attribs (int fd, int speed, int parity)
{
        struct termios tty;
        if (tcgetattr (fd, &tty) != 0)
        {
                daemon_log(LOG_ERR,"error %d from tcgetattr", errno);
                return -1;
        }

        cfsetospeed (&tty, speed);
        cfsetispeed (&tty, speed);

        tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8;     // 8-bit chars
        // disable IGNBRK for mismatched speed tests; otherwise receive break
        // as \000 chars
        tty.c_iflag &= ~IGNBRK;         // disable break processing
        tty.c_lflag = 0;                // no signaling chars, no echo,
                                        // no canonical processing
        tty.c_oflag = 0;                // no remapping, no delays
        tty.c_cc[VMIN]  = 0;            // read doesn't block
        tty.c_cc[VTIME] = 5;            // 0.5 seconds read timeout

        tty.c_iflag &= ~(IXON | IXOFF | IXANY); // shut off xon/xoff ctrl

        tty.c_cflag |= (CLOCAL | CREAD);// ignore modem controls,
                                        // enable reading
        tty.c_cflag &= ~(PARENB | PARODD);      // shut off parity
        tty.c_cflag |= parity;
        tty.c_cflag &= ~CSTOPB;
        tty.c_cflag &= ~CRTSCTS;

        if (tcsetattr (fd, TCSANOW, &tty) != 0)
        {
                daemon_log(LOG_ERR,"error %d from tcsetattr", errno);
                return -1;
        }
        return 0;
}

static void
set_blocking (int fd, int should_block)
{
        struct termios tty;
        memset (&tty, 0, sizeof tty);
        if (tcgetattr (fd, &tty) != 0)
        {
                daemon_log(LOG_ERR,"error %d from tggetattr", errno);
                return;
        }

        tty.c_cc[VMIN]  = should_block ? 1 : 0;
        tty.c_cc[VTIME] = 5;            // 0.5 seconds read timeout

        if (tcsetattr (fd, TCSANOW, &tty) != 0)
                daemon_log(LOG_ERR,"error %d setting term attributes", errno);
}

typedef struct f_cmd13_t  {
    uint8_t c[13];
} f_cmd13_t;


typedef struct f_cmd17_t  {
    uint8_t c[17];
} f_cmd17_t;

static
const faraday_reply_t* write_command_and_read_reply(int fd, uint8_t * cmd, size_t cmd_size) {

    static faraday_reply_t reply = {0};
    memset(&reply,0, sizeof(reply));

    uint8_t crc = 0;

    for (size_t i=0; i< cmd_size-1; i++) {
        crc += cmd[i];
    }

    cmd[cmd_size-1]=crc;

    write (fd, cmd, cmd_size);

    usleep ((cmd_size + 30 + sizeof(reply)) * 100);

    uint8_t echo[cmd_size];
    memset(echo,0,sizeof(echo));

    int n = read(fd, echo, sizeof(echo));

    if (n != (int)sizeof(echo)) {
	daemon_log(LOG_ERR, "echo n=%d size=%d\n", n, sizeof(echo));
	return NULL;
    }

    uint8_t read_buffer[255]={0};

    n = read(fd, &read_buffer, sizeof(read_buffer));
    if (n <= 0 ) {
	daemon_log(LOG_ERR, "reply n=%d size=%d\n", n, sizeof(reply));
	return NULL;
    }
    memmove(&reply, &read_buffer, sizeof(reply));
    return &reply;
}


const faraday_reply_t * read_faraday_data(const char * portname, faraday_psu_type_t psu_type) {

    if (!portname) {
	daemon_log(LOG_ERR,"portname is NULL");
        return NULL;
    }

    int fd = open (portname, O_RDWR | O_NOCTTY | O_SYNC);
    if (fd < 0)
    {
	daemon_log(LOG_ERR,"error %d opening %s: %s", errno, portname, strerror (errno));
        return NULL;
    }

    set_interface_attribs (fd, B9600, 0);  // set speed to 115,200 bps, 8n1 (no parity)
    set_blocking (fd, 0);                // set no blocking



//    f_cmd13_t cmd_buffer_acc_off = {.c={0x55, 0x07, 0x00, 0x00, 0x00, 0x00,
//		                 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x56}};

//    f_cmd13_t cmd_buffer_acc_on = {.c={0x55, 0x07, 0x00, 0x00, 0x00, 0x01,
//                                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x56}};

    const faraday_reply_t * reply_buffer = NULL;
    switch (psu_type) {
	case ft_normal: {
	    f_cmd13_t cmd_buffer = {.c={0x55, 0x01, 0x00, 0x00, 0x00, 0x00,
                                        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x56}};
	    reply_buffer = write_command_and_read_reply(fd, cmd_buffer.c, sizeof(cmd_buffer.c));
        } break;
	case ft_fire: {
	    f_cmd17_t cmd_buffer = {.c={0x55, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x56}};
	    reply_buffer = write_command_and_read_reply(fd, cmd_buffer.c, sizeof(cmd_buffer.c));
        } break;
        default:
         return NULL;
    }

    if (reply_buffer) {

	//char * p = (char*)reply_buffer;
        //for (size_t i = 0; i<sizeof(*reply_buffer); i++) {
	//    printf("%02x ",p[i]);
	//}
	//printf("\n");


	daemon_log(LOG_INFO, "batt:%.2f V %.2f A pws: %.2f A charge: %d a charge: %d ac220: %d",
	    reply_buffer->voltage_batt/10., reply_buffer->current_batt/10.,
	    reply_buffer->current_out/10., reply_buffer->charge, reply_buffer->assim_charge, reply_buffer->ac220);
    }

    close(fd);

    return(reply_buffer);
}