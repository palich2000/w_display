#include <CharLCD.h>
#include <stdio.h>
#include <inttypes.h>
#include <unistd.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>


// When the display powers up, it is configured as follows:
//
// 1. Display clear
// 2. Function set:
//    DL = 1; 8-bit interface data
//    N = 0; 1-line display
//    F = 0; 5x8 dot character font
// 3. Display on/off control:
//    D = 0; Display off
//    C = 0; Cursor off
//    B = 0; Blinking off
// 4. Entry mode set:
//    I/D = 1; Increment by 1
//    S = 0; No shift
//
// Note, however, that resetting the Arduino doesn't reset the LCD, so we
// can't assume that its in that state when a sketch starts (and the
// RGBLCDShield constructor is called).

static void CharLCD_write4bits(CharLCD_t * disp, uint8_t value);
static void CharLCD_send(CharLCD_t * disp, uint8_t value, uint8_t mode);
static void CharLCD_write8bits(CharLCD_t * disp, uint8_t value);
static void CharLCD_pulseEnable(CharLCD_t * disp);

CharLCD_t * CharLCD_new(int bus, int address)
{
    printf("bus: %d addr:%x\n", bus, address);

    CharLCD_t * disp = calloc(1, sizeof(*disp));
    if (!disp) return NULL;


   disp->_i2c = MCP23017_new(bus, address);

   disp->_displayfunction = LCD_4BITMODE | LCD_2LINE | LCD_5x8DOTS;

    // the I/O expander pinout
    disp->_rs_pin = 15;
    disp->_rw_pin = 14;
    disp->_enable_pin = 13;
    disp->_data_pins[0] = 12;  // really d4
    disp->_data_pins[1] = 11;  // really d5
    disp->_data_pins[2] = 10;  // really d6
    disp->_data_pins[3] = 9;  // really d7

    disp->_button_pins[0] = 0;
    disp->_button_pins[1] = 1;
    disp->_button_pins[2] = 2;
    disp->_button_pins[3] = 3;
    disp->_button_pins[4] = 4;
    // we can't begin() yet :(

    return disp;
}

void CharLCD_destroy(CharLCD_t ** disp) {
    if (!disp || !*disp) return;
    CharLCD_noBlink(*disp);
    CharLCD_noDisplay(*disp);
    CharLCD_setBacklight(*disp,BLACK);
    MCP23017_destroy(&((*disp)->_i2c));
    free(*disp);
    *disp = NULL;
}

