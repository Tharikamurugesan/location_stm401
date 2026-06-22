/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Fully Monitored Secure MQTTS Tracker to ThingsBoard
  * @platform       : STM32F401RET6U
  * @hardware       : USART1 (Quectel EC200U GNSS/LTE), USART6 (PC Terminal Monitor)
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* Private variables ---------------------------------------------------------*/
UART_HandleTypeDef huart1;
UART_HandleTypeDef huart6;

/* USER CODE BEGIN PV */
char cmd_buf[256];
char log_buf[128];
char telemetry_payload[128];
uint8_t response_buffer[512];

/* THINGSBOARD ENDPOINT SETUP */
#define THINGSBOARD_HOST    "mqtt.eu.thingsboard.cloud"
#define THINGSBOARD_PORT    "8883"
#define DEVICE_TOKEN        "KG6s8Oq6O9wcn6rAf19C"
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_USART1_UART_Init(void);
static void MX_USART6_UART_Init(void);
void Send_AT_Command(char* cmd, uint32_t timeout);
void Parse_And_Publish_Direct_GPS(char* raw_buffer);

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{
  HAL_Init();
  SystemClock_Config();
  MX_GPIO_Init();
  MX_USART1_UART_Init();
  MX_USART6_UART_Init();

  /* USER CODE BEGIN 2 */
  HAL_Delay(3000); // Wait for module stabilization on bootup

  sprintf(log_buf, "\r\n==================================================\r\n");
  HAL_UART_Transmit(&huart6, (uint8_t*)log_buf, strlen(log_buf), 1000);
  sprintf(log_buf, "    DIRECT SECURE GPS TO THINGSBOARD TELEMETRY    \r\n");
  HAL_UART_Transmit(&huart6, (uint8_t*)log_buf, strlen(log_buf), 1000);
  sprintf(log_buf, "==================================================\r\n\r\n");
  HAL_UART_Transmit(&huart6, (uint8_t*)log_buf, strlen(log_buf), 1000);

  // ----------------------------------------------------
  // STEP 1: CONFIGURE SECURE SSL PROFILE FOR BROKER CONNECTIONS
  // ----------------------------------------------------

  Send_AT_Command("AT\r\n", 1000);
  Send_AT_Command("ATI\r\n", 1000);
  Send_AT_Command("ATE0\r\n", 1000);                      // Turn off hardware command echo
  Send_AT_Command("AT+QSSLCFG=\"seclevel\",0,0\r\n", 1000); // No client certificate validation needed
  Send_AT_Command("AT+QSSLCFG=\"sslversion\",0,4\r\n", 1000); // Set SSL version to TLS 1.2
  Send_AT_Command("AT+QMTCFG=\"ssl\",0,1,0\r\n", 1000);     // Enable SSL configuration for MQTT context 0
  HAL_Delay(500);

  // ----------------------------------------------------
  // STEP 2: ACTIVATE GNSS RECEIVER CORE
  // ----------------------------------------------------


  Send_AT_Command("AT+QGPS=0\r\n", 1000); // Flush outstanding sessions
  HAL_Delay(500);
  Send_AT_Command("AT+QGPS=1\r\n", 2000); // Start fresh location tracking
  HAL_Delay(1000);

  // ----------------------------------------------------
  // STEP 3: ESTABLISH CONNECTION WITH THINGSBOARD MQTT SECURE SOCKET
  // ----------------------------------------------------

  Send_AT_Command("AT+QMTCLOSE=0\r\n", 1000);
    HAL_Delay(500);

  sprintf(cmd_buf, "AT+QMTOPEN=0,\"%s\",%s\r\n", THINGSBOARD_HOST, THINGSBOARD_PORT);
  Send_AT_Command(cmd_buf, 6000);
  HAL_Delay(3000); // Wait briefly for async +QMTOPEN response to hit the buffer

  // ----------------------------------------------------
  // STEP 4: AUTHENTICATE WITH DEVICE ACCESS TOKEN
  // ----------------------------------------------------

  sprintf(cmd_buf, "AT+QMTCONN=0,\"STM32_Telematics\",\"%s\",\"\"\r\n", DEVICE_TOKEN);
  Send_AT_Command(cmd_buf, 6000);
  HAL_Delay(2000);


  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    memset(response_buffer, 0, sizeof(response_buffer));

    // Echo outbound tracking request visibly to PuTTY
    sprintf(log_buf, "[TX -> Modem]: AT+QGPSLOC=2\r\n");
    HAL_UART_Transmit(&huart6, (uint8_t*)log_buf, strlen(log_buf), 1000);

    // Request location metrics using mode 0 (Decimal Degrees)
    HAL_UART_Transmit(&huart1, (uint8_t*)"AT+QGPSLOC=2\r\n", 14, 1000);

    // Receive response string back from module UART
    HAL_UART_Receive(&huart1, response_buffer, sizeof(response_buffer) - 1, 4000);

    // Display the exact raw data response from the modem to PuTTY
    sprintf(log_buf, "[RX <- Modem]:\r\n%s", (char*)response_buffer);
    HAL_UART_Transmit(&huart6, (uint8_t*)log_buf, strlen(log_buf), 1000);

    // If the module responds with Error 516, it means it doesn't have a satellite fix yet
    if (strstr((char*)response_buffer, "+CME ERROR: 516") != NULL)
    {
        sprintf(log_buf, ">> GNSS Status: Syncing with satellites (Waiting for Outdoor Fix...)\r\n\r\n");
        HAL_UART_Transmit(&huart6, (uint8_t*)log_buf, strlen(log_buf), 1000);
    }
    // Filter and extract when a valid location string containing coordinate markers arrives
    else if (strstr((char*)response_buffer, "+QGPSLOC:"))
    {
        Parse_And_Publish_Direct_GPS((char*)response_buffer);
    }

    /* USER CODE END WHILE */
    /* USER CODE BEGIN 3 */
    HAL_Delay(8000); // Upload coordinate telemetry packet once every 8 seconds
  }
  /* USER CODE END 3 */
}

