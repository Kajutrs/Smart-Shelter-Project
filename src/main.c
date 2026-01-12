/* Smart Shelter - STM32L476RG Main Code (SIM800C Edition)
 * ===========================================================
 * მიკროკონტროლერი: STM32L476RG
 * GSM მოდული: SIM800C (GPRS-ით მონაცემთა გადასაცემად)
 */

#include "main.h"
#include "vl53l0x_api.h"
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

/* კონსტანტები */
#define DISTANCE_THRESHOLD 700
#define TEMP_THRESHOLD 18.0f
#define READ_INTERVAL 5000 // GSM-ისთვის 5 წამიანი ინტერვალი უფრო სტაბილურია

/* ჰენდლერები */
extern I2C_HandleTypeDef hi2c1;
extern UART_HandleTypeDef huart2;
extern TIM_HandleTypeDef htim2;

/* გლობალური ცვლადები */
float temperature = 0.0f;
float humidity = 0.0f;
uint16_t distance = 0;
uint8_t heating_status = 0;

/* ფუნქციების დეკლარაციები */
void SIM800_Init(void);
void SIM800_SendData(void);
bool SIM800_SendAT(const char* cmd, const char* expected, uint16_t timeout);
// ... (DHT22 და VL53L0X ფუნქციები იგივე რჩება)

int main(void) {
    HAL_Init();
    SystemClock_Config();
    MX_GPIO_Init();
    MX_I2C1_Init();
    MX_USART2_UART_Init();
    MX_TIM2_Init();
    HAL_TIM_Base_Start(&htim2);

    // სისტემის ინიციალიზაცია
    DHT22_Init();
    VL53L0X_Initialize();
    SIM800_Init(); // WiFi-ს ნაცვლად GSM-ის მომზადება

    while (1) {
        Read_Sensors();
        Control_Heating();
        SIM800_SendData(); // მონაცემების გაგზავნა GPRS-ით
        HAL_Delay(READ_INTERVAL);
    }
}

/* ====================================================================
 * SIM800C ფუნქციები (HTTP POST)
 * ==================================================================== */
void SIM800_Init(void) {
    // ქსელში რეგისტრაციის შემოწმება
    SIM800_SendAT("AT\r\n", "OK", 1000);
    SIM800_SendAT("AT+CPIN?\r\n", "READY", 1000);
    
    // GPRS კონფიგურაცია (APN ჩაანაცვლეთ თქვენი ოპერატორის მიხედვით, მაგ: "internet")
    SIM800_SendAT("AT+SAPBR=3,1,\"Contype\",\"GPRS\"\r\n", "OK", 1000);
    SIM800_SendAT("AT+SAPBR=3,1,\"APN\",\"internet\"\r\n", "OK", 1000);
    SIM800_SendAT("AT+SAPBR=1,1\r\n", "OK", 3000); // GPRS-ის ჩართვა
}

void SIM800_SendData(void) {
    char json[128];
    char cmd[32];
    
    // JSON-ის მომზადება
    snprintf(json, sizeof(json), "{\"temp\":%.1f,\"dist\":%d,\"heat\":%d}", 
             temperature, distance, heating_status);
    int len = strlen(json);

    // HTTP სესია
    SIM800_SendAT("AT+HTTPINIT\r\n", "OK", 1000);
    SIM800_SendAT("AT+HTTPPARA=\"CID\",1\r\n", "OK", 1000);
    SIM800_SendAT("AT+HTTPPARA=\"URL\",\"http://your-server-ip:8080/data\"\r\n", "OK", 1000);
    SIM800_SendAT("AT+HTTPPARA=\"CONTENT\",\"application/json\"\r\n", "OK", 1000);

    // მონაცემთა გადაცემა (Datasheet Section 3.10)
    sprintf(cmd, "AT+HTTPDATA=%d,5000\r\n", len);
    if (SIM800_SendAT(cmd, "DOWNLOAD", 2000)) {
        HAL_UART_Transmit(&huart2, (uint8_t*)json, len, 1000);
    }

    // POST მოქმედება
    SIM800_SendAT("AT+HTTPACTION=1\r\n", "+HTTPACTION: 1,200", 5000);
    SIM800_SendAT("AT+HTTPTERM\r\n", "OK", 1000);
}

bool SIM800_SendAT(const char* cmd, const char* expected, uint16_t timeout) {
    uint8_t buffer[256] = {0};
    HAL_UART_Transmit(&huart2, (uint8_t*)cmd, strlen(cmd), 500);
    HAL_UART_Receive(&huart2, buffer, sizeof(buffer), timeout);
    return (strstr((char*)buffer, expected) != NULL);
}

/* გათბობის ლოგიკა */
void Control_Heating(void) {
    if (distance < DISTANCE_THRESHOLD && temperature < TEMP_THRESHOLD) {
        HAL_GPIO_WritePin(GPIOA, GPIO_PIN_5, GPIO_PIN_SET);
        heating_status = 1;
    } else {
        HAL_GPIO_WritePin