void CharLCD_start(CharLCD_t * disp, uint8_t cols, uint8_t lines) {
    if (!disp) return;

    if (!MCP23017_openI2C(disp->_i2c)) {
        printf("MCP23017 init FAIL !\n");
        return;
    } else {
        printf("MCP23017 init OK !\n");
    }


    MCP23017_pinMode(disp->_i2c, 8, MCP23017_OUTPUT);
    MCP23017_pinMode(disp->_i2c, 6, MCP23017_OUTPUT);
    MCP23017_pinMode(disp->_i2c, 7, MCP23017_OUTPUT);
    CharLCD_setBacklight(disp, 0x7);
    MCP23017_pinMode(disp->_i2c, disp->_rw_pin, MCP23017_OUTPUT);

    MCP23017_pinMode(disp->_i2c, disp->_rs_pin, MCP23017_OUTPUT);
    MCP23017_pinMode(disp->_i2c, disp->_enable_pin, MCP23017_OUTPUT);
    for (uint8_t i=0; i<4; i++)
      MCP23017_pinMode(disp->_i2c, disp->_data_pins[i], MCP23017_OUTPUT);

    for (uint8_t i=0; i<5; i++) {
      MCP23017_pinMode(disp->_i2c, disp->_button_pins[i], MCP23017_INPUT);
      MCP23017_pullUp(disp->_i2c, disp->_button_pins[i], HIGH);
    }



  disp->_numlines = lines;
  disp->_currline = 0;
  disp->_numcols = cols;
  disp->_currcol = 0;


  // SEE PAGE 45/46 FOR INITIALIZATION SPECIFICATION!
  // according to datasheet, we need at least 40ms after power rises above 2.7V
  // before sending commands. Arduino can turn on way befer 4.5V so we'll wait 50
  usleep(50000);
  // Now we pull both RS and R/W low to begin commands
  MCP23017_digitalWrite(disp->_i2c, disp->_rs_pin, LOW);
  MCP23017_digitalWrite(disp->_i2c, disp->_enable_pin, LOW);
  if (disp->_rw_pin != 255) {
    MCP23017_digitalWrite(disp->_i2c, disp->_rw_pin, LOW);


  }

  //put the LCD into 4 bit or 8 bit mode
  if (! (disp->_displayfunction & LCD_8BITMODE)) {
    // this is according to the hitachi HD44780 datasheet
    // figure 24, pg 46

    // we start in 8bit mode, try to set 4 bit mode
    CharLCD_write4bits(disp, 0x03);
    usleep(4500); // wait min 4.1ms

    // second try
    CharLCD_write4bits(disp, 0x03);
    usleep(4500); // wait min 4.1ms

    // third go!
    CharLCD_write4bits(disp, 0x03);
    usleep(150);

    // finally, set to 8-bit interface
    CharLCD_write4bits(disp, 0x02);
  } else {
    // this is according to the hitachi HD44780 datasheet
    // page 45 figure 23

    // Send function set command sequence
    CharLCD_command(disp, LCD_FUNCTIONSET | disp->_displayfunction);
    usleep(4500);  // wait more than 4.1ms

    // second try
    CharLCD_command(disp, LCD_FUNCTIONSET | disp->_displayfunction);
    usleep(150);

    // third go
    CharLCD_command(disp, LCD_FUNCTIONSET | disp->_displayfunction);
  }

  // finally, set # lines, font size, etc.
  CharLCD_command(disp, LCD_FUNCTIONSET | disp->_displayfunction);

  // turn the display on with no cursor or blinking default
  disp->_displaycontrol = LCD_DISPLAYON | LCD_CURSOROFF | LCD_BLINKOFF;
  CharLCD_display(disp);

  // clear it off
  CharLCD_clearDisplay(disp);

  // Initialize to default text direction (for romance languages)
  disp->_displaymode = LCD_ENTRYLEFT | LCD_ENTRYSHIFTDECREMENT;
  // set the entry mode
  CharLCD_command(disp, LCD_ENTRYMODESET | disp->_displaymode);

  // Set text entry to left-to-right (default)
  CharLCD_leftToRight(disp);

  CharLCD_setCursor(disp, 0, 0);

}


/********** high level commands, for the user! */
void CharLCD_clearDisplay(CharLCD_t * disp) {
    if (!disp) return;

    CharLCD_command(disp, LCD_CLEARDISPLAY);  // clear display, set cursor position to zero
    usleep(2000);  // this command takes a long time!
}

void CharLCD_home(CharLCD_t * disp) {
    if (!disp) return;

    CharLCD_command(disp, LCD_RETURNHOME);  // set cursor position to zero
    disp->_currline = 0;
    disp->_currcol = 0;
    usleep(2000);  // this command takes a long time!
}

void CharLCD_setCursor(CharLCD_t * disp, uint8_t col, uint8_t row)
{
    if (!disp) return;
    int row_offsets[] = { 0x00, 0x40, 0x14, 0x54 };
    if ( row > disp->_numlines ) {
        row = disp->_numlines-1;    // we count rows starting w/0
    }

    disp->_currline = row;
    disp->_currcol = col;

    CharLCD_command(disp, LCD_SETDDRAMADDR | (col + row_offsets[row]));
}

// Turn the display on/off (quickly)
void CharLCD_noDisplay(CharLCD_t * disp) {
    if (!disp) return;
    disp->_displaycontrol &= ~LCD_DISPLAYON;
    CharLCD_command(disp, LCD_DISPLAYCONTROL | disp->_displaycontrol);
}
void CharLCD_display(CharLCD_t * disp) {
    if (!disp) return;
    disp->_displaycontrol |= LCD_DISPLAYON;
    CharLCD_command(disp, LCD_DISPLAYCONTROL | disp->_displaycontrol);
}

