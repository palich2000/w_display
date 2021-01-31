#include <MCP23017.h>
#include <string.h>

static uint8_t bitForPin(uint8_t pin);
static uint8_t regForPin(uint8_t pin, uint8_t portAaddr, uint8_t portBaddr);
static uint8_t readRegister(MCP23017_t * dev, uint8_t addr);
static uint8_t writeRegister(MCP23017_t * dev, uint8_t addr, uint8_t value);
static uint8_t readByte(MCP23017_t * dev);
static uint8_t writeByte(MCP23017_t * dev, uint8_t value);
static void updateRegisterBit(MCP23017_t * dev, uint8_t p, uint8_t pValue, uint8_t portAaddr, uint8_t portBaddr);


MCP23017_t * MCP23017_new(int bus, int address) {
    MCP23017_t * dev =  calloc(1, sizeof(*dev));
    dev->kI2CBus = bus;           // I2C bus of Jetson (1 and 8 available on Xavier)
    dev->kI2CAddress = address ; // Address of MCP23017; defaults to 0x20
    dev->error = 0 ;
    printf("MCP23017 bus: %d addr:%x\n", bus, address);
    return dev;
}

void MCP23017_destroy(MCP23017_t ** dev) {
    if (!dev || !*dev) return;
    MCP23017_closeI2C(*dev) ;
    free(*dev);
    *dev = NULL;
}

static bool bitRead(uint8_t num, uint8_t index)
{
    return (num >> index) & 1;
}


static void bitWrite(uint8_t *var, uint8_t index, uint8_t bit)
{
    uint new_bit = 1 << index;
    if(bit)
    {
        *var = (*var) | new_bit;
    }
    else {
        new_bit = ~new_bit;
        *var = (*var) & new_bit;
    }
}


bool MCP23017_openI2C(MCP23017_t * dev)
{
    if (!dev) return false;

    char fileNameBuffer[32];
    sprintf(fileNameBuffer,"/dev/i2c-%d", dev->kI2CBus);
    dev->kI2CFileDescriptor = open(fileNameBuffer, O_RDWR);
    if (dev->kI2CFileDescriptor < 0) {
        // Could not open the file
       dev->error = errno ;
       printf("1************ %d %s %s\n", errno, strerror(errno), fileNameBuffer);
       return false ;
    }
    if (ioctl(dev->kI2CFileDescriptor, I2C_SLAVE, dev->kI2CAddress) < 0) {
        // Could not open the device on the bus
        dev->error = errno ;
        printf("2*********** %d %s\n", errno, strerror(errno));
        return false ;
    }
    // set defaults!
	// all inputs on port A and B
	writeRegister(dev,MCP23017_IODIRA,0b11111110);
	writeRegister(dev,MCP23017_IODIRB,0b11111110);
    return true ;
}

//close I2C communication
void MCP23017_closeI2C(MCP23017_t * dev)
{
    if (!dev) return;
    if (dev->kI2CFileDescriptor >= 0) {
        close(dev->kI2CFileDescriptor);
        dev->kI2CFileDescriptor = -1 ;
    }
}


static uint8_t bitForPin(uint8_t pin){
	return pin%8;
}

static uint8_t regForPin(uint8_t pin, uint8_t portAaddr, uint8_t portBaddr){
	return(pin<8) ?portAaddr:portBaddr;
}

static uint8_t readRegister(MCP23017_t * dev, uint8_t addr)
{
    if (!dev) return -1;
    int toReturn = i2c_smbus_read_byte_data(dev->kI2CFileDescriptor, addr);
    if (toReturn < 0) {
        printf("MCP23017 Read Byte error: %d",errno) ;
        dev->error = errno ;
        toReturn = -1 ;
    }
    // For debugging
    // printf("Device 0x%02X returned 0x%02X from register 0x%02X\n", kI2CAddress, toReturn, readRegister);
    return toReturn ;
}

/**
 * Writes a given register
 */
static uint8_t writeRegister(MCP23017_t * dev,uint8_t addr, uint8_t writeValue)
{   // For debugging:
    // printf("Wrote: 0x%02X to register 0x%02X \n",writeValue, writeRegister) ;
    if (!dev) return -1;
    int toReturn = i2c_smbus_write_byte_data(dev->kI2CFileDescriptor, addr, writeValue);
    if (toReturn < 0) {
        perror("Write to I2C Device failed");
        dev->error = errno ;
        toReturn = -1 ;
    }
    return toReturn ;

}

