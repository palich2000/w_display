CC = gcc

CFLAGS = -g -std=c11 -MD -MP  -Wall -Wfatal-errors

OBJGROUP = lcd_cpuinfo.o nxjson.o array.o dlog.o dpid.o dfork.o dexec.o dsignal.o dzip.o dmem.o dnonblock.o version.o

EXTRA_LIBS = -lwiringPi -lwiringPiDev -lpthread -lm -lcrypt -lrt -lmosquitto -lzip

all: lcd_cpuinfo

.c.o:
	$(CC) $(CFLAGS) $(INCLUDES) -c $<  -o $@

lcd_cpuinfo: $(OBJGROUP)
	$(CC) -o lcd_cpuinfo $(OBJGROUP) $(EXTRA_LIBS) -lm

DEPS = $(SRCS:%.c=%.d)


-include $(DEPS)

clean:
	rm -f *.o *.d lcd_cpuinfo

install: lcd_cpuinfo
	install -D -o root -g root ./lcd_cpuinfo /usr/local/bin
