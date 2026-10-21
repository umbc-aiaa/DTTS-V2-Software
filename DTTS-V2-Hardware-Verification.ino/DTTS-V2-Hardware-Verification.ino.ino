#include <SPI.h>

// ---------------------------------------------------------
// ESP32-S3 Pin Definitions
// ---------------------------------------------------------
const int IRQ_PIN  = 9;
const int CS_PIN   = 10;
const int MOSI_PIN = 11;
const int SCK_PIN  = 12;
const int MISO_PIN = 13;

const int SENSE_VB_PIN = 1;

const int IN_C_PIN = 16;
const int IN_U_PIN = 17;
const int IN_D_PIN = 18;
const int IN_L_PIN = 38;
const int IN_R_PIN = 21;
const int IN_EST_PIN = 0;

const int LED_R_PIN = 8;

const int ESC_PWM_PIN = 15;

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
const uint8_t REG_CONFIG3 = 0x04; // Configuration Register 3
const uint8_t REG_MUX     = 0x06; // Multiplexer Register

volatile bool readADC = false;
int32_t adcValue = 0;

long timeOffset = 0;

int32_t throttle = 0;

void ARDUINO_ISR_ATTR updateReadADC() {
    readADC = true;
}

void ARDUINO_ISR_ATTR incThrottle(){
    throttle += 50;
    throttle = throttle > 255 ? 255: throttle;
}

void ARDUINO_ISR_ATTR decThrottle(){
    throttle -= 50;
    throttle = throttle < 0 ? 0: throttle;
}

void ARDUINO_ISR_ATTR rstThrottle(){
    throttle = 0;
}
void ARDUINO_ISR_ATTR setThrottle(){
    throttle = 255;
}

void ARDUINO_ISR_ATTR rstTime(){
    timeOffset = millis();
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
    pinMode(SENSE_VB_PIN, INPUT);

    pinMode(IN_C_PIN, INPUT_PULLUP);
    pinMode(IN_U_PIN, INPUT_PULLUP);
    pinMode(IN_D_PIN, INPUT_PULLUP);
    pinMode(IN_L_PIN, INPUT_PULLUP);
    pinMode(IN_R_PIN, INPUT_PULLUP);
    pinMode(IN_EST_PIN, INPUT_PULLUP);

    pinMode(LED_R_PIN, OUTPUT);

    pinMode(ESC_PWM_PIN, OUTPUT);

    digitalWrite(CS_PIN, HIGH);

    // Initialize SPI on ESP32-S3 custom pins (SCK, MISO, MOSI, SS)
    SPI.begin(SCK_PIN, MISO_PIN, MOSI_PIN, CS_PIN);
    
    // The device interface is compatible with both SPI 0,0 and 1,1 modes up to 20 MHz[cite: 1].
    SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));

    delay(100); // Allow time for Power-on Reset (POR)
    attachInterrupt(IRQ_PIN, updateReadADC, FALLING);

    attachInterrupt(IN_U_PIN, incThrottle, FALLING);
    attachInterrupt(IN_D_PIN, decThrottle, FALLING);
    attachInterrupt(IN_L_PIN, setThrottle, FALLING);
    attachInterrupt(IN_EST_PIN, rstThrottle, FALLING);
    attachInterrupt(IN_C_PIN, rstTime, FALLING);

    // 1. Configure the Multiplexer (MUX Register 0x06)
    // To measure a shunt resistor connected to CH0, we configure VIN+ as CH0 (0000) 
    // and VIN- as AGND (1000)[cite: 1]. (Change 0x08 to 0x01 if using differential CH0/CH1).
    writeRegister(REG_MUX, 0x01);

    // 2. Configure Gain to 64x (CONFIG2 Register 0x03)
    // Bit 7-6: BOOST = 10 (Current x 1)
    // Bit 5-3: GAIN  = 111 (Gain is x64)[cite: 1].
    // Bit 2: AZ_MUX  = 0
    // Bit 1: AZ_REF  = 1
    // Bit 0: RESERVED = 1
    // Value = 0b10111011 = 0xBB
    writeRegister(REG_CONFIG2, 0xBB);

    writeRegister(REG_CONFIG3, 0xC0);   // Continuous, 24-bit output

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
    if (readADC) {
        readADC = false;

        adcValue = readADCData();
    } else {

        const float VREF = 2.4;
        const float GAIN = 64.0;
        const float SHUNT_RESISTANCE = 0.005; // 5 mOhm

        float voltage = ((float)adcValue * VREF) /
                        (8388608.0 * GAIN);

        float current = voltage / SHUNT_RESISTANCE;

        // Serial.print("Raw CURR: ");
        Serial.print(millis() - timeOffset);
        Serial.print(",");
        Serial.print(adcValue);
        Serial.print(","  );
        Serial.println(analogRead(SENSE_VB_PIN));

        // Serial.print(" | Divider Voltage: ");
        // Serial.print(voltage * 1000.0, 6);
        // Serial.print(" mV");

        // Serial.print(" | Current: ");
        // Serial.print(current, 6);
        // Serial.println(" A");
        delay(10);
    }
    analogWrite(ESC_PWM_PIN, throttle);
    analogWrite(LED_R_PIN, 255-throttle);
}