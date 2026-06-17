/*
 * SHA256 Mining Slave for ESP32-C3 (Single Core RISC-V)
 */
#include <Arduino.h>
#include <Wire.h>
#include "mbedtls/sha256.h"

#define I2C_ADDRESS     0x1C    // CHANGE PER C3 BOARD: 0x18, 0x19, 0x1A, 0x1B, 0x1C
#define I2C_SDA         20      // Edge grouped (Hijacked UART)
#define I2C_SCL         21      // Edge grouped (Hijacked UART)
#define I2C_FREQ        100000 
#define ATTENTION_PIN   1       // Moved off GPIO 0 (strapping pin), tied to Master's ATTN_1_PIN

#define CMD_NEW_JOB     0x01
#define CMD_STOP        0x02
#define CMD_SET_RANGE   0x04

#define RESP_READY      0x00
#define RESP_BUSY       0x01
#define RESP_FOUND      0x02
#define RESP_EXHAUSTED  0x03

struct BlockHeader {
    uint32_t version;
    uint8_t  prevHash[32];
    uint8_t  merkleRoot[32];
    uint32_t timestamp;
    uint32_t bits;
    uint32_t nonce;
} __attribute__((packed));

struct MiningJob {
    BlockHeader header;
    uint8_t  target[32];
    uint32_t nonceStart;
    uint32_t nonceEnd;
    uint32_t jobId;
    bool     valid;
};

struct MiningResult {
    uint8_t  status;
    uint32_t nonce;
    uint32_t hashrate;
    uint32_t jobId;
    uint8_t  hash[32];
};

volatile MiningJob currentJob;
volatile MiningResult lastResult;
volatile bool hasNewJob = false;
volatile bool stopMining = false;
volatile bool shareFound = false;

volatile uint32_t totalHashes = 0;
volatile uint32_t hashrate = 0;
unsigned long lastHashrateCalc = 0;

volatile bool shareSentToMaster = false;
volatile uint32_t currentNonceCore0 = 0;
volatile bool noncePositionValid = false;

uint8_t i2cRxBuffer[140];
uint8_t i2cTxBuffer[64];
volatile int i2cRxLen = 0;
volatile bool i2cDataReady = false;

TaskHandle_t miningTask0;
SemaphoreHandle_t jobMutex;

int compare256(const uint8_t* a, const uint8_t* b) {
    for(int i = 31; i >= 0; i--) {
        if(a[i] < b[i]) return -1;
        if(a[i] > b[i]) return 1;
    }
    return 0;
}

bool hashMeetsTarget(const uint8_t* hash, const uint8_t* target) {
    return compare256(hash, target) < 0;
}

void doubleSHA256(const uint8_t* data, size_t len, uint8_t* hash) {
    uint8_t firstHash[32];
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, data, len);
    mbedtls_sha256_finish(&ctx, firstHash);
    mbedtls_sha256_free(&ctx);
    
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, firstHash, 32);
    mbedtls_sha256_finish(&ctx, hash);
    mbedtls_sha256_free(&ctx);
}

void mineBlock(int coreId, int step, int offset) {
    if(!currentJob.valid) return;
    
    BlockHeader header;
    uint8_t hash[32];
    memcpy(&header, (void*)&currentJob.header, sizeof(BlockHeader));
    
    uint32_t start;
    if(noncePositionValid) {
        start = currentNonceCore0;
        start += step;
    } else {
        start = currentJob.nonceStart + offset;
    }
    
    uint32_t end = currentJob.nonceEnd;
    uint32_t yieldCounter = 0;
    
    for(uint32_t nonce = start; nonce <= end && !stopMining && !shareFound; nonce += step) {
        if(digitalRead(ATTENTION_PIN) == HIGH) {
            while(digitalRead(ATTENTION_PIN) == HIGH) delayMicroseconds(100);
        }
        
        header.nonce = nonce;
        doubleSHA256((uint8_t*)&header, 80, hash);
        totalHashes++;
        
        if(++yieldCounter >= 10000) {
            yieldCounter = 0;
            currentNonceCore0 = nonce;
            noncePositionValid = true;
            vTaskDelay(1); 
        }
        
        if(hashMeetsTarget(hash, (const uint8_t*)currentJob.target)) {
            shareFound = true;
            stopMining = true;
            
            currentNonceCore0 = nonce;
            noncePositionValid = true;
            
            xSemaphoreTake(jobMutex, portMAX_DELAY);
            lastResult.status = RESP_FOUND;
            lastResult.nonce = nonce;
            lastResult.jobId = currentJob.jobId;
            memcpy((void*)lastResult.hash, hash, 32);
            shareSentToMaster = false;
            xSemaphoreGive(jobMutex);
            return;
        }
    }
    
    if(!shareFound && !stopMining) {
        xSemaphoreTake(jobMutex, portMAX_DELAY);
        lastResult.status = RESP_EXHAUSTED;
        lastResult.jobId = currentJob.jobId;
        xSemaphoreGive(jobMutex);
    }
}

void miningTaskCore0(void* parameter) {
    while(true) {
        if(currentJob.valid && !shareFound && !stopMining) {
            mineBlock(0, 1, 0); // Single core, step by 1
        }
        vTaskDelay(1);
    }
}

