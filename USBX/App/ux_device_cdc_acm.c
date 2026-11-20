/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    ux_device_cdc_acm.c
  * @author  MCD Application Team
  * @brief   USBX Device CDC ACM applicative source file
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2020-2021 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "ux_device_cdc_acm.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "usb_device.h"

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN PV */
UX_SLAVE_CLASS_CDC_ACM * volatile diagnostic_cdc;
static UINT observed_dtr;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/**
  * @brief  USBD_CDC_ACM_Activate
  *         This function is called when insertion of a CDC ACM device.
  * @param  cdc_acm_instance: Pointer to the cdc acm class instance.
  * @retval none
  */
VOID USBD_CDC_ACM_Activate(VOID *cdc_acm_instance)
{
  /* USER CODE BEGIN USBD_CDC_ACM_Activate */
  usb_connection_changed();
  diagnostic_cdc = (UX_SLAVE_CLASS_CDC_ACM *)cdc_acm_instance;
  observed_dtr = diagnostic_cdc->ux_slave_class_cdc_acm_data_dtr_state != 0;
  /* USER CODE END USBD_CDC_ACM_Activate */

  return;
}

/**
  * @brief  USBD_CDC_ACM_Deactivate
  *         This function is called when extraction of a CDC ACM device.
  * @param  cdc_acm_instance: Pointer to the cdc acm class instance.
  * @retval none
  */
VOID USBD_CDC_ACM_Deactivate(VOID *cdc_acm_instance)
{
  /* USER CODE BEGIN USBD_CDC_ACM_Deactivate */
  UX_PARAMETER_NOT_USED(cdc_acm_instance);
  diagnostic_cdc = UX_NULL;
  observed_dtr = 0;
  usb_connection_changed();
  /* USER CODE END USBD_CDC_ACM_Deactivate */

  return;
}

/**
  * @brief  USBD_CDC_ACM_ParameterChange
  *         This function is invoked to manage the CDC ACM class requests.
  * @param  cdc_acm_instance: Pointer to the cdc acm class instance.
  * @retval none
  */
VOID USBD_CDC_ACM_ParameterChange(VOID *cdc_acm_instance)
{
  /* USER CODE BEGIN USBD_CDC_ACM_ParameterChange */
  UX_SLAVE_CLASS_CDC_ACM *cdc = cdc_acm_instance;
  UINT dtr = cdc->ux_slave_class_cdc_acm_data_dtr_state != 0;
  /* Hosts repeat line coding/control requests while opening a port. Only
   * DTR edges delimit this protocol; baud and RTS do not affect USB bytes. */
  if (dtr == observed_dtr) return;
  observed_dtr = dtr;
  /* Abort an old reader even if close raced with its blocking read call. */
  usb_connection_changed();
  usb_device_abort(cdc, UX_SLAVE_CLASS_CDC_ACM_ENDPOINT_RCV);
  usb_device_abort(cdc, UX_SLAVE_CLASS_CDC_ACM_ENDPOINT_XMIT);
  /* USER CODE END USBD_CDC_ACM_ParameterChange */

  return;
}

/* USER CODE BEGIN 1 */

/* USER CODE END 1 */
