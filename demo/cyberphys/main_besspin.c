/* Standard includes. */
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

/* Kernel includes. */
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

/* IP stack includes. */
#include "FreeRTOS_IP.h"
#include "FreeRTOS_Sockets.h"

/* Drivers */
#include "bsp.h"

#if BSP_USE_IIC0
    #include "iic.h"
#else
    #define TRUE  1
    #define FALSE 0
#endif

/* Canlib */
#include "canspecs.h"
#include "canlib.h"
#include "j1939.h"

/* Hack */
#include "hackFault.h"

/* FETT config */

#ifndef __waf__
#include "fettFreeRTOSConfig.h"
#include "fettFreeRTOSIPConfig.h"
#endif

#define STRINGIZE_NX(A) #A
#define STRINGIZE(A) STRINGIZE_NX(A)
#define CYBERPHYS_BROADCAST_ADDR STRINGIZE(configGATEWAY_ADDR0) \
"." STRINGIZE(configGATEWAY_ADDR1) "." STRINGIZE(configGATEWAY_ADDR2) ".255"

#define MAINTASK_STACK_SIZE configMINIMAL_STACK_SIZE * 10U
#define SENSORTASK_STACK_SIZE configMINIMAL_STACK_SIZE * 20U
#define CAN_TX_STACK_SIZE configMINIMAL_STACK_SIZE * 10U
#define CAN_RX_STACK_SIZE configMINIMAL_STACK_SIZE * 10U
#define IP_RESTART_STACK_SIZE configMINIMAL_STACK_SIZE * 2U
#define INFOTASK_STACK_SIZE configMINIMAL_STACK_SIZE * 10U
#define HACK_STACK_SIZE configMINIMAL_STACK_SIZE * 10U
#define TEST_STACK_SIZE configMINIMAL_STACK_SIZE * 10U

#define MAINTASK_PRIORITY tskIDLE_PRIORITY + 5
#define IP_RESTART_TASK_PRIORITY tskIDLE_PRIORITY + 5
#define SENSORTASK_PRIORITY tskIDLE_PRIORITY + 4
#define CAN_RX_TASK_PRIORITY tskIDLE_PRIORITY + 3
#define INFOTASK_PRIORITY tskIDLE_PRIORITY + 1
#define HACK_TASK_PRIORITY tskIDLE_PRIORITY
#define TEST_TASK_PRIORITY tskIDLE_PRIORITY+10

#define SENSOR_LOOP_DELAY_MS pdMS_TO_TICKS(50)

#define SENSOR_POWER_UP_DELAY_MS pdMS_TO_TICKS(100)
#define BROADCAST_LOOP_DELAY_MS pdMS_TO_TICKS(50)
#define INFOTASK_LOOP_DELAY_MS pdMS_TO_TICKS(1000)
#define HACKTASK_LOOP_DELAY_MS pdMS_TO_TICKS(120)
#define TESTTASK_LOOP_DELAY_MS pdMS_TO_TICKS(50)

#define THROTTLE_MAX 926 // fully pressed
#define THROTTLE_MIN 64
#define THROTTLE_GAIN 100

#define BRAKE_MAX 270
#define BRAKE_MIN 50
#define BRAKE_GAIN 100

#define TEENSY_I2C_ADDRESS 0x30

#define NOTIFY_SUCCESS_NTK  0x00000001

static void prvSensorTask(void *pvParameters);
static void prvCanRxTask(void *pvParameters);
static void prvInfoTask(void *pvParameters);
static void prvIPRestartHandlerTask(void *pvParameters);
static void prvSocketCreateSensorIP();
static void prvSocketCreateCanRx();

void main_besspin(void);
void prvMainTask (void *pvParameters);
void startNetwork(void);
char *getCurrTime(void);
uint8_t process_j1939(Socket_t xListeningSocket, struct freertos_sockaddr *xClient, size_t *msg_len, canid_t *can_id, uint8_t *msg_buf);
int16_t min(int16_t a, int16_t b);
int16_t max(int16_t a, int16_t b);

uint32_t ulApplicationGetNextSequenceNumber(uint32_t ulSourceAddress, uint16_t usSourcePort,
                                            uint32_t ulDestinationAddress, uint16_t usDestinationPort);

SemaphoreHandle_t data_mutex;
TaskHandle_t xMainTask = NULL;
TaskHandle_t xIPRestartHandlerTask = NULL;
TaskHandle_t xSensorTask = NULL;
TaskHandle_t xCanTask = NULL;

