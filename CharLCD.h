#ifndef _CHARLCD_H
#define _CHARLCD_H

#include <MCP23017.h>
#include <string.h>


// commands
#define LCD_CLEARDISPLAY 0x01
#define LCD_RETURNHOME 0x02
#define LCD_ENTRYMODESET 0x04
#define LCD_DISPLAYCONTROL 0x08
#define LCD_CURSORSHIFT 0x10
#define LCD_FUNCTIONSET 0x20
#define LCD_SETCGRAMADDR 0x40
#define LCD_SETDDRAMADDR 0x80

// flags for display entry mode
#define LCD_ENTRYRIGHT 0x00
#define LCD_ENTRYLEFT 0x02
#define LCD_ENTRYSHIFTINCREMENT 0x01
#define LCD_ENTRYSHIFTDECREMENT 0x00

// flags for display on/off control
#define LCD_DISPLAYON 0x04
#define LCD_DISPLAYOFF 0x00
#define LCD_CURSORON 0x02
#define LCD_CURSOROFF 0x00
#define LCD_BLINKON 0x01
#define LCD_BLINKOFF 0x00

// flags for display/cursor shift
#define LCD_DISPLAYMOVE 0x08
#define LCD_CURSORMOVE 0x00
#define LCD_MOVERIGHT 0x04
#define LCD_MOVELEFT 0x00

// flags for function set
#define LCD_8BITMODE 0x10
#define LCD_4BITMODE 0x00
#define LCD_2LINE 0x08
#define LCD_1LINE 0x00
#define LCD_5x10DOTS 0x04
#define LCD_5x8DOTS 0x00

#define BUTTON_UP 0x08
#define BUTTON_DOWN 0x04
#define BUTTON_LEFT 0x10
#define BUTTON_RIGHT 0x02
#define BUTTON_SELECT 0x01

// These #defines make it easy to set the backlight color
#define BLACK 0x0
#define RED 0x1
#define YELLOW 0x3
#define GREEN 0x2
#define TEAL 0x6
#define BLUE 0x4
#define VIOLET 0x5
#define WHITE 0x7

typedef struct CharLCD_t {
    uint8_t _rs_pin; // LOW: command.  HIGH: character.
    uint8_t _rw_pin; // LOW: write to LCD.  HIGH: read from LCD.
    uint8_t _enable_pin; // activated by a HIGH pulse.
    uint8_t _data_pins[8];
    uint8_t _button_pins[5];
    uint8_t _displayfunction;
    uint8_t _displaycontrol;
    uint8_t _displaymode;

    uint8_t _initialized;

    uint8_t _numlines, _currline, _numcols, _currcol;

    bool _left_to_right;

    uint8_t _i2cAddr;
    MCP23017_t * _i2c;
} CharLCD_t;


CharLCD_t * CharLCD_new(int bus, int address);
void CharLCD_destroy(CharLCD_t ** disp);

void CharLCD_start(CharLCD_t * disp, uint8_t cols, uint8_t rows);

void CharLCD_clearDisplay(CharLCD_t * disp);
void CharLCD_home(CharLCD_t * disp);

void CharLCD_setCursor(CharLCD_t * disp, uint8_t col, uint8_t row);

void CharLCD_noDisplay(CharLCD_t * disp);
void CharLCD_display(CharLCD_t * disp);
void CharLCD_noBlink(CharLCD_t * disp);
void CharLCD_blink(CharLCD_t * disp);
void CharLCD_noCursor(CharLCD_t * disp);
void CharLCD_cursor(CharLCD_t * disp);
void CharLCD_scrollDisplayLeft(CharLCD_t * disp);
void CharLCD_scrollDisplayRight(CharLCD_t * disp);
void CharLCD_leftToRight(CharLCD_t * disp);
void CharLCD_rightToLeft(CharLCD_t * disp);
void CharLCD_autoscroll(CharLCD_t * disp);
void CharLCD_noAutoscroll(CharLCD_t * disp);

void CharLCD_createChar(CharLCD_t * disp, uint8_t location, uint8_t charmap[]);

void CharLCD_command(CharLCD_t * disp, uint8_t value);
void CharLCD_write(CharLCD_t * disp, uint8_t value);

// only if using backpack
void CharLCD_setBacklight(CharLCD_t * disp, uint8_t status);

uint8_t CharLCD_readButtons(CharLCD_t * disp);

void CharLCD_print(CharLCD_t * disp, char * text);

#endif
