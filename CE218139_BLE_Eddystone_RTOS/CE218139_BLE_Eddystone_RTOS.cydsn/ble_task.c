/******************************************************************************
* File Name: task_ble.c
*
* Version: 1.00
*
* Description: This file contains the task that handles custom BLE services
*
* Related Document: CE220331_BLE_UI_RTOS.pdf
*
* Hardware Dependency: CY8CKIT-062-BLE PSoC 6 BLE Pioneer Kit
*
******************************************************************************
* Copyright (2017), Cypress Semiconductor Corporation.
******************************************************************************
* This software, including source code, documentation and related materials
* ("Software") is owned by Cypress Semiconductor Corporation (Cypress) and is
* protected by and subject to worldwide patent protection (United States and 
* foreign), United States copyright laws and international treaty provisions. 
* Cypress hereby grants to licensee a personal, non-exclusive, non-transferable
* license to copy, use, modify, create derivative works of, and compile the 
* Cypress source code and derivative works for the sole purpose of creating 
* custom software in support of licensee product, such licensee product to be
* used only in conjunction with Cypress's integrated circuit as specified in the
* applicable agreement. Any reproduction, modification, translation, compilation,
* or representation of this Software except as specified above is prohibited 
* without the express written permission of Cypress.
* 
* Disclaimer: THIS SOFTWARE IS PROVIDED AS-IS, WITH NO WARRANTY OF ANY KIND, 
* EXPRESS OR IMPLIED, INCLUDING, BUT NOT LIMITED TO, NONINFRINGEMENT, IMPLIED 
* WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.
* Cypress reserves the right to make changes to the Software without notice. 
* Cypress does not assume any liability arising out of the application or use
* of Software or any product or circuit described in the Software. Cypress does
* not authorize its products for use as critical components in any products 
* where a malfunction or failure may reasonably be expected to result in 
* significant injury or death ("ACTIVE Risk Product"). By including Cypress's 
* product in a ACTIVE Risk Product, the manufacturer of such system or application
* assumes all risk of such use and in doing so indemnifies Cypress against all
* liability. Use of this Software may be limited by and subject to the applicable
* Cypress software license agreement.
*****************************************************************************/
/*******************************************************************************
* This file contains the task that handles custom BLE services, which includes 
* the CapSense Slider service, CapSense button service and the RGB LED service
*******************************************************************************/

/* Header file includes */
#include <math.h>
#include "ble_task.h"
#include "status_led_task.h"
#include "eddystone_config.h"
#include "uart_debug.h"
#include "task.h"
#include "queue.h"
#include "timers.h"    

/* Macros to access BLE advertisement data and settings */
#define AdvertisementData       cy_ble_discoveryData\
                                [CY_BLE_BROADCASTER_CONFIGURATION_0_INDEX]\
                                .advData
                                
#define AdvertisementSize       cy_ble_discoveryData\
                                [CY_BLE_BROADCASTER_CONFIGURATION_0_INDEX]\
                                .advDataLen
                                
#define AdvertisementTimeOut    cy_ble_discoveryModeInfo\
                                [CY_BLE_BROADCASTER_CONFIGURATION_0_INDEX]\
                                .advTo
                                
/* Number of packets broadcasted during a UID/URL + TLM advertisement.
   This is equal to the total time out in seconds / 100 milliseconds or
   total time out *10 */                      
#define PACKETS_PER_BROADCAST   ((APP_UID_URL_TIMEOUT \
                                 +APP_TLM_TIMEOUT)*10)

/* Variables that store constant URL, UID and TLM packet data. To change 
   the Eddystone packet settings, see the header file eddystone_config.h */
uint8_t const solicitationData[] = SERVICE_SOLICITATION;
uint8_t const serviceDataUID[]   = UID_SERVICE_DATA;
uint8_t const nameSpaceID[]      = NAME_SPACE_ID;
uint8_t const instanceID[]       = INSTANCE_ID;
uint8_t const packetDataURL[]    = URL_PACKET_DATA;
uint8_t const serviceDataTLM[]   = TLM_SERVICE_DATA;
                                
/* Data type that stores advertisement packet count since power-up 
   or reset  */
typedef union
{
    /* 32-bit packet count for mathematical operations */
    uint32_t    count;
    /* Same packet count segmented into four 8-bit fields for copying 
       to the Eddystone frame */
    uint8_t     countByte[4u]; 
}   packet_count_t;