/**
 * Reads a byte
 */
static uint8_t readByte(MCP23017_t * dev)
{
    if (!dev) return -1;
    int toReturn = i2c_smbus_read_byte(dev->kI2CFileDescriptor);
    if (toReturn < 0) {
        printf("MCP23017 Read Byte error: %d",errno) ;
        dev->error = errno ;
        toReturn = -1 ;
    }
    // For debugging
    // printf("Device 0x%02X returned 0x%02X from register 0x%02X\n", kI2CAddress, toReturn, readRegister);
    return toReturn ;
}

/**
 * Writes a byte
 */
static uint8_t writeByte(MCP23017_t * dev, uint8_t writeValue)
{   // For debugging:
    // printf("Wrote: 0x%02X to register 0x%02X \n",writeValue, writeRegister) ;
    if (!dev) return -1;
    int toReturn = i2c_smbus_write_byte(dev->kI2CFileDescriptor, writeValue);
    if (toReturn < 0) {
        printf("MCP23017 Write Byte error: %d",errno) ;
        dev->error = errno ;
        toReturn = -1 ;
    }
    return toReturn ;
}

/**
 * Helper to update a single bit of an A/B register.
 * - Reads the current register value
 * - Writes the new register value
 */
static void updateRegisterBit(MCP23017_t * dev, uint8_t pin, uint8_t pValue, uint8_t portAaddr, uint8_t portBaddr) {
    if (!dev) return;
	uint8_t regValue;
	uint8_t regAddr=regForPin(pin,portAaddr,portBaddr);
	uint8_t bit=bitForPin(pin);
	regValue = readRegister(dev, regAddr);

	// set the value for the particular bit
	bitWrite(&regValue,bit,pValue);

	writeRegister(dev, regAddr,regValue);
}

/**
 * Sets the pin mode to either INPUT or OUTPUT
 */
void MCP23017_pinMode(MCP23017_t * dev, uint8_t p, uint8_t d) {
    if (!dev) return;
	updateRegisterBit(dev, p,(d==MCP23017_INPUT),MCP23017_IODIRA,MCP23017_IODIRB);
}

/**
 * Reads all 16 pins (port A and B) into a single 16 bits variable.
 */
uint16_t MCP23017_readGPIOAB(MCP23017_t * dev) {
    if (!dev) return -1;

	uint16_t ba = 0;
	uint8_t a;

	// read the current GPIO output latches
	writeByte(dev, MCP23017_GPIOA);

	a = readByte(dev);
	ba = readByte(dev);
	ba <<= 8;
	ba |= a;

	return ba;
}

/**
 * Read a single port, A or B, and return its current 8 bit value.
 * Parameter b should be 0 for GPIOA, and 1 for GPIOB.
 */
uint8_t MCP23017_readGPIO(MCP23017_t * dev, uint8_t b) {
    if (!dev) return -1;
	// read the current GPIO output latches
	if (b == 0)
		writeByte(dev, MCP23017_GPIOA);
	else {
		writeByte(dev, MCP23017_GPIOB);
	}


	uint8_t value = readByte(dev);
	return value;
}

/**
 * Writes all the pins in one go. This method is very useful if you are implementing a multiplexed matrix and want to get a decent refresh rate.
 */
void MCP23017_writeGPIOAB(MCP23017_t * dev, uint16_t ba) {
    if (!dev) return;
	writeByte(dev, MCP23017_GPIOA);
	writeByte(dev, ba & 0xFF);
	writeByte(dev, ba >> 8);

}

void MCP23017_digitalWrite(MCP23017_t * dev, uint8_t pin, uint8_t d) {
    if (!dev) return;

	uint8_t bit=bitForPin(pin);


	// read the current GPIO output latches
	uint8_t regAddr=regForPin(pin,MCP23017_OLATA,MCP23017_OLATB);
	uint8_t gpio = readRegister(dev, regAddr);

	// set the pin and direction
	bitWrite(&gpio,bit,d);

	// write the new GPIO
	regAddr=regForPin(pin,MCP23017_GPIOA,MCP23017_GPIOB);
	writeRegister(dev, regAddr,gpio);
}

