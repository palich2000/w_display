//------------------------------------------------------------------------------------------------
//
// ODROID-C printing time Application.
//
// Defined port number is wiringPi port number.
//
// Compile : gcc -o <create excute file name> <source file name> -lwiringPi -lwiringPiDev -lpthread
// Run : sudo ./<created excute file name>
//
//-------------------------------------------------------------------------------------------------
 
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
 
#include <unistd.h>
#include <string.h>
#include <time.h>
 
#include <wiringPi.h>
#include <wiringPiI2C.h>
#include <wiringSerial.h>
#include <lcd.h>
 
#define LCD_ROW             2   // 16 Char
#define LCD_COL             16  // 2 Line
#define LCD_BUS             4   // Interface 4 Bit mode
#define LCD_UPDATE_PERIOD   1000 // 300ms
 
static unsigned char lcdFb[LCD_ROW][LCD_COL] = {0, };
 
static int lcdHandle  = 0;
 
#define PORT_LCD_RS     7   // GPIOY.BIT3(#83)
#define PORT_LCD_E      0   // GPIOY.BIT8(#88)
#define PORT_LCD_D4     2   // GPIOX.BIT19(#116)
#define PORT_LCD_D5     3   // GPIOX.BIT18(#115)
#define PORT_LCD_D6     1   // GPIOY.BIT7(#87)
#define PORT_LCD_D7     4   // GPIOX.BIT7(#104)
 
static int ledPos = 0;
 
static void lcd_update (void)
{
	int i, j;
	time_t t;
	time(&t);
 
	memset((void *)&lcdFb, ' ', sizeof(lcdFb));
 
	sprintf(lcdFb[0], "Time %s", ctime(&t));
	lcdFb[0][strlen(lcdFb[0])-1] = ' ';
	lcdFb[0][strlen(lcdFb[0])] = ' ';
 
	for(i = 0; i < LCD_ROW; i++) {
		lcdPosition (lcdHandle, 0, i);
		for(j = 0; j < LCD_COL; j++)
			lcdPutchar(lcdHandle, lcdFb[i][j]);
	}
}
 
int system_init(void)
{
	int i, j;
 
	// LCD Init   
	lcdHandle = lcdInit(LCD_ROW, LCD_COL, LCD_BUS,
				PORT_LCD_RS, PORT_LCD_E,
				PORT_LCD_D4, PORT_LCD_D5,
				PORT_LCD_D6, PORT_LCD_D7, 0, 0, 0, 0);
 
	if(lcdHandle < 0) {
		fprintf(stderr, "%s : lcdInit failed!\n", __func__);
		return -1;
	}
	return  0;
}
 
int main (int argc, char *argv[])
{
	int timer = 0;
 
	wiringPiSetup();
 
	if (system_init() < 0) {
		fprintf (stderr, "%s: System Init failed\n", __func__);
		return -1;
	}
 
	for(;;) {
                usleep(100000);
		if (millis () < timer)
			continue;
		timer = millis () + LCD_UPDATE_PERIOD;
 
		// lcd update
		lcd_update();
	}
 
	return 0 ;
}
