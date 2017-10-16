#!/usr/bin/python

import wiringpi2
import subprocess
import time
# --LCD
LCD_ROW = 2 # 16 Char
LCD_COL = 16 # 2 Line
LCD_BUS = 4 # Interface 4 Bit mode

PORT_LCD_RS = 7 # GPIOY.BIT3(#83)
PORT_LCD_E = 0 # GPIOY.BIT8(#88)
PORT_LCD_D4 = 2 # GPIOX.BIT19(#116)
PORT_LCD_D5 = 3 # GPIOX.BIT18(#115)
PORT_LCD_D6 = 1 # GPIOY.BIT7(#87)
PORT_LCD_D7 = 4 # GPIOX.BIT4(#104)
# --LCD

wiringpi2.wiringPiSetup()
# --LCD
lcdHandle = wiringpi2.lcdInit(LCD_ROW, LCD_COL, LCD_BUS,
PORT_LCD_RS, PORT_LCD_E,
PORT_LCD_D4, PORT_LCD_D5,
PORT_LCD_D6, PORT_LCD_D7, 0, 0, 0, 0);
lcdRow = 0 # LCD Row
lcdCol = 0 # LCD Column
# --LCD

def run_cmd(cmd):
        # runs whatever is in the cmd variable in the terminal
        p = subprocess.Popen(cmd, shell=True, stdout=subprocess.PIPE)
        output = p.communicate()[0]
        return output

cmd = "ifconfig eth0 | grep 'inet addr' | cut -d ':' -f2 | cut -d ' ' -f1"
ipaddr = run_cmd(cmd)[:-1] # set output of command into ipaddr variable

cmd_hostname = "hostname"
hostname = run_cmd(cmd_hostname)[:-1]

wiringpi2.lcdClear(lcdHandle)

for x in range(0, 7):
    wiringpi2.lcdClear(lcdHandle)
    wiringpi2.lcdPosition(lcdHandle, lcdCol, lcdRow)
    wiringpi2.lcdPrintf(lcdHandle, str(x))
    wiringpi2.lcdPosition(lcdHandle, lcdCol, lcdRow + 1)
    wiringpi2.lcdPrintf(lcdHandle, ipaddr)
    time.sleep(3)