/* Data type that stores the time elapsed since power-up or reset */
typedef union
{
    /* 32-bit second count for mathematical operations */
    uint32_t    count;
    /* Same second count segmented into four 8-bit fields for copying 
       to the Eddystone frame */
    uint8_t     countByte[4u];   
}   second_count_t;

/* Enumerated data type for core Eddystone roles */
typedef enum
{
    /* Eddystone UID adv */
    EDDYSTONE_UID,
    /* Eddystone URL adv */
    EDDYSTONE_URL,
    /* Eddystone TLM adv */
    EDDYSTONE_TLM
}   eddystone_role_t;

/* Variable that stores the current role  (UID/URL or TLM). To change 
   the Eddystone packet settings, see the header file eddystone_config.h */
eddystone_role_t beaconCurrentRole = EDDYSTONE_IMPLEMENTATION;

/* variable that stores advertisement packet count since power-up or reset */
packet_count_t   packetCount;

/* Variable  that stores the time elapsed in seconds since power-up or reset */
second_count_t seconds;

/*  These static functions are not available outside this file. 
    See the respective function definitions for more details */
void static BleControllerInterruptEventHandler(void);
void static StackEventHandler(uint32_t eventType, void *eventParam);
void static ConfigureAdvPacket(void);
void static SecondsTimerStart(void);

/* Semaphore that unblocks the BLE Task */
SemaphoreHandle_t  bleSemaphore; 

/* Timer handle */
TimerHandle_t xTimer_Seconds;
                    
/*******************************************************************************
* Function Name: void Task_Ble(void *pvParameters)
********************************************************************************
* Summary:
*  Task that processes the BLE state and events, and then commands other tasks 
*  to take an action based on the current BLE state and data received over BLE  
*
* Parameters:
*  void *pvParameters : Task parameter defined during task creation (unused)                            
*
* Return:
*  void
*
*******************************************************************************/
void Task_Ble(void *pvParameters)
{
    /* Variable used to store the return values of BLE APIs */
    cy_en_ble_api_result_t bleApiResult;
    
    /* Variable used to store the return values of RTOS APIs */
    BaseType_t rtosApiResult;
    
    /* Remove warning for unused parameter */
    (void)pvParameters;

    /* Start the BLE component and register the stack event handler */
    bleApiResult = Cy_BLE_Start(StackEventHandler);
    
    /* Check if the operation was successful */
    if(bleApiResult == CY_BLE_SUCCESS)
    {
        DebugPrintf("Success  : BLE - Stack initialization", 0u);
        
        /* Register the BLE controller (Cortex-M0+) interrupt event handler  */
        Cy_BLE_RegisterAppHostCallback(BleControllerInterruptEventHandler); 
        /* Process BLE events to turn the stack on */
        Cy_BLE_ProcessEvents();
    }
    else
    {
        DebugPrintf("Failure! : BLE  - Stack initialization. Error Code:",
                    bleApiResult);
    }

    /* Repeatedly running part of the task */
    for(;;)
    {       
        /* Block until a BLE semaphore has been received */
        rtosApiResult =  xSemaphoreTake(bleSemaphore,portMAX_DELAY);

        /* Semaphore has been received */
        if(rtosApiResult == pdTRUE)
        {
            /* Process event callback to handle BLE events. The events generated 
            and used for this application are inside the 'StackEventHandler' 
            routine */
            Cy_BLE_ProcessEvents();
        }
        /* Task has timed out and received no semaphores during an interval of 
            portMAXDELAY ticks */
        else
        {               
            DebugPrintf("Warning! : BLE - Task Timed out ", 0u);
        }  
    }
}

/*******************************************************************************
* Function Name: void static StackEventHandler(uint32_t event, void *eventParam)
********************************************************************************
* Summary:
*  Call back event function to handle various events from the BLE stack. Note 
*  that Cortex M4 only handles the BLE host portion of the stack, while 
*  Cortex M0+ handles the BLE controller portion. 
*
* Parameters:
*  event        :	BLE event occurred
*  eventParam   :	Pointer to the value of event specific parameters
*
* Return:
*  void
*
*******************************************************************************/
void static StackEventHandler(uint32_t eventType, void *eventParam)
{ 
    /* Variable used to store the return values of BLE APIs */
    cy_en_ble_api_result_t bleApiResult;

    (void)(eventParam);
    
    /* Take an action based on the current event */
    switch ((cy_en_ble_event_t)eventType)
    {
        /*~~~~~~~~~~~~~~~~~~~~~~ GENERAL  EVENTS ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/
        
        /* This event is received when the BLE stack is Started */
        case CY_BLE_EVT_STACK_ON:
        
            DebugPrintf("Info     : BLE - Stack on", 0u);
            
            /* Note: To change the Eddystone packet settings, see the  
               header file eddystone_config.h */
            /* Check if the selected Eddystone implementation is valid.
               Halt the CPU otherwise */
            if ((EDDYSTONE_IMPLEMENTATION != EDDYSTONE_UID)&&
                (EDDYSTONE_IMPLEMENTATION != EDDYSTONE_URL))
            {
                CY_ASSERT(0u);
            }
            else
            {
                /* Start with the selected implementation */
                beaconCurrentRole = EDDYSTONE_IMPLEMENTATION;
            }
            
            /* Reset advertisement packet count and start tracking time */
            packetCount.count = 0x00000000u;
            seconds.count     = 0x00000000u; 
            
            SecondsTimerStart();
            
            /* Configure Eddystone packets for the selected implementation */
            ConfigureAdvPacket();

            /* Start advertisement */
            bleApiResult = Cy_BLE_GAPP_StartAdvertisement(CY_BLE_ADVERTISING_CUSTOM,
                            CY_BLE_BROADCASTER_CONFIGURATION_0_INDEX);

            if(bleApiResult == CY_BLE_SUCCESS )
            {
                DebugPrintf("Success  : BLE - Advertisement API", 0u);
                Cy_BLE_ProcessEvents();
            }
            else
            {
                DebugPrintf("Failure! : BLE - Advertisement API, Error code:"
                            , bleApiResult);
            }  
            break;
            
        /* This event is received when there is a timeout */
        case CY_BLE_EVT_TIMEOUT:
            
            DebugPrintf("Info     : BLE - Event timeout", 0u);
            break;
        
        /* This event indicates that some internal HW error has occurred */    
	    case CY_BLE_EVT_HARDWARE_ERROR:
            
            DebugPrintf("Error!   : BLE - Internal hardware error", 0u);
		    break;
               
        /*~~~~~~~~~~~~~~~~~~~~~~~~~~~ GAP EVENTS ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/
        
        /* This event indicates peripheral device has started/stopped
           advertising */
        case CY_BLE_EVT_GAPP_ADVERTISEMENT_START_STOP:
            
                DebugPrintf("Info     : BLE - Advertisement start/stop event"
                            ,0u);
                            /* Advertisement timeout */
            if (Cy_BLE_GetAdvertisementState() != CY_BLE_ADV_STATE_ADVERTISING)
            {
                /* On advertisement timeout, switch between URI/URL and
                *  TLM packets. */
                if( (beaconCurrentRole == EDDYSTONE_UID) ||
                    (beaconCurrentRole == EDDYSTONE_URL) )
                {
                    beaconCurrentRole = EDDYSTONE_TLM;
                }
                else if(beaconCurrentRole == EDDYSTONE_TLM)
                {
                    beaconCurrentRole = EDDYSTONE_IMPLEMENTATION;
                }
                
                /* Configure Eddystone packets for the selected implementation */
                ConfigureAdvPacket();
                
                /* Restart advertisement */
                bleApiResult = Cy_BLE_GAPP_StartAdvertisement(CY_BLE_ADVERTISING_CUSTOM,
                                CY_BLE_BROADCASTER_CONFIGURATION_0_INDEX);
                
            if(bleApiResult == CY_BLE_SUCCESS )
            {
                DebugPrintf("Success  : BLE - Advertisement API", 0u);
                Cy_BLE_ProcessEvents();
            }
            else
            {
                DebugPrintf("Failure! : BLE - Advertisement API, Error code:"
                            , bleApiResult);
            } 
            }
            /* Advertisement started */
            else
            {
                /* If the beacon role switched to UID/URL, a complete URL/UID + TLM 
                   frames have been broadcasted */
                if( (beaconCurrentRole == EDDYSTONE_UID) ||
                    (beaconCurrentRole == EDDYSTONE_URL) )
                {
                    /* Increment advertisement packet count */
                    packetCount.count+=PACKETS_PER_BROADCAST;
                }    
            }
            break;                
        
        /*~~~~~~~~~~~~~~~~~~~~~~~~~~ OTHER EVENTS ~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/ 
        
         /* See the data-type cy_en_ble_event_t to understand the event 
			occurred */
        default:
                DebugPrintf("Info     : BLE - Event code: ", eventType);
            break;
    }
} 