// Turns the underline cursor on/off
void CharLCD_noCursor(CharLCD_t * disp) {
    if (!disp) return;
    disp->_displaycontrol &= ~LCD_CURSORON;
    CharLCD_command(disp, LCD_DISPLAYCONTROL | disp->_displaycontrol);
}

void CharLCD_cursor(CharLCD_t * disp) {
    if (!disp) return;
  disp->_displaycontrol |= LCD_CURSORON;
  CharLCD_command(disp, LCD_DISPLAYCONTROL | disp->_displaycontrol);
}

// Turn on and off the blinking cursor
void CharLCD_noBlink(CharLCD_t * disp) {
    if (!disp) return;
    disp->_displaycontrol &= ~LCD_BLINKON;
    CharLCD_command(disp, LCD_DISPLAYCONTROL | disp->_displaycontrol);
}
void CharLCD_blink(CharLCD_t * disp) {
    if (!disp) return;
    disp->_displaycontrol |= LCD_BLINKON;
    CharLCD_command(disp, LCD_DISPLAYCONTROL | disp->_displaycontrol);
}

// These commands scroll the display without changing the RAM
void CharLCD_scrollDisplayLeft(CharLCD_t * disp) {
    if (!disp) return;
    CharLCD_command(disp, LCD_CURSORSHIFT | LCD_DISPLAYMOVE | LCD_MOVELEFT);
}
void CharLCD_scrollDisplayRight(CharLCD_t * disp) {
    if (!disp) return;
    CharLCD_command(disp, LCD_CURSORSHIFT | LCD_DISPLAYMOVE | LCD_MOVERIGHT);
}

// This is for text that flows Left to Right
void CharLCD_leftToRight(CharLCD_t * disp) {
    if (!disp) return;
    disp->_displaymode |= LCD_ENTRYLEFT;
    CharLCD_command(disp, LCD_ENTRYMODESET | disp->_displaymode);
    disp->_left_to_right = true;
}

// This is for text that flows Right to Left
void CharLCD_rightToLeft(CharLCD_t * disp) {
    if (!disp) return;
    disp->_displaymode &= ~LCD_ENTRYLEFT;
    CharLCD_command(disp, LCD_ENTRYMODESET | disp->_displaymode);
    disp->_left_to_right = false;
}

// This will 'right justify' text from the cursor
void CharLCD_autoscroll(CharLCD_t * disp) {
    if (!disp) return;
    disp->_displaymode |= LCD_ENTRYSHIFTINCREMENT;
    CharLCD_command(disp, LCD_ENTRYMODESET | disp->_displaymode);
}

// This will 'left justify' text from the cursor
void CharLCD_noAutoscroll(CharLCD_t * disp) {
    if (!disp) return;
    disp->_displaymode &= ~LCD_ENTRYSHIFTINCREMENT;
    CharLCD_command(disp, LCD_ENTRYMODESET | disp->_displaymode);
}

// Allows us to fill the first 8 CGRAM locations
// with custom characters
void CharLCD_createChar(CharLCD_t * disp, uint8_t location, uint8_t charmap[]) {
    if (!disp) return;
    location &= 0x7; // we only have 8 locations 0-7
    CharLCD_command(disp, LCD_SETCGRAMADDR | (location << 3));
    for (int i=0; i<8; i++) {
        CharLCD_write(disp, charmap[i]);
    }
    CharLCD_command(disp, LCD_SETDDRAMADDR);  // unfortunately resets the location to 0,0
}


// Print text to LCD screen