void MCP23017_pullUp(MCP23017_t * dev, uint8_t p, uint8_t d) {
    if (!dev) return;
	updateRegisterBit(dev, p,d,MCP23017_GPPUA,MCP23017_GPPUB);
}

bool MCP23017_digitalRead(MCP23017_t * dev, uint8_t pin) {
    if (!dev) return false;
	uint8_t bit=bitForPin(pin);
	uint8_t regAddr=regForPin(pin,MCP23017_GPIOA,MCP23017_GPIOB);
	return (readRegister(dev, regAddr) >> bit) & 0x1;
}

/**
 * Configures the interrupt system. both port A and B are assigned the same configuration.
 * Mirroring will OR both INTA and INTB pins.
 * Opendrain will set the INT pin to value or open drain.
 * polarity will set LOW or HIGH on interrupt.
 * Default values after Power On Reset are: (false, false, LOW)
 * If you are connecting the INTA/B pin to arduino 2/3, you should configure the interupt handling as FALLING with
 * the default configuration.
 */
void MCP23017_setupInterrupts(MCP23017_t * dev, uint8_t mirroring, uint8_t openDrain, uint8_t polarity){
    if (!dev) return;
	// configure the port A
	uint8_t ioconfValue=readRegister(dev, MCP23017_IOCONA);
	bitWrite(&ioconfValue,6,mirroring);
	bitWrite(&ioconfValue,2,openDrain);
	bitWrite(&ioconfValue,1,polarity);
	writeRegister(dev, MCP23017_IOCONA,ioconfValue);

	// Configure the port B
	ioconfValue=readRegister(dev, MCP23017_IOCONB);
	bitWrite(&ioconfValue,6,mirroring);
	bitWrite(&ioconfValue,2,openDrain);
	bitWrite(&ioconfValue,1,polarity);
	writeRegister(dev, MCP23017_IOCONB,ioconfValue);
}

/**
 * Set's up a pin for interrupt. uses arduino MODEs: CHANGE, FALLING, RISING.
 *
 * Note that the interrupt condition finishes when you read the information about the port / value
 * that caused the interrupt or you read the port itself. Check the datasheet can be confusing.
 *
 */
void MCP23017_setupInterruptPin(MCP23017_t * dev, uint8_t pin, uint8_t mode) {
    if (!dev) return;
	// set the pin interrupt control (0 means change, 1 means compare against given value);
	updateRegisterBit(dev, pin,(mode!=CHANGE),MCP23017_INTCONA,MCP23017_INTCONB);
	// if the mode is not CHANGE, we need to set up a default value, different value triggers interrupt

	// In a RISING interrupt the default value is 0, interrupt is triggered when the pin goes to 1.
	// In a FALLING interrupt the default value is 1, interrupt is triggered when pin goes to 0.
	updateRegisterBit(dev, pin,(mode==FALLING),MCP23017_DEFVALA,MCP23017_DEFVALB);

	// enable the pin for interrupt
	updateRegisterBit(dev, pin,HIGH,MCP23017_GPINTENA,MCP23017_GPINTENB);

}

uint8_t MCP23017_getLastInterruptPin(MCP23017_t * dev){
    if (!dev) return MCP23017_INT_ERR;;
	uint8_t intf;

	// try port A
	intf=readRegister(dev, MCP23017_INTFA);
	for(int i=0;i<8;i++) if (bitRead(intf,i)) return i;

	// try port B
	intf=readRegister(dev, MCP23017_INTFB);
	for(int i=0;i<8;i++) if (bitRead(intf,i)) return i+8;

	return MCP23017_INT_ERR;

}

uint8_t MCP23017_getLastInterruptPinValue(MCP23017_t * dev){
    if (!dev) return MCP23017_INT_ERR;;
	uint8_t intPin=MCP23017_getLastInterruptPin(dev);
	if(intPin!=MCP23017_INT_ERR){
		uint8_t intcapreg=regForPin(intPin,MCP23017_INTCAPA,MCP23017_INTCAPB);
		uint8_t bit=bitForPin(intPin);
		return (readRegister(dev, intcapreg)>>bit) & (0x01);
	}

	return MCP23017_INT_ERR;
}