static volatile Socket_t xCanRxListeningSocket;
static volatile Socket_t xCanRxClientSocket;
static volatile struct freertos_sockaddr xDestinationAddress;
static volatile Socket_t xSensorClientSocket;
static volatile Socket_t xHackSocket;

char * global_fault_buffer[60];

/* CAN rx buffer */
uint8_t j1939_rx_buf[0x100] __attribute__((aligned(64)));

/* Stereing assist config */
bool camera_ok;
uint8_t steering_assist;

/* Gains */
int16_t throttle_gain;
int16_t brake_gain;
int16_t throttle_min;
int16_t throttle_max;
int16_t brake_min;
int16_t brake_max;
/* Raw readings */
int16_t throttle_raw;
uint8_t gear_raw;
int16_t brake_raw;

/* Transmission status */
bool transmission_ok;

/* Final values */
uint8_t throttle;
uint8_t brake;
uint8_t gear;

/* Debug info */
uint32_t hz_sensor_task;
uint32_t hz_sensor_throttle_task;

/* Network config variables */
static const uint8_t ucIPAddress[4] = {configIP_ADDR0, configIP_ADDR1, configIP_ADDR2, configIP_ADDR3};
static const uint8_t ucNetMask[4] = {configNET_MASK0, configNET_MASK1, configNET_MASK2, configNET_MASK3};
static const uint8_t ucGatewayAddress[4] = {configGATEWAY_ADDR0, configGATEWAY_ADDR1, configGATEWAY_ADDR2, configGATEWAY_ADDR3};
static const uint8_t ucDNSServerAddress[4] = {configDNS_SERVER_ADDR0, configDNS_SERVER_ADDR1, configDNS_SERVER_ADDR2, configDNS_SERVER_ADDR3};
const uint8_t ucMACAddress[6] = {configMAC_ADDR0, configMAC_ADDR1, configMAC_ADDR2, configMAC_ADDR3, configMAC_ADDR4, configMAC_ADDR5};

/* Auxilliary function */
int16_t min(int16_t a, int16_t b)
{
    if (a > b)
    {
        return b;
    }
    else
    {
        return a;
    }
}

/* Auxilliary function */
int16_t max(int16_t a, int16_t b)
{
    if (a > b)
    {
        return a;
    }
    else
    {
        return b;
    }
}

/**
 * Print uptime in human readable format
 * "HH:MM:SS"
 */
char *getCurrTime(void)
{
#ifdef USE_CURRENT_TIME
    static char buf[16] = {0};
    TickType_t t = xTaskGetTickCount();
    uint32_t n_seconds = t / configTICK_RATE_HZ;
    uint32_t n_ms = t - n_seconds * configTICK_RATE_HZ;
    n_ms = (n_ms * 1000) / configTICK_RATE_HZ;
    uint32_t n_minutes = n_seconds / 60;
    uint32_t n_hours = n_minutes / 60;

    n_seconds = n_seconds - n_minutes * 60;
    n_minutes = n_minutes - n_hours * 60;

    sprintf(buf, "%02u:%02u:%02u.%03u", n_hours, n_minutes, n_seconds, n_ms);
    return buf;
#else
    return "";
#endif
}

/*
--- This function is called by FreeRTOS_TCP_IP.c. This is a dirty implementation
 * Callback that provides the inputs necessary to generate a randomized TCP
 * Initial Sequence Number per RFC 6528.  THIS IS ONLY A DUMMY IMPLEMENTATION
 * THAT RETURNS A PSEUDO RANDOM NUMBER SO IS NOT INTENDED FOR USE IN PRODUCTION
 * SYSTEMS.
 */
uint32_t ulApplicationGetNextSequenceNumber(uint32_t ulSourceAddress,
                                            uint16_t usSourcePort,
                                            uint32_t ulDestinationAddress,
                                            uint16_t usDestinationPort)
{
    (void)ulSourceAddress;
    (void)usSourcePort;
    (void)ulDestinationAddress;
    (void)usDestinationPort;

    return uxRand();
}

/*
 * Set *pulNumber to a random number, and return pdTRUE. When the random number
 * generator is broken, it shall return pdFALSE.
 * The macros ipconfigRAND32() and configRAND32() are not in use
 * anymore in FreeRTOS+TCP.
 *
 * THIS IS ONLY A DUMMY IMPLEMENTATION THAT RETURNS A PSEUDO RANDOM NUMBER SO IS
 * NOT INTENDED FOR USE IN PRODUCTION SYSTEMS.
 */
#if 0
BaseType_t xApplicationGetRandomNumber( uint32_t * pulNumber )
{
    *pulNumber = uxRand();
    return pdTRUE;
}
#endif

/* Called automatically when a reply to an outgoing ping is received. */
void vApplicationPingReplyHook( ePingReplyStatus_t eStatus,
                                uint16_t usIdentifier )
{
    static const char * pcSuccess = "Ping reply received - ";
    static const char * pcInvalidChecksum = "Ping reply received with invalid checksum - ";
    static const char * pcInvalidData = "Ping reply received with invalid data - ";

    switch( eStatus )
    {
        case eSuccess:
            FreeRTOS_printf( ( pcSuccess ) );
            break;

        case eInvalidChecksum:
            FreeRTOS_printf( ( pcInvalidChecksum ) );
            break;

        case eInvalidData:
            FreeRTOS_printf( ( pcInvalidData ) );
            break;

        default:

            /* It is not possible to get here as all enums have their own
             * case. */
            break;
    }

    FreeRTOS_printf( ( "identifier %d\r\n", ( int ) usIdentifier ) );

    /* Prevent compiler warnings in case FreeRTOS_debug_printf() is not defined. */
    ( void ) usIdentifier;
}

void startNetwork()
{
    BaseType_t funcReturn;

    funcReturn = FreeRTOS_IPInit(ucIPAddress, ucNetMask, ucGatewayAddress, ucDNSServerAddress, ucMACAddress);
    if (funcReturn != pdPASS)
    {
        FreeRTOS_printf(("%s (Error)~  startNetwork: Failed to initialize network. [ret=%ld].\r\n", getCurrTime(), funcReturn));
    }
    else
    {
        FreeRTOS_printf(("%s (Info)~  startNetwork: Network IP initialized successfully!.\r\n", getCurrTime()));
    }

    FreeRTOS_printf(("%s ECU: FreeRTOS_IPInit\r\n", getCurrTime()));
}

void main_besspin(void)
{
    BaseType_t funcReturn;

    if( xTaskGetSchedulerState() == taskSCHEDULER_RUNNING )
    {
        vTaskSuspendAll();
    }

    FreeRTOS_printf(("\n>>>Beginning of Besspin<<<\r\n"));
    
//startNetwork();

    funcReturn = xTaskCreate(prvMainTask, "prvMainTask", MAINTASK_STACK_SIZE, NULL, MAINTASK_PRIORITY, NULL);

    if (funcReturn == pdPASS) {
        FreeRTOS_printf(("%s (Info)~  main_besspin: Created prvMainTask successfully.<<<\r\n", getCurrTime()));
    } else {
        FreeRTOS_printf(("%s (Error)~  main_besspin: Failed to create prvMainTask.<<<\r\n", getCurrTime()));
    }

    /* Start the RTOS scheduler. */
    FreeRTOS_debug_printf( ( "vTaskStartScheduler\n" ) );
    if( xTaskGetSchedulerState() == taskSCHEDULER_SUSPENDED )
    {
        xTaskResumeAll();
        while ( 1 );
    }
}

/**
 * Suspended tasks that use sockets, recreate the sockets after a TCP/IP stack
 * restarts, then resume the tasks.
 */
void prvIPRestartHandlerTask(void *pvParameters) {
    while(1) {
        FreeRTOS_printf(("ip restart handler called\r\n"));
        ulTaskNotifyTake( pdFALSE, portMAX_DELAY );
        prvSocketCreateCanRx();
        prvSocketCreateSensorIP();
        vTaskResume(xCanTask);
        vTaskResume(xSensorTask);
    }
}



TickType_t BeginTime;
TickType_t EndTime;
/**
 * Creating an attack task
 * Attack Scenario: corrupted library
 * don't forget to un-comment the attack task call on main task if you want to run it
 */
static void prvHackTask(void * pvParameters){
    FreeRTOS_printf(("%s (Info)~ Hack Task Started.\r\n", getCurrTime()));
    FreeRTOS_printf(("%s (Hack)~ Before fault.\r\n", getCurrTime()));
    char local_fault_buffer[50];
    strcpy(local_fault_buffer, "initial value");
    //char * buffer = global_fault_buffer;
    char  * buffer = local_fault_buffer;
    for(;;){
        //strcpy(buffer, "before everything");
    //     BeginTime = xTaskGetTickCount();
     if(fault(buffer) == -2){
    //     EndTime = xTaskGetTickCount();
    //     printf("ERROR RAISED\n");
    //     //break;
      } else {
    //     EndTime = xTaskGetTickCount();
    //     printf("NORMAL OPERATION\n");
     }

    //     printf("BUFFER VALUE:%s\n", buffer);
    //     TickType_t t = EndTime - BeginTime;
    //     uint32_t n_seconds = t / configTICK_RATE_HZ;
    //     uint32_t n_ms = t - n_seconds * configTICK_RATE_HZ;
    //     n_ms = (n_ms * 1000) / configTICK_RATE_HZ;
    //     uint32_t n_minutes = n_seconds / 60;
    //     uint32_t n_hours = n_minutes / 60;

    //     n_seconds = n_seconds - n_minutes * 60;
    //     n_minutes = n_minutes - n_hours * 60;
    //     printf("fault handle Measure: ms:%03u seconds:%02u minutes:%02u",n_ms, n_seconds, n_minutes);

       /*
       printf("second fault test\n");
       fault2();
       printf("third fault test\n");
       int x = 0;
       x = fault3(x);
       printf("%i\n",x);
       printf("fourth test\n");
       fault4(local_fault_buffer);
       printf("%s\n",local_fault_buffer);
       printf("fith test\n");
       fault5(x);
       printf("sixth test\n");
       fault6(global_fault_buffer);
       printf("%s\n", global_fault_buffer);
        printf("seventh test\n");
       fault7(global_fault_buffer);
       printf("%s\n", global_fault_buffer);
       printf("heigth test");
       int count = fault8();
       printf("%i\n", count);

       printf("End of faulting tests\n");
        */
    printf("\nBEFORE ATTEMPTING TO STOP ALL TASKS\n");
    fault9(&vTaskSuspendAll);    
    vTaskDelay(HACKTASK_LOOP_DELAY_MS);
    }
}


void prvTestTask(void * pvParameters){
    TickType_t BeginTestTime;
    TickType_t EndTestTime;
    for(;;){
    BeginTime = xTaskGetTickCount();
    for(int i=0;i<1000000;i++){
        test();
    }
    EndTime = xTaskGetTickCount();
    TickType_t t = EndTime - BeginTime;
    uint32_t n_seconds = t / configTICK_RATE_HZ;
    uint32_t n_ms = t - n_seconds * configTICK_RATE_HZ;
    n_ms = (n_ms * 1000) / configTICK_RATE_HZ;
    uint32_t n_minutes = n_seconds / 60;
    uint32_t n_hours = n_minutes / 60;
    n_seconds = n_seconds - n_minutes * 60;
    n_minutes = n_minutes - n_hours * 60;
    printf("test time: ms:%03u seconds:%02u minutes:%02u",n_ms, n_seconds, n_minutes);
    vTaskDelay(TESTTASK_LOOP_DELAY_MS);
    }
}

void prvMainTask (void *pvParameters) {
    (void) pvParameters;
    BaseType_t funcReturn;
    uint32_t recvNotification = 0;

    xMainTask = xTaskGetCurrentTaskHandle();
    FreeRTOS_printf(("%s (Info)~  prvMainTask started.\r\n", getCurrTime()));

    startNetwork();uint8_t dummy = 1;
    // Give the sensor time to power up
    vTaskDelay(SENSOR_POWER_UP_DELAY_MS);

    funcReturn = xTaskNotifyWait(0xffffffffUL, 0xffffffffUL, &recvNotification, pdMS_TO_TICKS(60000)); //it should take less than 15s
    if (funcReturn != pdPASS) {
        FreeRTOS_printf (("%s (Error)~  prvMainTask: Failed to receive a notification.\r\n", getCurrTime()));
        vTaskDelete(NULL);
    } else if (recvNotification != NOTIFY_SUCCESS_NTK) {
        FreeRTOS_printf (("%s (Error)~  prvMainTask: Unexpected notification value <%08x>.\r\n", getCurrTime(),recvNotification));
        vTaskDelete(NULL);
    } else {
        FreeRTOS_printf(("%s <NTK-READY>\r\n",getCurrTime()));
        // For compliance with BESSPIN tool
        FreeRTOS_printf(("<NTK-READY>\r\n"));
    }
    
    /* Initialize mutex */
    data_mutex = xSemaphoreCreateMutex();
    configASSERT(data_mutex != NULL);

    /* Camera is not connected, don't use */
    camera_ok = FALSE;

    /* Transmission is OK */
    transmission_ok = TRUE;

    /* Create the tasks */
    funcReturn = xTaskCreate(prvInfoTask, "prvInfoTask", INFOTASK_STACK_SIZE, NULL, INFOTASK_PRIORITY, NULL);
    funcReturn &= xTaskCreate(prvSensorTask, "prvSensorTask", SENSORTASK_STACK_SIZE, NULL, SENSORTASK_PRIORITY, &xSensorTask);
    funcReturn &= xTaskCreate(prvCanRxTask, "prvCanRxTask", CAN_RX_STACK_SIZE, NULL, CAN_RX_TASK_PRIORITY, &xCanTask);
    funcReturn &= xTaskCreate(prvIPRestartHandlerTask, "prvIPRestartTask", IP_RESTART_STACK_SIZE, NULL, IP_RESTART_TASK_PRIORITY, &xIPRestartHandlerTask);
    //funcReturn &= xTaskCreate(prvHackTask, "prvHackTask", HACK_STACK_SIZE, NULL, HACK_TASK_PRIORITY, NULL);
    //funcReturn &= xTaskCreate(prvTestTask, "prvTestTask", TEST_STACK_SIZE, NULL, TEST_TASK_PRIORITY, NULL);

    if (funcReturn == pdPASS) {
        FreeRTOS_printf (("%s (Info)~  prvMainTask: Created all app tasks successfully.\r\n", getCurrTime()));
    } else {
        FreeRTOS_printf (("%s (Error)~  prvMainTask: Failed to create the app tasks.\r\n", getCurrTime()));
    }

    vTaskDelete(NULL);
}

