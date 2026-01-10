/* Smart Shelter - STM32L476RG Main Code (სრული რეალიზაცია)
 * ===========================================================
 * მიკროკონტროლერი: STM32L476RG (Nucleo-L476RG)
 * WiFi მოდული: ESP8266 (ESP-01)
 * 
 * აღწერა: ეს კოდი მართავს სენსორებს და გათბობის სისტემას ცხოველის თავშესაფრისთვის.
 * 
 * პინების კონფიგურაცია:
 * - PA1: DHT22 (ტემპერატურა და ტენიანობა)
 * - PB8/PB9: VL53L0X ToF (I2C1: SCL/SDA)
 * - PA5: რელე (გათბობის კონტროლი)
 * - PA2/PA3: USART2 → ESP-01 (115200 baud)
 * 
 * სპეციალური: TIM2 გამოიყენება მიკროწამიანი დაყოვნებებისთვის (DHT22)
 */

#include "main.h"
#include "vl53l0x_api.h"  // VL53L0X API
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

/* კონსტანტები */
#define DISTANCE_THRESHOLD 700
#define TEMP_THRESHOLD 18.0f
#define READ_INTERVAL 2000

/* ჰენდლერები */
extern I2C_HandleTypeDef hi2c1;
extern UART_HandleTypeDef huart2;
extern TIM_HandleTypeDef htim2;  // μs დაყოვნებებისთვის

/* გლობალური ცვლადები */
VL53L0X_Dev_t vl53_device;
VL53L0X_RangingMeasurementData_t vl53_measurement;

float temperature = 0.0f;
float humidity = 0.0f;
uint16_t distance = 0;
uint8_t heating_status = 0;
bool sensors_ready = false;

/* ფუნქციების დეკლარაციები */
void System_Init(void);
void Read_Sensors(void);
void Control_Heating(void);
void Send_Data_ESP01(void);

// DHT22 ფუნქციები
void DHT22_Init(void);
uint8_t DHT22_Read(float* temp, float* hum);
void DHT22_SetPinOutput(void);
void DHT22_SetPinInput(void);
uint8_t DHT22_WaitForPulse(uint8_t level, uint16_t timeout_us);

// VL53L0X ფუნქციები
bool VL53L0X_Initialize(void);
uint16_t VL53L0X_ReadDistance(void);

// ESP-01 ფუნქციები
void ESP01_Init(void);
bool ESP01_SendATCommand(const char* cmd, const char* expected_response, uint16_t timeout_ms);
void ESP01_SendData(const char* data);

// μs დაყოვნება (TIM2 გამოყენებით)
void delay_us(uint16_t us);

/* ====================================================================
 * მთავარი პროგრამა
 * ==================================================================== */
int main(void) {
    HAL_Init();
    SystemClock_Config();
    
    /* პერიფერიების ინიციალიზაცია */
    MX_GPIO_Init();
    MX_I2C1_Init();
    MX_USART2_UART_Init();
    MX_TIM2_Init();
    
    /* TIM2 გაშვება (μs counter) */
    HAL_TIM_Base_Start(&htim2);
    
    /* სისტემის ინიციალიზაცია */
    System_Init();
    
    /* მთავარი ციკლი */
    while (1) {
        if (sensors_ready) {
            Read_Sensors();
            Control_Heating();
            Send_Data_ESP01();
        }
        HAL_Delay(READ_INTERVAL);
    }
}

/* ====================================================================
 * სისტემის ინიციალიზაცია
 * ==================================================================== */
void System_Init(void) {
    /* რელე გამორთული */
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_5, GPIO_PIN_RESET);
    
    /* DHT22 ინიციალიზაცია */
    DHT22_Init();
    HAL_Delay(2000);  // DHT22-ს სჭირდება 2წმ ჩართვის შემდეგ
    
    /* VL53L0X ინიციალიზაცია */
    if (VL53L0X_Initialize()) {
        sensors_ready = true;
    } else {
        // შეცდომა: VL53L0X ვერ მოიძებნა
        sensors_ready = false;
    }
    
    /* ESP-01 ინიციალიზაცია */
    ESP01_Init();
    HAL_Delay(1000);
}

/* ====================================================================
 * DHT22 ფუნქციები (μs precision)
 * ==================================================================== */
void DHT22_Init(void) {
    DHT22_SetPinInput();
}

void DHT22_SetPinOutput(void) {
    GPIO_InitTypeDef GPIO_InitStruct = {0};
    GPIO_InitStruct.Pin = GPIO_PIN_1;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);
}

void DHT22_SetPinInput(void) {
    GPIO_InitTypeDef GPIO_InitStruct = {0};
    GPIO_InitStruct.Pin = GPIO_PIN_1;
    GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);
}

uint8_t DHT22_WaitForPulse(uint8_t level, uint16_t timeout_us) {
    uint16_t start_time = __HAL_TIM_GET_COUNTER(&htim2);
    while (HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_1) == level) {
        if ((__HAL_TIM_GET_COUNTER(&htim2) - start_time) > timeout_us) {
            return 0; // Timeout
        }
    }
    return 1;
}