/*******************************************************************************
* Function Name: void ConfigureAdvPacket(void)
********************************************************************************
* Summary:
*  Function that configures Eddystone packets at run-time
*
* Parameters:
*  None
*
* Return:
*  None
*
*******************************************************************************/
void ConfigureAdvPacket(void)
{
   /* Note: To change the Eddystone packet settings, see the header file 
      eddystone_config.h */
    status_led_data_t   statusLedData;
    
    /* Variable used to store the return values of RTOS APIs */
    BaseType_t rtosApiResult;

    /* Load the service Solicitation data onto the Eddystone advertisement 
       packet */
    memcpy (&AdvertisementData[SOLICITATION_INDEX],
            &solicitationData, 
            (sizeof solicitationData));
    
    /* Check if the current Eddystone role is either UID or URL */
    if( (beaconCurrentRole == EDDYSTONE_UID) ||
        (beaconCurrentRole == EDDYSTONE_URL))
    {
        /* Turn the Red LED on and the Orange LED off */
            statusLedData.orangeLed = LED_TURN_OFF;
            statusLedData.redLed    = LED_TURN_ON;
            rtosApiResult = xQueueSend(statusLedDataQ, &statusLedData,0u);
            
            /* Check if the operation has been successful */
            if(rtosApiResult != pdTRUE)
            {
                DebugPrintf("Failure! : BLE - Sending data to Status LED queue"
                            , 0u);   
            }
        
        /* Configure the advertisement timeout */
        AdvertisementTimeOut = APP_UID_URL_TIMEOUT;

        /* If the current role is UID, load the UID specific data onto the
           Eddystone advertisement packet */
        if(beaconCurrentRole == EDDYSTONE_UID)
        {
            /* Load Service Data */
            memcpy (&AdvertisementData[UID_SERVICE_INDEX], 
                    &serviceDataUID, 
                    (sizeof serviceDataUID));
           
            /* Load Name-space ID */
            memcpy (&AdvertisementData[NAME_SPACE_INDEX],
                    &nameSpaceID,
                    (sizeof nameSpaceID));

            /* Load Instance ID  */
             memcpy (&AdvertisementData[INSTANCE_INDEX],
                     &instanceID, 
                     (sizeof instanceID));
            
            /* Load the reserved fields with default values */
            AdvertisementData[RESERVED_INDEX_0] = RESERVED_FIELD_VALUE;              
            AdvertisementData[RESERVED_INDEX_1] = RESERVED_FIELD_VALUE;              
            
            /* Configure advertisement packet length */
            AdvertisementSize = UID_PACKET_SIZE;
        }
        /* If the current role is URL, load the URL specific data onto the
           Eddystone advertisement packet */
        else if(beaconCurrentRole == EDDYSTONE_URL)
        {    
            /* Load URL data */
            memcpy (&AdvertisementData[URL_INDEX],
                     &packetDataURL, 
                     (sizeof packetDataURL));
            
            /* Configure advertisement packet length */
            AdvertisementSize = URL_PACKET_SIZE;
        }
    }
    /* If the current role is TLM, load the TLM specific data onto the
       Eddystone advertisement packet */
    else if(beaconCurrentRole == EDDYSTONE_TLM)
    {
        /* Turn the Orange LED on and the Red LED off */
        statusLedData.orangeLed = LED_TURN_ON;
        statusLedData.redLed    = LED_TURN_OFF;
        rtosApiResult = xQueueSend(statusLedDataQ, &statusLedData,0u);
        
        /* Check if the operation has been successful */
        if(rtosApiResult != pdTRUE)
        {
            DebugPrintf("Failure! : BLE - Sending data to Status LED queue"
                        , 0u);   
        }
        
        /* Configure the advertisement timeout */
        AdvertisementTimeOut = APP_TLM_TIMEOUT;

        /* Load Service Data */
        memcpy (&AdvertisementData[TLM_SERVICE_INDEX],
                &serviceDataTLM,    
                (sizeof serviceDataTLM));
        
        /* Load battery voltage in mV (1 mV per bit) */
        AdvertisementData[BATTERY_MSB_INDEX] = BATTERY_VOLTAGE_MSB;     
        AdvertisementData[BATTERY_LSB_INDEX] = BATTERY_VOLTAGE_LSB;     

        /* Load beacon temperature in Celsius (8.8 fixed point notation) */
        AdvertisementData[TEMPERATURE_MSB_INDEX] = BEACON_TEMPERATURE_MSB;     
        AdvertisementData[TEMPERATURE_LSB_INDEX] = BEACON_TEMPERATURE_LSB;     

        /* Load advertising packet count since power-up or reset */
        AdvertisementData[PACKET_COUNT_INDEX_0] = packetCount.countByte
                                                  [TLM_4B_ENDIAN_SWAP_0];     
        AdvertisementData[PACKET_COUNT_INDEX_1] = packetCount.countByte
                                                  [TLM_4B_ENDIAN_SWAP_1];     
        AdvertisementData[PACKET_COUNT_INDEX_2] = packetCount.countByte
                                                  [TLM_4B_ENDIAN_SWAP_2];     
        AdvertisementData[PACKET_COUNT_INDEX_3] = packetCount.countByte
                                                  [TLM_4B_ENDIAN_SWAP_3];     
    
        /* Load time elapsed since power-on or reboot (100 ms per bit) */
        AdvertisementData[SECONDS_INDEX_0] = seconds.countByte
                                             [TLM_4B_ENDIAN_SWAP_0];     
        AdvertisementData[SECONDS_INDEX_1] = seconds.countByte
                                             [TLM_4B_ENDIAN_SWAP_1];     
        AdvertisementData[SECONDS_INDEX_2] = seconds.countByte
                                             [TLM_4B_ENDIAN_SWAP_2];     
        AdvertisementData[SECONDS_INDEX_3] = seconds.countByte
                                             [TLM_4B_ENDIAN_SWAP_3];     
                
        /* Configure advertisement packet length */
        AdvertisementSize = TLM_PACKET_SIZE;
    } 
}