static void prvInfoTask(void *pvParameters)
{
    (void)pvParameters;
    int16_t local_throttle, local_brake;
    int16_t local_throttle_raw, local_brake_raw;
    uint8_t local_gear;
    uint32_t hz_sensor_task_old = 0;

    FreeRTOS_printf(("%s Starting prvInfoTask\r\n", getCurrTime()));

    for (;;)
    {
        FreeRTOS_printf(("prvInfoTask loop begin\r\n"));

        /* Copy data over */
        if (xSemaphoreTake(data_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
        {
            local_throttle_raw = throttle_raw;
            local_brake_raw = brake_raw;
            local_throttle = throttle;
            local_brake = brake;
            local_gear = gear;
            xSemaphoreGive(data_mutex);
        }
        /* Sensor info */
        FreeRTOS_printf(("%s (prvInfoTask:raw) throttle: %d, brake: %d\r\n", getCurrTime(), local_throttle_raw, local_brake_raw));
        FreeRTOS_printf(("%s (prvInfoTask:scaled) Gear: %c, throttle: %u, brake: %u\r\n", getCurrTime(), local_gear, local_throttle, local_brake));
        FreeRTOS_printf(("%s (prvInfoTask:hz) prvSensorTask: %u[Hz]\r\n", getCurrTime(), hz_sensor_task - hz_sensor_task_old));
        hz_sensor_task_old = hz_sensor_task;

        /* IIC bus info */
#if IIC0_PRINT_STATS && BSP_USE_IIC0
        iic0_print_stats();
#endif

        if (camera_ok)
        {
            FreeRTOS_printf(("%s (prvInfoTask:LKAS) Camera OK: %d, steering_assist: %d\r\n", getCurrTime(), camera_ok, steering_assist));
        }

        vTaskDelay(INFOTASK_LOOP_DELAY_MS);
    }
}

static void prvSocketCreateSensorIP() {

    xSensorClientSocket = FreeRTOS_socket(FREERTOS_AF_INET, FREERTOS_SOCK_DGRAM, FREERTOS_IPPROTO_UDP);
    configASSERT(xSensorClientSocket != FREERTOS_INVALID_SOCKET);

    FreeRTOS_printf(("%s xSensorClientSocket socket connected\r\n", getCurrTime()));
}

/**
 * Read and update gear values
 */
static void prvSensorTask(void *pvParameters)
{
    (void)pvParameters;

    int returnval;
    struct freertos_sockaddr xDestinationAddress;

    // Broadcast address
    xDestinationAddress.sin_addr = FreeRTOS_inet_addr(CYBERPHYS_BROADCAST_ADDR);
    xDestinationAddress.sin_port = FreeRTOS_htons((uint16_t)CAN_PORT);

    FreeRTOS_printf(("%s Starting prvSensorTask\r\n", getCurrTime()));

    // Create a socket for the sensor
    prvSocketCreateSensorIP();

    hz_sensor_task = 0;
    uint8_t data[5];
    uint8_t tmp_var;

    // Gear variables
    uint8_t tmp_gear;

    // Throttle variables
    int16_t tmp_throttle;
    hz_sensor_throttle_task = 0;
    throttle_gain = THROTTLE_GAIN;
    throttle_min = THROTTLE_MIN;
    throttle_max = THROTTLE_MAX;

    // Brake variables
    int16_t tmp_brake;
    brake_gain = BRAKE_GAIN;
    brake_min = BRAKE_MIN;
    brake_max = BRAKE_MAX;

    int err_cnt = 0;

    for (;;)
    {
        #if BSP_USE_IIC0
            returnval = iic_receive(&Iic0, TEENSY_I2C_ADDRESS, data, 5);
        #else
            /* Default to neutral */
            tmp_gear = 'N';
            returnval = 1;
        #endif

        vTaskDelay(pdMS_TO_TICKS(1));
        if (returnval < 1) {
            FreeRTOS_printf(("%s (prvSensorTask) iic_receive error: %i\r\n", getCurrTime(), returnval));
            vTaskDelay(pdMS_TO_TICKS(100));
            err_cnt++;
            #if BSP_USE_IIC0
            if (err_cnt >= IIC_RESET_ERROR_THRESHOLD) {
                FreeRTOS_printf(("%s (prvSensorTask) err_cnt == %i, resetting!\r\n", getCurrTime(), err_cnt));
                iic0_master_reset();
                err_cnt = 0;
            }
            #endif
            continue;
        }

        /* Is transmission OK? */
        if (transmission_ok)
        {
            // data[4] = gear
            switch (data[4])
            {
            case 0x28:
                tmp_gear = 'P';
                break;
            case 0x27:
                tmp_gear = 'R';
                break;
            case 0x26:
                tmp_gear = 'N';
                break;
            case 0x25:
                tmp_gear = 'D';
                break;
            default:
                FreeRTOS_printf(("%s (prvSensorTask) unknown gear value: %c\r\n", getCurrTime(), data[4]));
                break;
            }
        } else {
            /* Default to neutral */
            tmp_gear = 'N';
        }

        /* Send gear */
        if (send_can_message(xSensorClientSocket, &xDestinationAddress, CAN_ID_GEAR, (void *)&tmp_gear, sizeof(tmp_gear)) != SUCCESS)
        {
            FreeRTOS_printf(("%s (prvSensorTask) send gear failed\r\n", getCurrTime()));
        } else {
            FreeRTOS_printf(("%s (prvSensorTask) send gear succeed\r\n", getCurrTime()));
        }

        /* Process throttle */
        // data[0,1] = throttle_raw
        throttle_raw = (int16_t)(data[1] << 8 | data[0]);
        tmp_throttle = max(throttle_raw - throttle_min, 0); // remove offset
        tmp_throttle = tmp_throttle * throttle_gain / (throttle_max - throttle_min);
        tmp_throttle = min(max(tmp_throttle, 0), 100);
        tmp_var = (uint8_t)tmp_throttle;

        /* Send throttle */
        if (send_can_message(xSensorClientSocket, &xDestinationAddress, CAN_ID_THROTTLE_INPUT, (void *)&tmp_var, sizeof(tmp_var)) != SUCCESS)
        {
            FreeRTOS_printf(("%s (prvSensorTask) send throttle failed\r\n", getCurrTime()));
        } else {
            FreeRTOS_printf(("%s (prvSensorTask) send throttle succeed\r\n", getCurrTime()));
        }

        /* Request brake */
        // data[2,3] = brake_raw
        brake_raw = (int16_t)(data[3] << 8 | data[2]);
        tmp_brake = max(brake_max - brake_raw, 0); // reverse brake
        tmp_brake = tmp_brake * brake_gain / (brake_max - brake_min);
        tmp_brake = min(max(tmp_brake, 0), 100);
        tmp_var = (uint8_t)tmp_brake;

        /* Send brake */
        if (send_can_message(xSensorClientSocket, &xDestinationAddress, CAN_ID_BRAKE_INPUT, (void *)&tmp_var, sizeof(tmp_var)) != SUCCESS)
        {
            FreeRTOS_printf(("%s (prvSensorTask) send brake failed\r\n", getCurrTime()));
        } else {
            FreeRTOS_printf(("%s (prvSensorTask) send brake succeed\r\n", getCurrTime()));
        }

        if (xSemaphoreTake(data_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
        {
            gear = (uint8_t)tmp_gear;
            throttle = (uint8_t)tmp_throttle;
            brake = (uint8_t)tmp_brake;
            xSemaphoreGive(data_mutex);
        }

        if (camera_ok)
        {
            /* Steering assist */
            if (send_can_message(xSensorClientSocket, &xDestinationAddress, CAN_ID_STEERING_INPUT, (void *)&steering_assist, sizeof(steering_assist)) != SUCCESS)
            {
                FreeRTOS_printf(("%s (prvSensorTask) send steering_assist failed\r\n", getCurrTime()));
            }
        }

        /* Increment only *after* a successful run */
        hz_sensor_task++;
        vTaskDelay(SENSOR_LOOP_DELAY_MS);
    }
}


static void prvSocketCreateCanRx(void) {
    uint32_t ulIPAddress;
    struct freertos_sockaddr xBindAddress;
    char cBuffer[16];

    xDestinationAddress.sin_addr = FreeRTOS_inet_addr(CYBERPHYS_BROADCAST_ADDR);
    xDestinationAddress.sin_port = FreeRTOS_htons((uint16_t)CAN_PORT);
    xCanRxClientSocket = FreeRTOS_socket(FREERTOS_AF_INET, FREERTOS_SOCK_DGRAM, FREERTOS_IPPROTO_UDP);
    configASSERT(xCanRxClientSocket != FREERTOS_INVALID_SOCKET);

    /* Attempt to open the socket. */
    xCanRxListeningSocket = FreeRTOS_socket(FREERTOS_AF_INET, FREERTOS_SOCK_DGRAM, FREERTOS_IPPROTO_UDP);
    configASSERT(xCanRxListeningSocket != FREERTOS_INVALID_SOCKET);

    /* Attempt to open the socket. */
    xCanRxListeningSocket = FreeRTOS_socket(FREERTOS_AF_INET, FREERTOS_SOCK_DGRAM, FREERTOS_IPPROTO_UDP);
    configASSERT(xCanRxListeningSocket != FREERTOS_INVALID_SOCKET);

    /* This test receives data sent from a different port on the same IP address.
	Obtain the nodes IP address.  Configure the freertos_sockaddr structure with
	the address being bound to.  The strange casting is to try and remove
	compiler warnings on 32 bit machines.  Note that this task is only created
	after the network is up, so the IP address is valid here. */
    FreeRTOS_GetAddressConfiguration(&ulIPAddress, NULL, NULL, NULL);
    xBindAddress.sin_addr = ulIPAddress;
    xBindAddress.sin_port = FreeRTOS_htons((uint16_t)CAN_PORT);

    /* Bind the socket to the port that the client task will send to. */
    FreeRTOS_bind(xCanRxListeningSocket, &xBindAddress, sizeof(xBindAddress));

    FreeRTOS_inet_ntoa(xBindAddress.sin_addr, cBuffer);
    FreeRTOS_printf(("%s xCanRxListeningSocket bound to addr %s:%u\r\n", getCurrTime(), cBuffer, (uint16_t)CAN_PORT));
}

/* Called by FreeRTOS+TCP when the network connects or disconnects.  Disconnect
events are only received if implemented in the MAC driver. */
void vApplicationIPNetworkEventHook(eIPCallbackEvent_t eNetworkEvent)
{
    uint32_t ulIPAddress, ulNetMask, ulGatewayAddress, ulDNSServerAddress;
    char cBuffer[16];
    BaseType_t funcReturn;

    /* If the network has just come up...*/
    if (eNetworkEvent == eNetworkUp)
    {
        /* Print out the network configuration, which may have come from a DHCP
        server. */
        FreeRTOS_GetAddressConfiguration(&ulIPAddress, &ulNetMask, &ulGatewayAddress, &ulDNSServerAddress);
        FreeRTOS_inet_ntoa(ulIPAddress, cBuffer);
        FreeRTOS_printf(("\r\n\r\n%s IP Address: %s\r\n", getCurrTime(), cBuffer));

        FreeRTOS_inet_ntoa(ulNetMask, cBuffer);
        FreeRTOS_printf(("%s Subnet Mask: %s\r\n", getCurrTime(), cBuffer));

        FreeRTOS_inet_ntoa(ulGatewayAddress, cBuffer);
        FreeRTOS_printf(("%s Gateway Address: %s\r\n", getCurrTime(), cBuffer));

        FreeRTOS_inet_ntoa(ulDNSServerAddress, cBuffer);
        FreeRTOS_printf(("%s DNS Server Address: %s\r\n\r\n\r\n", getCurrTime(), cBuffer));

        // notify main
        if (xMainTask == NULL) {
            FreeRTOS_printf(("%s (Error)~  NtkHook: Unable to get the handle of <prvMainTask>.\r\n", getCurrTime()));
        }

        if (xSensorTask && xCanTask) {
            BaseType_t pxHigherPriorityTaskWoken = pdFALSE;
            vTaskNotifyGiveFromISR( xIPRestartHandlerTask, &pxHigherPriorityTaskWoken );
        } else {
            funcReturn = xTaskNotify( xMainTask, NOTIFY_SUCCESS_NTK ,eSetBits);
            if (funcReturn != pdPASS) {
                FreeRTOS_printf(("%s (Error)~  NtkHook: Failed to notify <prvMainTask>!\r\n", getCurrTime()));
            }
        }

    } else if (eNetworkEvent == eNetworkDown) {
        vTaskSuspend(xSensorTask);
        vTaskSuspend(xCanTask);
        startNetwork();
    }
}
/*-----------------------------------------------------------*/

static void prvCanRxTask(void *pvParameters)
{
    (void)pvParameters;
    char cBuffer[16];
    struct freertos_sockaddr xClient;
    size_t msg_len;
    canid_t can_id;
    uint32_t request_id;
    uint32_t target_id;

    FreeRTOS_printf(("%s Starting prvCanRxTask\r\n", getCurrTime()));

    /* Create socket for responding to requests */
    prvSocketCreateCanRx();
    /*  End of the socket for respondnig to requests */

    /* Set target ID */
    target_id = FreeRTOS_htonl(FreeRTOS_GetIPAddress());
    printf("j1939 buf addr: %p\n",j1939_rx_buf);
    for (;;)
    {
        printf("prvCanTask og ret addr: %p\n", __builtin_return_address(0));

        uint8_t res = process_j1939(xCanRxListeningSocket, &xClient, &msg_len, &can_id, (uint8_t*)&request_id);
        if (res == SUCCESS)
        {
            printf("prvCanTask after overflow ret addr: %p\n", __builtin_return_address(0));
            printf("j1939 buf content: %s\n",j1939_rx_buf);

            switch (can_id)
            {
                case CAN_ID_HEARTBEAT_REQ:
                    /* received data are in network endian, simply copy over as we do not need to process them */
                    FreeRTOS_printf(("%s (prvCanRxTask) Replying to heartbeat #%u\r\n", getCurrTime(), FreeRTOS_ntohl(request_id)));
                    /* Copy target ID (stored in network byte order) */
                    memcpy(&cBuffer[0], &target_id, sizeof(uint32_t));
                    /* Copy request ID (already in network byte order) */
                    memcpy(&cBuffer[4], &request_id, sizeof(uint32_t));
                    res = send_can_message(xCanRxClientSocket, &xDestinationAddress, CAN_ID_HEARTBEAT_ACK,
                        (void *)cBuffer, BYTE_LENGTH_HEARTBEAT_ACK);
                    if ( res != SUCCESS)
                    {
                        FreeRTOS_printf(("%s (prvCanRxTask) Replying to heartbeat failed with %u\r\n", getCurrTime(), res));
                    }
                    break;
            default:
                break;
            }
        }
        else
        {
            FreeRTOS_printf(("%s (prvCanRxTask) recv_can_message returned %u\r\n", getCurrTime(), res));
        }
    }
}

uint8_t process_j1939(Socket_t xListeningSocket, struct freertos_sockaddr *xClient, size_t *msg_len, canid_t *can_id, uint8_t *msg_buf)
{
    FreeRTOS_printf(("process_j1939\r\n"));
    char msg[100];
    //printf("process_j1939 ret addr before overflow %p\n", __builtin_return_address(0));
    /* Receive a message that can overflow the msg buffer */
    uint8_t res = recv_can_message(xListeningSocket, xClient, can_id, msg, msg_len);
    
    if (res == SUCCESS) {
        /* Check CAN ID */
        //printf("process_j1939 after overflow ret addr: %p\n", __builtin_return_address(0));
        if (*can_id == PGN_BAM)
        {
            /* Copy message over to a persistent buffer */
            memcpy(j1939_rx_buf, msg, *msg_len);
        } else {
            /* All other messages are pass-through */
            memcpy(msg_buf, msg, min(sizeof(uint32_t), *msg_len));
        }
    }
    return res;
}