uint8_t DHT22_Read(float* temp, float* hum) {
    uint8_t data[5] = {0};
    uint8_t bit_index = 0;
    
    /* 1. START Signal: LOW 1ms, then HIGH 30μs */
    DHT22_SetPinOutput();
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_1, GPIO_PIN_RESET);
    HAL_Delay(1);
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_1, GPIO_PIN_SET);
    delay_us(30);
    
    /* 2. Input mode და DHT22-ის პასუხის მოლოდინი */
    DHT22_SetPinInput();
    
    /* DHT22 პასუხობს LOW ~80μs, then HIGH ~80μs */
    if (!DHT22_WaitForPulse(GPIO_PIN_RESET, 100)) return 1;
    if (!DHT22_WaitForPulse(GPIO_PIN_SET, 100)) return 1;
    
    /* 3. 40 ბიტის წაკითხვა */
    for (int i = 0; i < 40; i++) {
        /* თითოეული ბიტი: LOW ~50μs, then HIGH 26-28μs (0) or 70μs (1) */
        if (!DHT22_WaitForPulse(GPIO_PIN_RESET, 70)) return 1;
        
        uint16_t start_time = __HAL_TIM_GET_COUNTER(&htim2);
        if (!DHT22_WaitForPulse(GPIO_PIN_SET, 100)) return 1;
        uint16_t pulse_width = __HAL_TIM_GET_COUNTER(&htim2) - start_time;
        
        /* თუ pulse > 40μs → ბიტი არის 1 */
        data[i / 8] <<= 1;
        if (pulse_width > 40) {
            data[i / 8] |= 1;
        }
    }
    
    /* 4. Checksum შემოწმება */
    if (data[4] != ((data[0] + data[1] + data[2] + data[3]) & 0xFF)) {
        return 1; // Checksum error
    }
    
    /* 5. მონაცემების დეკოდირება */
    *hum = ((data[0] << 8) | data[1]) / 10.0f;
    *temp = (((data[2] & 0x7F) << 8) | data[3]) / 10.0f;
    if (data[2] & 0x80) *temp *= -1;  // უარყოფითი ტემპერატურა
    
    return 0; // წარმატება
}

/* ====================================================================
 * VL53L0X ფუნქციები (ST API გამოყენებით)
 * ==================================================================== */
bool VL53L0X_Initialize(void) {
    VL53L0X_Error status;
    
    /* მოწყობილობის სტრუქტურის ინიციალიზაცია */
    vl53_device.I2cHandle = &hi2c1;
    vl53_device.I2cDevAddr = 0x52;  // 0x29 << 1
    
    /* 1. Data Initialization */
    status = VL53L0X_DataInit(&vl53_device);
    if (status != VL53L0X_ERROR_NONE) return false;
    
    /* 2. Static Initialization */
    status = VL53L0X_StaticInit(&vl53_device);
    if (status != VL53L0X_ERROR_NONE) return false;
    
    /* 3. Device Mode: Continuous Ranging */
    status = VL53L0X_SetDeviceMode(&vl53_device, VL53L0X_DEVICEMODE_CONTINUOUS_RANGING);
    if (status != VL53L0X_ERROR_NONE) return false;
    
    /* 4. Measurement Timing Budget: 33ms (30Hz) */
    status = VL53L0X_SetMeasurementTimingBudgetMicroSeconds(&vl53_device, 33000);
    if (status != VL53L0X_ERROR_NONE) return false;
    
    /* 5. გაზომვის დაწყება */
    status = VL53L0X_StartMeasurement(&vl53_device);
    if (status != VL53L0X_ERROR_NONE) return false;
    
    return true;
}

uint16_t VL53L0X_ReadDistance(void) {
    VL53L0X_Error status;
    
    /* მონაცემების მზაობის შემოწმება */
    uint8_t data_ready = 0;
    status = VL53L0X_GetMeasurementDataReady(&vl53_device, &data_ready);
    
    if (status == VL53L0X_ERROR_NONE && data_ready) {
        /* გაზომვის მონაცემების წაკითხვა */
        status = VL53L0X_GetRangingMeasurementData(&vl53_device, &vl53_measurement);
        
        if (status == VL53L0X_ERROR_NONE) {
            /* შემდეგი გაზომვის მოსამზადებლად */
            VL53L0X_ClearInterruptMask(&vl53_device, 0);
            
            /* სტატუსის შემოწმება */
            if (vl53_measurement.RangeStatus == 0) {  // Valid measurement
                return vl53_measurement.RangeMilliMeter;
            }
        }
    }
    
    return distance;  // წინა მნიშვნელობის დაბრუნება
}

/* ====================================================================
 * ESP-01 ფუნქციები (AT Commands)
 * ==================================================================== */
void ESP01_Init(void) {
    /* ESP-01-ის ტესტი */
    if (!ESP01_SendATCommand("AT\r\n", "OK", 1000)) {
        return; // ESP-01 არ პასუხობს
    }
    
    /* WiFi Station Mode */
    ESP01_SendATCommand("AT+CWMODE=1\r\n", "OK", 2000);
    
    /* WiFi-ზე დაკავშირება (ჩაანაცვლეთ თქვენი credentials) */
    ESP01_SendATCommand("AT+CWJAP=\"YourSSID\",\"YourPassword\"\r\n", "OK", 10000);
    
    /* Multiple connections disable */
    ESP01_SendATCommand("AT+CIPMUX=0\r\n", "OK", 2000);
}

bool ESP01_SendATCommand(const char* cmd, const char* expected_response, uint16_t timeout_ms) {
    uint8_t rx_buffer[256];
    uint16_t rx_len = 0;
    
    /* ბრძანების გაგზავნა */
    HAL_UART_Transmit(&huart2, (uint8_t*)cmd, strlen(cmd), 1000);
    
    /* პასუხის მოლოდინი */
    uint32_t start = HAL_GetTick();
    while ((HAL_GetTick() - start) < timeout_ms) {
        if (HAL_UART_Receive(&huart2, &rx_buffer[rx_len], 1, 10) == HAL_OK) {
            rx_len++;
            if (rx_len >= sizeof(rx_buffer) - 1) break;
        }
    }
    rx_buffer[rx_len] = '\0';
    
    /* expected_response-ის შემოწმება */
    return (strstr((char*)rx_buffer, expected_response) != NULL);
}

void ESP01_SendData(const char* data) {
    char cmd[32];
    uint16_t data_len = strlen(data);
    
    /* 1. Backend სერვერთან TCP კავშირი (IP და Port ჩაანაცვლეთ) */
    if (!ESP01_SendATCommand("AT+CIPSTART=\"TCP\",\"192.168.1.100\",8080\r\n", "CONNECT", 5000)) {
        return; // კავშირი ვერ დამყარდა
    }
    
    /* 2. CIPSEND: ბაიტების რაოდენობის მითითება */
    snprintf(cmd, sizeof(cmd), "AT+CIPSEND=%d\r\n", data_len);
    HAL_UART_Transmit(&huart2, (uint8_t*)cmd, strlen(cmd), 1000);
    
    /* 3. '>' სიმბოლოს მოლოდინი */
    uint8_t rx_char;
    uint32_t start = HAL_GetTick();
    while ((HAL_GetTick() - start) < 2000) {
        if (HAL_UART_Receive(&huart2, &rx_char, 1, 100) == HAL_OK) {
            if (rx_char == '>') break;
        }
    }
    
    /* 4. მონაცემების გაგზავნა */
    HAL_UART_Transmit(&huart2, (uint8_t*)data, data_len, 2000);
    
    /* 5. კავშირის დახურვა */
    HAL_Delay(500);
    ESP01_SendATCommand("AT+CIPCLOSE\r\n", "OK", 2000);
}

/* ====================================================================
 * სენსორების წაკითხვა
 * ==================================================================== */
void Read_Sensors(void) {
    /* DHT22 */
    if (DHT22_Read(&temperature, &humidity) != 0) {
        // შეცდომა - წინა მნიშვნელობა რჩება
    }
    
    /* VL53L0X */
    distance = VL53L0X_ReadDistance();
}

/* ====================================================================
 * გათბობის კონტროლი
 * ==================================================================== */
void Control_Heating(void) {
    if (distance < DISTANCE_THRESHOLD && temperature < TEMP_THRESHOLD) {
        HAL_GPIO_WritePin(GPIOA, GPIO_PIN_5, GPIO_PIN_SET);
        heating_status = 1;
    } else {
        HAL_GPIO_WritePin(GPIOA, GPIO_PIN_5, GPIO_PIN_RESET);
        heating_status = 0;
    }
}

/* ====================================================================
 * მონაცემების გაგზავნა ESP-01-ით Backend-ზე
 * ==================================================================== */
void Send_Data_ESP01(void) {
    char json[128];
    
    snprintf(json, sizeof(json),
             "{\"temp\":%.1f,\"humidity\":%.1f,\"distance\":%d,\"heating\":\"%s\"}",
             temperature, humidity, distance, heating_status ? "ON" : "OFF");
    
    ESP01_SendData(json);
}

/* ====================================================================
 * μs დაყოვნება (TIM2 გამოყენებით)
 * TIM2 კონფიგურაცია: 1MHz (1μs resolution)
 * ==================================================================== */
void delay_us(uint16_t us) {
    __HAL_TIM_SET_COUNTER(&htim2, 0);
    while (__HAL_TIM_GET_COUNTER(&htim2) < us);
}

/* ====================================================================
 * GPIO ინიციალიზაცია
 * ==================================================================== */
void MX_GPIO_Init(void) {
    GPIO_InitTypeDef GPIO_InitStruct = {0};
    
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    
    /* PA5: Relay */
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_5, GPIO_PIN_RESET);
    GPIO_InitStruct.Pin = GPIO_PIN_5;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);
}