/**
  * @brief Extract strings directly without modifications and push over secure MQTT pipeline
  */
void Parse_And_Publish_Direct_GPS(char* raw_buffer)
{
    char* token;
    char sub_tokens[12][24];
    int index = 0;

    // Isolate arguments past the leading token header colon
    char* data_start = strchr(raw_buffer, ':');
    if (!data_start) return;
    data_start++; // Skip past ':' character symbol

    char working_string[192];
    memset(working_string, 0, sizeof(working_string));
    strncpy(working_string, data_start, sizeof(working_string) - 1);

    // Split out arguments sequentially by target comma delimiter points
    token = strtok(working_string, ",");
    while (token != NULL && index < 12)
    {
        // Strip trailing/leading spaces or line return garbage from individual parameters
        sscanf(token, "%s", sub_tokens[index]);
        index++;
        token = strtok(NULL, ",");
    }

    // Validation: Mode 0 strings require at least 3 populated arrays (Time, Lat, Lon)
    if (index < 3) return;

    // Capture the decimal degree strings directly from token arrays
    char* latitude_str = sub_tokens[1];
    char* longitude_str = sub_tokens[2];

    // ----------------------------------------------------
    // DEDICATED SEPARATED COORDINATES MONITOR DISPLAY
    // ----------------------------------------------------
    char parse_out[128];
    sprintf(parse_out, "\r\n  ===============================\r\n");
    HAL_UART_Transmit(&huart6, (uint8_t*)parse_out, strlen(parse_out), 1000);
    sprintf(parse_out, "    PARSED LATITUDE  : %s\r\n", latitude_str);
    HAL_UART_Transmit(&huart6, (uint8_t*)parse_out, strlen(parse_out), 1000);
    sprintf(parse_out, "    PARSED LONGITUDE : %s\r\n", longitude_str);
    HAL_UART_Transmit(&huart6, (uint8_t*)parse_out, strlen(parse_out), 1000);
    sprintf(parse_out, "  ===============================\r\n\r\n");
    HAL_UART_Transmit(&huart6, (uint8_t*)parse_out, strlen(parse_out), 1000);

    // Construct telemetry payload securely using pure string embedding
    memset(telemetry_payload, 0, sizeof(telemetry_payload));
    sprintf(telemetry_payload, "{\"latitude\":%s,\"longitude\":%s}", latitude_str, longitude_str);

    int payload_len = strlen(telemetry_payload);

    // Construct the actual publishing command to show in tracking logs
    memset(cmd_buf, 0, sizeof(cmd_buf));
    sprintf(cmd_buf, "AT+QMTPUB=0,1,1,0,\"v1/devices/me/telemetry\",%d,\"%s\"\r\n", payload_len, telemetry_payload);

    // Echo publishing transmission execution directly to PuTTY monitor
    sprintf(log_buf, "[TX -> Modem]: %s", cmd_buf);
    HAL_UART_Transmit(&huart6, (uint8_t*)log_buf, strlen(log_buf), 1000);

    // Forward telemetry frame via automated AT publishing string onto secure cellular line
    HAL_UART_Transmit(&huart1, (uint8_t*)cmd_buf, strlen(cmd_buf), 1000);

    // Catch response from the publish request and output it
    memset(response_buffer, 0, sizeof(response_buffer));
    HAL_UART_Receive(&huart1, response_buffer, sizeof(response_buffer) - 1, 2000);
    sprintf(log_buf, "[RX <- Modem]:\r\n%s", (char*)response_buffer);
    HAL_UART_Transmit(&huart6, (uint8_t*)log_buf, strlen(log_buf), 1000);
}

