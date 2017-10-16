//------------------------------------------------------------------------------------------------------------
//
// ODROID-C 16x2 LCD / LED / Button Test Application.
//
// Defined port number is wiringPi port number.
//
// Compile : gcc -o <create excute file name> <source file name> -lwiringPi -lwiringPiDev -lpthread
// Run : sudo ./<created excute file name>
//
//------------------------------------------------------------------------------------------------------------
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
 
//------------------------------------------------------------------------------------------------------------
//
// Global handle Define
//
//------------------------------------------------------------------------------------------------------------
 
//------------------------------------------------------------------------------------------------------------
//
// LCD:
//
//------------------------------------------------------------------------------------------------------------
#define LCD_ROW             2   // 16 Char
#define LCD_COL             16  // 2 Line
#define LCD_BUS             4   // Interface 4 Bit mode
#define LCD_UPDATE_PERIOD   3000 // 300ms
 
static unsigned char lcdFb[LCD_ROW][LCD_COL] = {0};
 
static int lcdHandle  = 0;
static int lcdDispPos = 0;
 
#define PORT_LCD_RS     7
#define PORT_LCD_E      0
#define PORT_LCD_D4     2
#define PORT_LCD_D5     3
#define PORT_LCD_D6     1
#define PORT_LCD_D7     4
 
const unsigned char lcdDispString[LCD_ROW][LCD_COL] = {
    //1234567890123456
     " Hello! ODROID! ",
     " - HardKernel - "
};
//------------------------------------------------------------------------------------------------------------
//
// Button:
//
//------------------------------------------------------------------------------------------------------------
#define PORT_BUTTON1    5
#define PORT_BUTTON2    6
 
//------------------------------------------------------------------------------------------------------------
//
// LED:
//
//------------------------------------------------------------------------------------------------------------
static int ledPos = 0;
 
const int ledPorts[] = {
    21, 
    22,
    23,
    24,
    11,
    26,
    27,
};
 
#define MAX_LED_CNT sizeof(ledPorts) / sizeof(ledPorts[0])
 
//------------------------------------------------------------------------------------------------------------
//------------------------------------------------------------------------------------------------------------
//
// LCD Update Function:
//
//------------------------------------------------------------------------------------------------------------
static void lcd_update (void)
{
    int i, j;
 
    memset((void *)&lcdFb, ' ', sizeof(lcdFb));
 
    if(lcdDispPos)    lcdDispPos %= LCD_COL; 
    
    // LCD Display Shift

    if(lcdDispPos < 0)  {
        strncpy(&lcdFb[0][abs(lcdDispPos)], &lcdDispString[0][0], LCD_COL - abs(lcdDispPos));
        strncpy(&lcdFb[1][abs(lcdDispPos)], &lcdDispString[1][0], LCD_COL - abs(lcdDispPos));
    }
    else    {
        strncpy(&lcdFb[0][0], &lcdDispString[0][lcdDispPos], LCD_COL - lcdDispPos);
        strncpy(&lcdFb[1][0], &lcdDispString[1][lcdDispPos], LCD_COL - lcdDispPos);
    }
/*
    lcdPosition (lcdHandle, 0, 0);
    for(i = 0; i < LCD_ROW; i++)    {
         lcdPosition (lcdHandle, 0, i);
	printf("\n");
        for(j = 0; j < LCD_COL; j++)    {
	    lcdPutchar(lcdHandle, lcdFb[i][j]);
	    printf("%c",lcdFb[i][j]);
	}
    }
*/
    lcdClear(lcdHandle);
    lcdPosition (lcdHandle, 1, 0);
    lcdPuts(lcdHandle,"zhopa1");
    lcdPosition (lcdHandle, 1, 1);
    lcdPuts(lcdHandle,"zhopa2");
}
 
//------------------------------------------------------------------------------------------------------------
//
// system init
//
//------------------------------------------------------------------------------------------------------------
int system_init(void)
{
    int i;
 
    // LCD Init   
    lcdHandle = lcdInit (LCD_ROW, LCD_COL, LCD_BUS,
                         PORT_LCD_RS, PORT_LCD_E,
                         PORT_LCD_D4, PORT_LCD_D5, PORT_LCD_D6, PORT_LCD_D7, 0, 0, 0, 0);
 
    if(lcdHandle < 0)   {
        fprintf(stderr, "%s : lcdInit failed!\n", __func__);    return -1;
    }
 
    // GPIO Init(LED Port ALL Output)
    for(i = 0; i < MAX_LED_CNT; i++)    {
        pinMode (ledPorts[i], OUTPUT);  pullUpDnControl (PORT_BUTTON1, PUD_OFF);
    }
 
    // Button Pull Up Enable.
    pinMode (PORT_BUTTON1, INPUT);    pullUpDnControl (PORT_BUTTON1, PUD_UP);
    pinMode (PORT_BUTTON2, INPUT);    pullUpDnControl (PORT_BUTTON2, PUD_UP);
 
    return  0;
 }
 
//------------------------------------------------------------------------------------------------------------
//
// board data update
//
//------------------------------------------------------------------------------------------------------------
void boardDataUpdate(void)
{
    int i;
 
    //  LED Control
    for(i = 0; i < MAX_LED_CNT; i++)    digitalWrite (ledPorts[i], 0); // LED All Clear
 
    digitalWrite(ledPorts[ledPos++], 1);
 
    if(ledPos)  ledPos %= MAX_LED_CNT;
 
    // button status read
    if(!digitalRead (PORT_BUTTON1)) {
	lcdDispPos++;
	printf("B1\n");
    }
    if(!digitalRead (PORT_BUTTON2)) {
	printf("B2\n");
	lcdDispPos--;
    }
}
 
//------------------------------------------------------------------------------------------------------------
//
// Start Program
//
//------------------------------------------------------------------------------------------------------------
int main (int argc, char *argv[])
{
    int timer = 0 ;
 
    wiringPiSetup ();
 
    if (system_init() < 0)
    {
        fprintf (stderr, "%s: System Init failed\n", __func__);     return -1;
    }
    int i = 0; 
    for(;;)    {
        usleep(100000);
        if (millis () < timer)  continue ;
 
        timer = millis () + LCD_UPDATE_PERIOD;
 
        // All Data update
        //boardDataUpdate();
 
        // lcd update
        lcd_update();
	printf("%d\n",i);
	i++;
    }
 
    return 0 ;
}
 
//------------------------------------------------------------------------------------------------------------
//------------------------------------------------------------------------------------------------------------