/*******************************************************************************
* Function Name: void static BleControllerInterruptEventHandler (void)
********************************************************************************
* Summary:
*  Call back event function to handle interrupts from BLE Controller
*  (Cortex M0+)
*
* Parameters:
*  None
*
* Return:
*  void
*
*******************************************************************************/
void static BleControllerInterruptEventHandler(void)
{
    BaseType_t xHigherPriorityTaskWoken;
    
    /* Send semaphore to process BLE events  */
    xSemaphoreGiveFromISR(bleSemaphore, &xHigherPriorityTaskWoken); 

    portYIELD_FROM_ISR(xHigherPriorityTaskWoken );
}

/*******************************************************************************
* Function Name: void static BleTimerCallback(TimerHandle_t xTimer)                          
********************************************************************************
* Summary:
*  This function is called when the BLE Timer expires
*
* Parameters:
*  TimerHandle_t xTimer :  Current timer value (unused)
*
* Return:
*  void
*
*******************************************************************************/
void static SecondsTimerCallback(TimerHandle_t xTimer)
{
    /* Remove warning for unused parameter */
    (void)xTimer;
   
    /* Increment the seconds count (100 ms per bit) */
    seconds.count++;
}

/*******************************************************************************
* Function Name: void static SecondsTimerStart(void)                     
********************************************************************************
* Summary:
*  This function starts the timer that provides timing 
*
* Parameters:
*  void
*
* Return:
*  void
*
*******************************************************************************/
void static SecondsTimerStart(void)
{
    /* Variable used to store the return values of RTOS APIs */
    BaseType_t rtosApiResult;

    /* Create an RTOS timer with 100ms interval */
    xTimer_Seconds =  xTimerCreate ("Seconds Timer", (pdMS_TO_TICKS(100u)), 
                                    pdTRUE, NULL, SecondsTimerCallback);
    
    /* Make sure that timer handle is valid */
    if (xTimer_Seconds != NULL)
    {
        /* Start the timer */
        rtosApiResult = xTimerStart(xTimer_Seconds, 0u);
        
        /* Check if the operation has been successful */
        if(rtosApiResult != pdPASS)
        {
            DebugPrintf("Failure! : BLE  - Seconds Timer initialization", 0u);    
        }
    }
    else
    {
        DebugPrintf("Failure! : BLE  - Seconds Timer creation", 0u); 
    }
}

/* [] END OF FILE */