void Send_AT_Command(char* cmd, uint32_t timeout) {
  char echo_buf[256];

  // Format outbound command mirror string explicitly
  sprintf(echo_buf, "[TX -> Modem]: %s", cmd);
  HAL_UART_Transmit(&huart6, (uint8_t*)echo_buf, strlen(echo_buf), 1000);

  // Forward command down the wire to the cellular module over USART1
  HAL_UART_Transmit(&huart1, (uint8_t*)cmd, strlen(cmd), 1000);

  memset(response_buffer, 0, sizeof(response_buffer));
  HAL_UART_Receive(&huart1, response_buffer, sizeof(response_buffer) - 1, timeout);

  // Format inbound response mirror string explicitly
  sprintf(echo_buf, "[RX <- Modem]:\r\n%s\r\n", (char*)response_buffer);
  HAL_UART_Transmit(&huart6, (uint8_t*)echo_buf, strlen(echo_buf), 1000);
}

/* STM32 Clock & Peripherals Low Level Controls */
void SystemClock_Config(void) {
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE2);
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLM = 16;
  RCC_OscInitStruct.PLL.PLLN = 336;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV4;
  RCC_OscInitStruct.PLL.PLLQ = 7;
  HAL_RCC_OscConfig(&RCC_OscInitStruct);
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK|RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;
  HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2);
}

static void MX_USART1_UART_Init(void) {
  huart1.Instance = USART1;
  huart1.Init.BaudRate = 115200;
  huart1.Init.WordLength = UART_WORDLENGTH_8B;
  huart1.Init.StopBits = UART_STOPBITS_1;
  huart1.Init.Parity = UART_PARITY_NONE;
  huart1.Init.Mode = UART_MODE_TX_RX;
  huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart1.Init.OverSampling = UART_OVERSAMPLING_16;
  HAL_UART_Init(&huart1);
}

static void MX_USART6_UART_Init(void) {
  huart6.Instance = USART6;
  huart6.Init.BaudRate = 115200;
  huart6.Init.WordLength = UART_WORDLENGTH_8B;
  huart6.Init.StopBits = UART_STOPBITS_1;
  huart6.Init.Parity = UART_PARITY_NONE;
  huart6.Init.Mode = UART_MODE_TX_RX;
  huart6.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart6.Init.OverSampling = UART_OVERSAMPLING_16;
  HAL_UART_Init(&huart6);
}

static void MX_GPIO_Init(void) {
  __HAL_RCC_GPIOH_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();
}

void Error_Handler(void) {
  __disable_irq();
  while (1) {}
}