void statsTask(void* parameter) {
    while(true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        unsigned long now = millis();
        unsigned long elapsed = now - lastHashrateCalc;
        if(elapsed >= 1000) {
            hashrate = (totalHashes * 1000) / elapsed;
            totalHashes = 0;
            lastHashrateCalc = now;
            
            xSemaphoreTake(jobMutex, portMAX_DELAY);
            lastResult.hashrate = hashrate;
            xSemaphoreGive(jobMutex);
        }
    }
}

void i2cReceiveEvent(int numBytes) {
    i2cRxLen = 0;
    while(Wire.available() && i2cRxLen < sizeof(i2cRxBuffer)) {
        i2cRxBuffer[i2cRxLen++] = Wire.read();
    }
    i2cDataReady = true;
}

void i2cRequestEvent() {
    xSemaphoreTake(jobMutex, portMAX_DELAY);
    if(lastResult.status == RESP_FOUND && !shareSentToMaster) {
        i2cTxBuffer[0] = RESP_FOUND;
        memcpy(&i2cTxBuffer[1], (void*)&lastResult.nonce, 4);
        memcpy(&i2cTxBuffer[5], (void*)&lastResult.hashrate, 4);
        memcpy(&i2cTxBuffer[9], (void*)&lastResult.jobId, 4);
        memcpy(&i2cTxBuffer[13], (void*)lastResult.hash, 32);
        Wire.write(i2cTxBuffer, 45);
        shareSentToMaster = true;
        lastResult.status = RESP_BUSY;
        shareFound = false;
        stopMining = false;
        hasNewJob = true;
    } 
    else if(lastResult.status == RESP_BUSY || currentJob.valid) {
        i2cTxBuffer[0] = RESP_BUSY;
        memcpy(&i2cTxBuffer[1], (void*)&lastResult.hashrate, 4);
        Wire.write(i2cTxBuffer, 5);
    }
    else if(lastResult.status == RESP_EXHAUSTED) {
        memcpy(&i2cTxBuffer[1], (void*)&lastResult.jobId, 4);
        Wire.write(i2cTxBuffer, 5);
        lastResult.status = RESP_READY;
    }
    else {
        i2cTxBuffer[0] = RESP_READY;
        memcpy(&i2cTxBuffer[1], (void*)&hashrate, 4);
        Wire.write(i2cTxBuffer, 5);
    }
    xSemaphoreGive(jobMutex);
}

void processI2CData() {
    if(!i2cDataReady || i2cRxLen == 0) return;
    i2cDataReady = false;
    
    uint8_t cmd = i2cRxBuffer[0];
    if (cmd == CMD_NEW_JOB && i2cRxLen >= 125) {
        stopMining = true;
        shareFound = false;
        shareSentToMaster = true; 
        noncePositionValid = false;
        vTaskDelay(10);
        xSemaphoreTake(jobMutex, portMAX_DELAY);
        
        memcpy((void*)&currentJob.header, &i2cRxBuffer[1], 80);
        
        uint8_t tempTarget[32];
        for(int i = 0; i < 32; i++) tempTarget[i] = i2cRxBuffer[81 + 31 - i];
        memcpy((void*)currentJob.target, tempTarget, 32);
        
        memcpy((void*)&currentJob.nonceStart, &i2cRxBuffer[113], 4);
        memcpy((void*)&currentJob.nonceEnd, &i2cRxBuffer[117], 4);
        memcpy((void*)&currentJob.jobId, &i2cRxBuffer[121], 4);
        currentJob.valid = true;
        lastResult.status = RESP_BUSY;
        lastResult.jobId = currentJob.jobId;
        
        xSemaphoreGive(jobMutex);
        stopMining = false;
        hasNewJob = true;
    } else if (cmd == CMD_STOP) {
        stopMining = true;
        currentJob.valid = false;
        lastResult.status = RESP_READY;
    }
}

void setup() {
    Serial.begin(115200);
    delay(800);   // let USB CDC enumerate so the boot banner is visible
    Serial.println();
    Serial.println("======================================");
    Serial.printf ("  C3 SINGLE-CORE SLAVE\n");
    Serial.printf ("  I2C ADDRESS: 0x%02X\n", I2C_ADDRESS);
    Serial.printf ("  SDA=%d  SCL=%d  ATTN=%d\n", I2C_SDA, I2C_SCL, ATTENTION_PIN);
    Serial.println("======================================");
    pinMode(ATTENTION_PIN, INPUT_PULLDOWN);
    jobMutex = xSemaphoreCreateMutex();
    
    currentJob.valid = false;
    lastResult.status = RESP_READY;
    shareSentToMaster = true;
    
    Wire.begin(I2C_ADDRESS, I2C_SDA, I2C_SCL, I2C_FREQ);
    Wire.onReceive(i2cReceiveEvent);
    Wire.onRequest(i2cRequestEvent);

    xTaskCreatePinnedToCore(miningTaskCore0, "Mine0", 8192, NULL, 1, &miningTask0, 0);
    xTaskCreate(statsTask, "Stats", 2048, NULL, 0, NULL);
}

void loop() {
    processI2CData();

    // Periodic identity heartbeat so you can connect at any time and
    // confirm THIS board's assigned address over its own USB serial.
    static unsigned long lastPrint = 0;
    if(millis() - lastPrint > 2000) {
        lastPrint = millis();
        Serial.printf("[C3 slave 0x%02X] status=%u  hashrate=%lu H/s\n",
                      I2C_ADDRESS, lastResult.status, (unsigned long)hashrate);
    }

    delay(10);
}