void CharLCD_print(CharLCD_t * disp, char * text) {
    if (!disp || !text) return;

    int line = disp->_currline;
    int col = disp->_currcol;

    char current_char;

    int len = strlen(text);
    //iterate through each character
    for (int i=0; i<len; i++) {
        current_char = text[i];

        if (current_char == '\n' ){
            line++;

            if (disp->_left_to_right) {
                col = 0;
            }
            else {
                col = disp->_numcols - 1;
            }
            CharLCD_setCursor(disp, col, line);

        }
        else if (col > (disp->_numcols-1) || col < 0){
            line++;

            if (disp->_left_to_right) {
                col = 0;
            }
            else {
                col = disp->_numcols - 1;
            }
            CharLCD_setCursor(disp, col, line);
            CharLCD_write(disp, current_char);
            if (disp->_left_to_right) {
                col++;
            }
            else {
                col--;
            }
        }
        else {
            CharLCD_write(disp, current_char);
            if (disp->_left_to_right) {
                col++;
            }
            else {
                col--;
            }
        }


    }


}

/*********** mid level commands, for sending data/cmds */

inline void CharLCD_command(CharLCD_t * disp, uint8_t value) {
    if (!disp ) return;
    CharLCD_send(disp, value, LOW);
}


inline void CharLCD_write(CharLCD_t * disp, uint8_t value) {
    if (!disp ) return;
    CharLCD_send(disp, value, HIGH);
}

/************ low level data pushing commands **********/


// Allows to set the backlight, if the LCD backpack is used
void CharLCD_setBacklight(CharLCD_t * disp, uint8_t status) {
  // check if i2c or SPI
  if (!disp ) return;
  MCP23017_digitalWrite(disp->_i2c, 8, ~(status >> 2) & 0x1);
  MCP23017_digitalWrite(disp->_i2c, 7, ~(status >> 1) & 0x1);
  MCP23017_digitalWrite(disp->_i2c, 6, ~status & 0x1);
}


// write either command or data, with automatic 4/8-bit selection
void CharLCD_send(CharLCD_t * disp, uint8_t value, uint8_t mode) {
    if (!disp ) return;
    MCP23017_digitalWrite(disp->_i2c, disp->_rs_pin, mode);

    // if there is a RW pin indicated, set it low to Write
    if (disp->_rw_pin != 255) {
        MCP23017_digitalWrite(disp->_i2c, disp->_rw_pin, LOW);
    }

    if (disp->_displayfunction & LCD_8BITMODE) {
        CharLCD_write8bits(disp, value);
    } else {
        CharLCD_write4bits(disp, value>>4);
        CharLCD_write4bits(disp, value);
    }
}

void CharLCD_pulseEnable(CharLCD_t * disp) {
    if (!disp ) return;
    MCP23017_digitalWrite(disp->_i2c, disp->_enable_pin, LOW);
    usleep(1);
    MCP23017_digitalWrite(disp->_i2c, disp->_enable_pin, HIGH);
    usleep(1);    // enable pulse must be >450ns
    MCP23017_digitalWrite(disp->_i2c, disp->_enable_pin, LOW);
    usleep(100);   // commands need > 37us to settle
}


void CharLCD_write4bits(CharLCD_t * disp, uint8_t value) {
    if (!disp ) return;
    for (int i = 0; i < 4; i++) {
     MCP23017_pinMode(disp->_i2c, disp->_data_pins[i], MCP23017_OUTPUT);
     MCP23017_digitalWrite(disp->_i2c, disp->_data_pins[i], (value >> i) & 0x01);
    }
    CharLCD_pulseEnable(disp);
}


void CharLCD_write8bits(CharLCD_t * disp, uint8_t value) {
    if (!disp ) return;
    for (int i = 0; i < 8; i++) {
        MCP23017_pinMode(disp->_i2c, disp->_data_pins[i], MCP23017_OUTPUT);
        MCP23017_digitalWrite(disp->_i2c, disp->_data_pins[i], (value >> i) & 0x01);
    }

    CharLCD_pulseEnable(disp);
}

uint8_t CharLCD_readButtons(CharLCD_t * disp) {
    uint8_t reply = 0x1F;
    if (!disp ) return reply;

    for (uint8_t i=0; i<5; i++) {
        reply &= ~((MCP23017_digitalRead(disp->_i2c, disp->_button_pins[i])) << i);
    }
    return reply;
}
