#include <SPI.h>

// ---------------------------------------------------------
// ESP32-S3 Pin Definitions
// ---------------------------------------------------------
const int IRQ_PIN  = 9;
const int CS_PIN   = 10;
const int MOSI_PIN = 11;
const int SCK_PIN  = 12;
const int MISO_PIN = 13;

// ---------------------------------------------------------
// MCP3561/2/4R Commands and Registers
// ---------------------------------------------------------
// The device address is hard-coded within the device and defaults to '01'.
const uint8_t DEV_ADDR = 0x01; 

// Command Type Bits
const uint8_t CMD_STAT_READ = 0x01; // Static Read 
const uint8_t CMD_INC_WRITE = 0x02; // Incremental Write 

// Register Map Addresses
const uint8_t REG_ADCDATA = 0x00; // ADC Channel Data Output Register
const uint8_t REG_CONFIG0 = 0x01; // Configuration Register 0
const uint8_t REG_CONFIG2 = 0x03; // Configuration Register 2
const uint8_t REG_MUX     = 0x06; // Multiplexer Register

uint8_t readADC = 0;

void ARDUINO_ISR_ATTR updateReadADC (){
  readADC = 1;
}


// ---------------------------------------------------------
// Helper: Construct Command Byte
// ---------------------------------------------------------
// The COMMAND byte is divided into three parts: the device address bits (CMD[7:6]), 
// the command address bits (CMD[5:2]) and the command-type bits (CMD[1:0])[cite: 1].
uint8_t makeCommand(uint8_t regAddr, uint8_t cmdType) {
    return (DEV_ADDR << 6) | (regAddr << 2) | cmdType;
}

// ---------------------------------------------------------
// Helper: Write a single register
// ---------------------------------------------------------
void writeRegister(uint8_t reg, uint8_t value) {
    uint8_t cmd = makeCommand(reg, CMD_INC_WRITE);
    digitalWrite(CS_PIN, LOW);
    SPI.transfer(cmd);
    SPI.transfer(value);
    digitalWrite(CS_PIN, HIGH);
}

// ---------------------------------------------------------
// Helper: Read 24-bit ADC Data
// ---------------------------------------------------------
int32_t readADCData() {
    uint8_t cmd = makeCommand(REG_ADCDATA, CMD_STAT_READ);
    digitalWrite(CS_PIN, LOW);
    SPI.transfer(cmd);

    // When DATA_FORMAT[1:0] = 00, the output register shows only the 24-bit value[cite: 1].
    uint8_t b1 = SPI.transfer(0x00);
    uint8_t b2 = SPI.transfer(0x00);
    uint8_t b3 = SPI.transfer(0x00);
    digitalWrite(CS_PIN, HIGH);

    // Sign extend the 24-bit (23-bit plus sign) value to a standard 32-bit integer[cite: 1].
    int32_t result = (b1 << 16) | (b2 << 8) | b3;
    if (result & 0x800000) {
        result |= 0xFF000000; 
    }
    return result;
}

void setup() {
    Serial.begin(115200);
    
    pinMode(IRQ_PIN, INPUT_PULLUP);
    pinMode(CS_PIN, OUTPUT);
    digitalWrite(CS_PIN, HIGH);

    // Initialize SPI on ESP32-S3 custom pins (SCK, MISO, MOSI, SS)
    SPI.begin(SCK_PIN, MISO_PIN, MOSI_PIN, CS_PIN);
    
    // The device interface is compatible with both SPI 0,0 and 1,1 modes up to 20 MHz[cite: 1].
    SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));

    delay(100); // Allow time for Power-on Reset (POR)
    attachInterrupt(IRQ_PIN, updateReadADC, FALLING);

    // 1. Configure the Multiplexer (MUX Register 0x06)
    // To measure a shunt resistor connected to CH0, we configure VIN+ as CH0 (0000) 
    // and VIN- as AGND (1000)[cite: 1]. (Change 0x08 to 0x01 if using differential CH0/CH1).
    writeRegister(REG_MUX, 0x08);

    // 2. Configure Gain to 64x (CONFIG2 Register 0x03)
    // Bit 7-6: BOOST = 10 (Current x 1)
    // Bit 5-3: GAIN  = 111 (Gain is x64)[cite: 1].
    // Bit 2: AZ_MUX  = 0
    // Bit 1: AZ_REF  = 1
    // Bit 0: RESERVED = 1
    // Value = 0b10111011 = 0xBB
    writeRegister(REG_CONFIG2, 0xBB);

    // 3. Start ADC Conversions (CONFIG0 Register 0x01)
    // Bit 7: VREF_SEL = 1 (Internal reference selected)[cite: 1].
    // Bit 6: CONFIG0[6] = 1 (Required for normal mode)
    // Bit 5-4: CLK_SEL = 10 (Internal RC Oscillator, no clock output)[cite: 1].
    // Bit 3-2: CS_SEL = 00 (No burnout current)
    // Bit 1-0: ADC_MODE = 11 (ADC Conversion mode)[cite: 1].
    // Value = 0b11100011 = 0xE3
    writeRegister(REG_CONFIG0, 0xE3);

    Serial.println("MCP3561/2/4R Initialized. Reading CH0 at 64x Gain...");
}

void loop() {
    // The data ready interrupt generates a falling edge on the IRQ pin[cite: 1].
    if (readADC) {
                // 1. Configure the Multiplexer (MUX Register 0x06)
    // To measure a shunt resistor connected to CH0, we configure VIN+ as CH0 (0000) 
    // and VIN- as AGND (1000)[cite: 1]. (Change 0x08 to 0x01 if using differential CH0/CH1).
    writeRegister(REG_MUX, 0x08);

    // 2. Configure Gain to 64x (CONFIG2 Register 0x03)
    // Bit 7-6: BOOST = 10 (Current x 1)
    // Bit 5-3: GAIN  = 111 (Gain is x64)[cite: 1].
    // Bit 2: AZ_MUX  = 0
    // Bit 1: AZ_REF  = 1
    // Bit 0: RESERVED = 1
    // Value = 0b10111011 = 0xBB
    writeRegister(REG_CONFIG2, 0xBB);

    // 3. Start ADC Conversions (CONFIG0 Register 0x01)
    // Bit 7: VREF_SEL = 1 (Internal reference selected)[cite: 1].
    // Bit 6: CONFIG0[6] = 1 (Required for normal mode)
    // Bit 5-4: CLK_SEL = 10 (Internal RC Oscillator, no clock output)[cite: 1].
    // Bit 3-2: CS_SEL = 00 (No burnout current)
    // Bit 1-0: ADC_MODE = 11 (ADC Conversion mode)[cite: 1].
    // Value = 0b11100011 = 0xE3
    writeRegister(REG_CONFIG0, 0xE3);
        readADC = 0;
        int32_t adcValue = readADCData();
        
        Serial.print("ADC Value: ");
        Serial.println(adcValue);   
        
        delay(250); // Optional: slow down printing for readability
    }
}