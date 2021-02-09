CC = gcc

CFLAGS = -g -std=c11 -MD -MP  -Wall -Werror -Wfatal-errors -Wextra  -g -I.

OBJGROUP = lcd_cpuinfo.o nxjson.o array.o dlog.o dpid.o dfork.o dexec.o dsignal.o dzip.o dmem.o dnonblock.o version.o CharLCD.o MCP23017.o ina219.o

EXTRA_LIBS = -li2c -lpthread -lm -lcrypt -lrt -lmosquitto -lzip

all: lcd_cpuinfo

.c.o:
	$(CC) $(CFLAGS) $(INCLUDES) -c $<  -o $@

lcd_cpuinfo: $(OBJGROUP)
	$(CC) -o lcd_cpuinfo $(OBJGROUP) $(EXTRA_LIBS) -lm

DEPS = $(SRCS:%.c=%.d)


-include $(DEPS)

clean:
	rm -f *.o *.d lcd_cpuinfo core

install: lcd_cpuinfo
	install -D -o root -g root ./lcd_cpuinfo /usr/local/bin

format:
	astyle -A2 -p -xg -k2 -W2 *.c *.h
check:
	cppcheck --enable=all --inconclusive --std=c11 --suppress=unusedFunction .

#######################
G_EX = $(shell git describe --tag > /dev/null ; if [ $$? -eq 0 ]; then echo "OK"; else echo "FAIL" ; fi)
GVER = $(shell git describe --abbrev=7 --long)
#######################

version.c: FORCE
	@echo "==============================================="
	@echo "git present:" $(G_EX) " ver:" $(GVER)
	@echo "==============================================="
ifeq "$(G_EX)" "OK"
	git describe --tag | awk 'BEGIN { FS="-" } {print "#include \"version.h\""} {print "const char * git_version = \"" $$1"."$$2"\";"} END {}' > version.c
	git rev-parse --abbrev-ref HEAD | awk '{print "const char * git_branch = \""$$0"\";"} {}' >> version.c
endif

FORCE:
