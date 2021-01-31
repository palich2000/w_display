#ifndef _MCP23017_H
#define _MCP23017_H

#define _GNU_SOURCE
#include <i2c/smbus.h>
#include <linux/i2c-dev.h>
#include <sys/ioctl.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct _MCP23017_t {
    unsigned char kI2CBus ;         // I2C bus of the MCP23017
    int kI2CFileDescriptor ;        // File Descriptor to the MCP23017
    int kI2CAddress ;               // Address of MCP23017; defaults to 0x20
    int error ;
    uint8_t i2caddr;

} MCP23017_t;

MCP23017_t * MCP23017_new(int bus, int address);
void MCP23017_destroy(MCP23017_t ** dev) ;
bool MCP23017_openI2C(MCP23017_t * dev) ;
void MCP23017_closeI2C(MCP23017_t * dev);


void MCP23017_pinMode(MCP23017_t * dev, uint8_t p, uint8_t d);
void MCP23017_digitalWrite(MCP23017_t * dev, uint8_t p, uint8_t d);
void MCP23017_pullUp(MCP23017_t * dev, uint8_t p, uint8_t d);
bool MCP23017_digitalRead(MCP23017_t * dev, uint8_t p);

void MCP23017_writeGPIOAB(MCP23017_t * dev, uint16_t);
uint16_t MCP23017_readGPIOAB(MCP23017_t * dev);
uint8_t MCP23017_readGPIO(MCP23017_t * dev, uint8_t b);

void MCP23017_setupInterrupts(MCP23017_t * dev, uint8_t mirroring, uint8_t open, uint8_t polarity);
void MCP23017_setupInterruptPin(MCP23017_t * dev, uint8_t p, uint8_t mode);
uint8_t MCP23017_getLastInterruptPin(MCP23017_t * dev);
uint8_t MCP23017_getLastInterruptPinValue(MCP23017_t * dev);

#define MCP23017_ADDRESS 0x20

// registers
#define MCP23017_IODIRA 0x00
#define MCP23017_IPOLA 0x02
#define MCP23017_GPINTENA 0x04
#define MCP23017_DEFVALA 0x06
#define MCP23017_INTCONA 0x08
#define MCP23017_IOCONA 0x0A
#define MCP23017_GPPUA 0x0C
#define MCP23017_INTFA 0x0E
#define MCP23017_INTCAPA 0x10
#define MCP23017_GPIOA 0x12
#define MCP23017_OLATA 0x14


#define MCP23017_IODIRB 0x01
#define MCP23017_IPOLB 0x03
#define MCP23017_GPINTENB 0x05
#define MCP23017_DEFVALB 0x07
#define MCP23017_INTCONB 0x09
#define MCP23017_IOCONB 0x0B
#define MCP23017_GPPUB 0x0D
#define MCP23017_INTFB 0x0F
#define MCP23017_INTCAPB 0x11
#define MCP23017_GPIOB 0x13
#define MCP23017_OLATB 0x15

#define MCP23017_INT_ERR 255

//constants set up to emulate Arduino pin parameters
#define HIGH 1
#define LOW 0

#define MCP23017_INPUT 1
#define MCP23017_OUTPUT 0

#define CHANGE 0
#define FALLING 1
#define RISING 2

#endif
