/*
    ChibiOS - Copyright (C) 2006..2018 Giovanni Di Sirio

    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

        http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
*/

/**
 * @file    hal_usb_lld.c
 * @brief   PLATFORM USB subsystem low level driver source.
 *
 * @addtogroup USB
 * @{
 */

#include <SN32F240B.h>
#include <string.h>
#include "hal.h"
#include "usb.h"
#include "usbhw.h"
#include "usbuser.h"
#include "usbepfunc.h"
#include "usbram.h"
#include "usbdesc.h"

#if (HAL_USE_USB == TRUE) || defined(__DOXYGEN__)

/*===========================================================================*/
/* Driver local definitions.                                                 */
/*===========================================================================*/
#define SN32_USB_IRQ_VECTOR    Vector44
#define SN32_USB_PMA_SIZE      256

/*===========================================================================*/
/* Driver exported variables.                                                */
/*===========================================================================*/

/**
 * @brief   USB1 driver identifier.
 */
#if (PLATFORM_USB_USE_USB1 == TRUE) || defined(__DOXYGEN__)
USBDriver USBD1;
#endif

/*===========================================================================*/
/* Driver local variables and types.                                         */
/*===========================================================================*/

static int address;

/**
 * @brief   EP0 state.
 * @note    It is an union because IN and OUT endpoints are never used at the
 *          same time for EP0.
 */
static union {
  /**
   * @brief   IN EP0 state.
   */
  USBInEndpointState in;
  /**
   * @brief   OUT EP0 state.
   */
  USBOutEndpointState out;
} ep0_state;

/**
 * @brief   EP0 initialization structure.
 */
static const USBEndpointConfig ep0config = {
  USB_ENDPOINT_TYPE_CONTROL,
  _usb_ep0setup,
  _usb_ep0in,
  _usb_ep0out,
  0x40,
  0x40,
  &ep0_state.in,
  &ep0_state.out
};

/*===========================================================================*/
/* Driver local variables and types.                                         */
/*===========================================================================*/

/*===========================================================================*/
/* Driver local functions.                                                   */
/*===========================================================================*/

static void sn32_usb_read_fifo(usbep_t ep, uint8_t *buf, size_t sz) {
    size_t off;
    size_t chunk;
    uint32_t data;

    if (ep == 0) {
        off = 0;

        while (off != sz) {
            chunk = 4;
            if (off + chunk > sz)
                chunk = sz - off;

            SN_USB->RWADDR = off;
            SN_USB->RWSTATUS = 0x02;
            while (SN_USB->RWSTATUS & 0x02);

            data = SN_USB->RWDATA;
            memcpy(buf, &data, chunk);

            off += chunk;
            buf += chunk;
        }
    } else {
        __asm__ volatile ("bkpt");
    }
}

static void sn32_usb_write_fifo(usbep_t ep, const uint8_t *buf, size_t sz) {
    size_t off;
    size_t chunk;
    uint32_t data;

    if (ep == 0) {
        off = 0;

        while (off != sz) {
            chunk = 4;
            if (off + chunk > sz)
                chunk = sz - off;

            memcpy(&data, buf, chunk);

            SN_USB->RWADDR = off;
            SN_USB->RWDATA = data;
            SN_USB->RWSTATUS = 0x01;
            while (SN_USB->RWSTATUS & 0x01);

            off += chunk;
            buf += chunk;
        }
    } else {
        off = 0;

        while (off != sz) {
            chunk = 4;
            if (off + chunk > sz)
                chunk = sz - off;

            memcpy(&data, buf, chunk);

            SN_USB->RWADDR2 = off + ep * 0x40;
            SN_USB->RWDATA2 = data;
            SN_USB->RWSTATUS2 = 0x01;
            while (SN_USB->RWSTATUS2 & 0x01);

            off += chunk;
            buf += chunk;
        }
    }
}

/**
 * @brief   Reads from a dedicated packet buffer.
 *
 * @param[in] ep        endpoint number
 * @param[out] buf      buffer where to copy the packet data
 * @return              The size of the receive packet.
 *
 * @notapi
 */
static size_t usb_packet_read_to_buffer(usbep_t ep, uint8_t *buf) {
    size_t i, n;

    n = (SN_USB->EP0CTL + ep) & mskEPn_CNT; // Endpoint byte count

    i = n;

    while (i >= 2) {

        fnUSBINT_ReadFIFO(wUSB_EPnOffset[ep]);
        // USB_SRAM = wUSBINT_ReadDataBuf;
        // memcpy(buf, USB_SRAM, 4);

        uint32_t w = wUSBINT_ReadDataBuf++;
        *buf++ = (uint8_t)w;
        *buf++ = (uint8_t)(w >> 8);

        i -= 2;
    }

    if (i >= 1) {
        *buf = (uint8_t)wUSBINT_ReadDataBuf;
    }

    return n;
}

/**
 * @brief   Writes to a dedicated packet buffer.
 *
 * @param[in] ep        endpoint number
 * @param[in] buf       buffer where to fetch the packet data
 * @param[in] n         maximum number of bytes to copy. This value must
 *                      not exceed the maximum packet size for this endpoint.
 *
 * @notapi
 */
static void usb_packet_write_from_buffer(usbep_t ep, const uint8_t *buf, size_t n) {
    sn32_usb_write_fifo(ep, buf, n);
}

/**
 * @brief   USB shared ISR.
 *
 * @param[in] usbp      pointer to the @p USBDriver object
 *
 * @notapi
 */
static void usb_lld_serve_interrupt(USBDriver *usbp) {
	uint32_t iwIntFlag;

	//** Get Interrupt Status and clear immediately.
	iwIntFlag = SN_USB->INSTS;
	SN_USB->INSTSC = 0xFEFBFFFF;	//** Don't clear PRESETUP & ERR_SETUP flag

	if(iwIntFlag == 0)
	{
		//@20160902 add for EMC protection
		USB_ReturntoNormal();
		return;
	}

	/////////////////////////////////////////////////
	/* Device Status Interrupt (BusReset, Suspend) */
	/////////////////////////////////////////////////
	if (iwIntFlag & (mskBUS_RESET | mskBUS_SUSPEND | mskBUS_RESUME))
	{
		if (iwIntFlag & mskBUS_RESET)
		{
			/* BusReset */
			USB_ReturntoNormal();
			USB_ResetEvent();
            _usb_reset(usbp);
		}
		else if (iwIntFlag & mskBUS_SUSPEND)
		{
			/* Suspend */
			USB_SuspendEvent();
            _usb_suspend(usbp);
		}
		else if(iwIntFlag & mskBUS_RESUME)
		{
			/* Resume */
			USB_ReturntoNormal();
			USB_ResumeEvent();
            _usb_wakeup(usbp);
		}
	}
	/////////////////////////////////////////////////
	/* Device Status Interrupt (SETUP, IN, OUT) 	 */
	/////////////////////////////////////////////////
	else if (iwIntFlag & (mskEP0_SETUP|mskEP0_IN|mskEP0_OUT|mskEP0_IN_STALL|mskEP0_OUT_STALL))
	{
        const USBEndpointConfig *epcp = usbp->epc[0];

		if (iwIntFlag & mskEP0_SETUP)
		{
			/* SETUP */
      __USB_CLRINSTS((mskEP0_SETUP|mskEP0_PRESETUP|mskEP0_OUT_STALL|mskEP0_IN_STALL));
      //** keep EP0	NAK
      USB_EPnNak(USB_EP0);
      _usb_isr_invoke_setup_cb(usbp, 0);
      USB_EPnAck(USB_EP0, epcp->in_state->txsize);
		}
		else if (iwIntFlag & mskEP0_IN)
		{
			/* IN */
      __USB_CLRINSTS(mskEP0_IN);
      if (address) {
        SN_USB->ADDR = address;
        address = 0;
        USB_EPnStall(USB_EP0);
      }
      USB_EPnAck(USB_EP0,0);
		}
		else if (iwIntFlag & mskEP0_OUT)
		{
            // size_t cnt = SN_USB->EP0CTL_b.ENDP_CNT;
            // sn32_usb_read_fifo(0, epcp->out_state->rxbuf, cnt);
            // epcp->out_state->rxbuf += cnt;
            // epcp->out_state->

            __USB_CLRINSTS(mskEP0_OUT);
            _usb_isr_invoke_out_cb(usbp, 0);
		}
		else if (iwIntFlag & (mskEP0_IN_STALL|mskEP0_OUT_STALL))
		{
			/* EP0_IN_OUT_STALL */
			USB_EPnStall(USB_EP0);
            SN_USB->INSTSC = (mskEP0_IN_STALL|mskEP0_OUT_STALL);
		}
	}
	/////////////////////////////////////////////////
	/* Device Status Interrupt (EPnACK) 					 */
	/////////////////////////////////////////////////
	else if (iwIntFlag & (mskEP4_ACK|mskEP3_ACK|mskEP2_ACK|mskEP1_ACK))
	{
		if (iwIntFlag & mskEP1_ACK)
		{
			/* EP1 ACK */
      __USB_CLRINSTS(mskEP1_ACK);
      _usb_isr_invoke_in_cb(usbp, 1);
		}
		if (iwIntFlag & mskEP2_ACK)
		{
			/* EP2 ACK */
      __USB_CLRINSTS(mskEP2_ACK);
      _usb_isr_invoke_in_cb(usbp, 2);
		}
		if (iwIntFlag & mskEP3_ACK)
		{
			/* EP3 ACK */
      __USB_CLRINSTS(mskEP3_ACK);
      _usb_isr_invoke_in_cb(usbp, 3);
		}
		if (iwIntFlag & mskEP4_ACK)
		{
			/* EP4 ACK */
      __USB_CLRINSTS(mskEP4_ACK);
      _usb_isr_invoke_in_cb(usbp, 4);
		}
	}

	/////////////////////////////////////////////////
	/* Device Status Interrupt (EPnNAK) 					 */
	/////////////////////////////////////////////////
	else if (iwIntFlag & (mskEP4_NAK|mskEP3_NAK|mskEP2_NAK|mskEP1_NAK))
	{
		if (iwIntFlag & mskEP1_NAK)
		{
			/* EP1 NAK */
			// USB_EP1NakEvent();
            USB_EPnNak(USB_EP1);
            // usb_serve_endpoints(usbp, 1, iwIntFlag);
		}
		if (iwIntFlag & mskEP2_NAK)
		{
			/* EP2 NAK */
			// USB_EP2NakEvent();
            USB_EPnNak(USB_EP2);
            // usb_serve_endpoints(usbp, 2, iwIntFlag);
		}
		if (iwIntFlag & mskEP3_NAK)
		{
			/* EP3 NAK */
			// USB_EP3NakEvent();
            USB_EPnNak(USB_EP3);
            // usb_serve_endpoints(usbp, 3, iwIntFlag);
		}
		if (iwIntFlag & mskEP4_NAK)
		{
			/* EP4 NAK */
			// USB_EP4NakEvent();
            USB_EPnNak(USB_EP4);
            // usb_serve_endpoints(usbp, 4, iwIntFlag);
		}
	}

	/////////////////////////////////////////////////
	/* Device Status Interrupt (SOF) 							 */
	/////////////////////////////////////////////////
	if ((iwIntFlag & mskUSB_SOF) && (SN_USB->INTEN & mskUSB_SOF_IE))
	{
		/* SOF */
		USB_SOFEvent();
        _usb_isr_invoke_sof_cb(usbp);
	}
}

/*===========================================================================*/
/* Driver interrupt handlers and threads.                                    */
/*===========================================================================*/

/**
 * @brief   OTG1 interrupt handler.
 *
 * @isr
 */
OSAL_IRQ_HANDLER(SN32_USB_IRQ_VECTOR) {

  OSAL_IRQ_PROLOGUE();
  usb_lld_serve_interrupt(&USBD1);
  OSAL_IRQ_EPILOGUE();
}

/*===========================================================================*/
/* Driver exported functions.                                                */
/*===========================================================================*/

/**
 * @brief   Low level USB driver initialization.
 *
 * @notapi
 */
void usb_lld_init(void) {

#if PLATFORM_USB_USE_USB1 == TRUE
  /* Driver initialization.*/
  usbObjectInit(&USBD1);
#endif
}

/**
 * @brief   Configures and activates the USB peripheral.
 *
 * @param[in] usbp      pointer to the @p USBDriver object
 *
 * @notapi
 */
void usb_lld_start(USBDriver *usbp) {

  if (usbp->state == USB_STOP) {
    /* Enables the peripheral.*/
#if PLATFORM_USB_USE_USB1 == TRUE
    if (&USBD1 == usbp) {
        USB_Init();
        nvicEnableVector(USB_IRQn, 2);
    }
#endif
  }
  /* Configures the peripheral.*/

}

/**
 * @brief   Deactivates the USB peripheral.
 *
 * @param[in] usbp      pointer to the @p USBDriver object
 *
 * @notapi
 */
void usb_lld_stop(USBDriver *usbp) {

  if (usbp->state == USB_READY) {
    /* Resets the peripheral.*/

    /* Disables the peripheral.*/
#if PLATFORM_USB_USE_USB1 == TRUE
    if (&USBD1 == usbp) {

    }
#endif
  }
}

/**
 * @brief   USB low level reset routine.
 *
 * @param[in] usbp      pointer to the @p USBDriver object
 *
 * @notapi
 */
void usb_lld_reset(USBDriver *usbp) {

    /* Post reset initialization.*/

    /* EP0 initialization.*/
    usbp->epc[0] = &ep0config;
    usb_lld_init_endpoint(usbp, 0);
}

/**
 * @brief   Sets the USB address.
 *
 * @param[in] usbp      pointer to the @p USBDriver object
 *
 * @notapi
 */
void usb_lld_set_address(USBDriver *usbp) {
    address = usbp->address;

    USB_EPnAck(USB_EP1, 0);
    USB_EPnAck(USB_EP2, 0);
}

/**
 * @brief   Enables an endpoint.
 *
 * @param[in] usbp      pointer to the @p USBDriver object
 * @param[in] ep        endpoint number
 *
 * @notapi
 */
void usb_lld_init_endpoint(USBDriver *usbp, usbep_t ep) {
    uint32_t 	wTmp = 0;
    // sn32_usb_descriptor_t *dp;
    const USBEndpointConfig *epcp = usbp->epc[ep];

    /* IN and OUT common parameters.*/
    switch (epcp->ep_mode & USB_ENDPOINT_TYPE_MASK) {
    case USB_ENDPOINT_TYPE_CONTROL:
        // ctl = DIEPCTL_SD0PID | DIEPCTL_USBAEP | DIEPCTL_EPTYP_CTRL;
        break;
    case USB_ENDPOINT_TYPE_ISOCHRONOUS:
        // ctl = DIEPCTL_SD0PID | DIEPCTL_USBAEP | DIEPCTL_EPTYP_ISO;
        break;
    case USB_ENDPOINT_TYPE_BULK:
        // ctl = DIEPCTL_SD0PID | DIEPCTL_USBAEP | DIEPCTL_EPTYP_BULK;
        break;
    case USB_ENDPOINT_TYPE_INTERRUPT:
        // ctl = DIEPCTL_SD0PID | DIEPCTL_USBAEP | DIEPCTL_EPTYP_INTR;
        break;
    default:
        return;
    }

    // dp = USB_GET_ENDPOINT_DESCRIPTOR(ep);

    /* IN endpoint activation or deactivation.*/
    if (epcp->in_state != NULL) {
        // USB_DIRECTION_IN
        epcp->in_state->txcnt = 0;
        // epcp->in_state->txbuf = (uint8_t)(wUSB_EPnOffset[0]);
        // dp->TXCOUNT0 = 0;
        // dp->TXADDR0 = usb_pm_alloc(usbp, epcp->in_maxsize);
        // dp->TXADDR0 = (sn32_usb_pma_t)&wUSB_EPnOffset[ep-1];
        // epcp->in_state->txbuf = usb_pm_alloc(usbp, epcp->in_maxsize);//(uint8_t)&wUSB_EPnOffset[ep-1]; //(uint8_t)&wUSBINT_WriteDataBuf;
        // USB_EPnNak(ep);
        // switch (ep)
        // {
        // case 1:
        //     wTmp |= ~mskEP1_DIR;
        //     break;
        // case 2:
        //     wTmp |= ~mskEP2_DIR;
        //     break;
        // case 3:
        //     wTmp |= ~mskEP3_DIR;
        //     break;
        // case 4:
        //     wTmp |= ~mskEP4_DIR;
        //     break;
        // }
    }

    /* OUT endpoint activation or deactivation.*/
    if (epcp->out_state != NULL) {
        // USB_DIRECTION_OUT
        // USB_ENDPOINT_OUT(ep);
        epcp->out_state->rxcnt = 0;
        // epcp->out_state->rxbuf = (uint8_t)(wUSB_EPnOffset);
        // epcp->out_state->rxbuf = usb_pm_alloc(usbp, epcp->out_maxsize);//(uint8_t)&wUSB_EPnOffset[ep-1];//0; //&wUSBINT_ReadDataBuf;
        // USB_EPnNak(ep);

        // uint16_t nblocks;

        /* Endpoint size and address initialization.*/
        // if (epcp->out_maxsize > 62)
        // nblocks = (((((epcp->out_maxsize - 1) | 0x1f) + 1) / 32) << 10) |
        //             0x8000;
        // else
        // nblocks = ((((epcp->out_maxsize - 1) | 1) + 1) / 2) << 10;
        // dp->RXCOUNT0 = 0;
        // dp->RXADDR0  = (sn32_usb_pma_t)&wUSB_EPnOffset[ep-1];
        // dp->RXADDR0  = usb_pm_alloc(usbp, epcp->out_maxsize);
        // epr |= EPR_STAT_RX_NAK;

        // switch (ep)
        // {
        // case 1:
        //     wTmp |= mskEP1_DIR;
        //     break;
        // case 2:
        //     wTmp |= mskEP2_DIR;
        //     break;
        // case 3:
        //     wTmp |= mskEP3_DIR;
        //     break;
        // case 4:
        //     wTmp |= mskEP4_DIR;
        //     break;
        // }
    }

    SN_USB->CFG |= wTmp;

}

/**
 * @brief   Disables all the active endpoints except the endpoint zero.
 *
 * @param[in] usbp      pointer to the @p USBDriver object
 *
 * @notapi
 */
void usb_lld_disable_endpoints(USBDriver *usbp) {
    unsigned i;

    /* Disabling all endpoints.*/
    for (i = 1; i <= USB_ENDOPOINTS_NUMBER; i++) {
        USB_EPnDisable(i);
    }
}

/**
 * @brief   Returns the status of an OUT endpoint.
 *
 * @param[in] usbp      pointer to the @p USBDriver object
 * @param[in] ep        endpoint number
 * @return              The endpoint status.
 * @retval EP_STATUS_DISABLED The endpoint is not active.
 * @retval EP_STATUS_STALLED  The endpoint is stalled.
 * @retval EP_STATUS_ACTIVE   The endpoint is active.
 *
 * @notapi
 */
usbepstatus_t usb_lld_get_status_out(USBDriver *usbp, usbep_t ep) {
  (void)usbp;
   if (SN_USB->INSTS & mskEP0_OUT) {
       return EP_STATUS_DISABLED;
   } else if (SN_USB->INSTS & mskEP0_OUT_STALL) {
       return EP_STATUS_STALLED;
   } else {
       return EP_STATUS_ACTIVE;
   }
   // (mskEP0_IN_STALL|mskEP0_OUT_STALL))
//   switch (SN_USB->INSTS) {
//       case mskEP0_IN:
//           return EP_STATUS_DISABLED;
//       case mskEP0_IN_STALL:
//           return EP_STATUS_ACTIVE;
//       default:
//           return EP_STATUS_ACTIVE;
//   }
}

/**
 * @brief   Returns the status of an IN endpoint.
 *
 * @param[in] usbp      pointer to the @p USBDriver object
 * @param[in] ep        endpoint number
 * @return              The endpoint status.
 * @retval EP_STATUS_DISABLED The endpoint is not active.
 * @retval EP_STATUS_STALLED  The endpoint is stalled.
 * @retval EP_STATUS_ACTIVE   The endpoint is active.
 *
 * @notapi
 */
usbepstatus_t usb_lld_get_status_in(USBDriver *usbp, usbep_t ep) {
  (void)usbp;
   if (SN_USB->INSTS & mskEP0_IN) {
       return EP_STATUS_DISABLED;
   } else if (SN_USB->INSTS & mskEP0_IN_STALL) {
       return EP_STATUS_STALLED;
   } else {
       return EP_STATUS_ACTIVE;
   }

//   switch (SN_USB->INSTS) {
//     case mskEP0_OUT:
//       return EP_STATUS_DISABLED;
//     case mskEP0_OUT_STALL:
//       return EP_STATUS_STALLED;
//     default:
//       return EP_STATUS_ACTIVE;
//   }
}

/**
 * @brief   Reads a setup packet from the dedicated packet buffer.
 * @details This function must be invoked in the context of the @p setup_cb
 *          callback in order to read the received setup packet.
 * @pre     In order to use this function the endpoint must have been
 *          initialized as a control endpoint.
 * @post    The endpoint is ready to accept another packet.
 *
 * @param[in] usbp      pointer to the @p USBDriver object
 * @param[in] ep        endpoint number
 * @param[out] buf      buffer where to copy the packet data
 *
 * @notapi
 */
void usb_lld_read_setup(USBDriver *usbp, usbep_t ep, uint8_t *buf) {
    sn32_usb_read_fifo(ep, buf, 8);
}

// /**
//  * @brief   Prepares for a receive operation.
//  *
//  * @param[in] usbp      pointer to the @p USBDriver object
//  * @param[in] ep        endpoint number
//  *
//  * @notapi
//  */
// void usb_lld_prepare_receive(USBDriver *usbp, usbep_t ep) {

//   (void)usbp;
//   (void)ep;

// }

// /**
//  * @brief   Prepares for a transmit operation.
//  *
//  * @param[in] usbp      pointer to the @p USBDriver object
//  * @param[in] ep        endpoint number
//  *
//  * @notapi
//  */
// void usb_lld_prepare_transmit(USBDriver *usbp, usbep_t ep) {

//   (void)usbp;
//   (void)ep;

// }

/**
 * @brief   Starts a receive operation on an OUT endpoint.
 *
 * @param[in] usbp      pointer to the @p USBDriver object
 * @param[in] ep        endpoint number
 *
 * @notapi
 */
void usb_lld_start_out(USBDriver *usbp, usbep_t ep) {

  USBOutEndpointState *osp = usbp->epc[ep]->out_state;

  /* Transfer initialization.*/
  if (osp->rxsize == 0)         /* Special case for zero sized packets.*/
    osp->rxpkts = 1;
  else
    osp->rxpkts = (uint16_t)((osp->rxsize + usbp->epc[ep]->out_maxsize - 1) /
                             usbp->epc[ep]->out_maxsize);

//   EPR_SET_STAT_RX(ep, EPR_STAT_RX_VALID);

}

/**
 * @brief   Starts a transmit operation on an IN endpoint.
 *
 * @param[in] usbp      pointer to the @p USBDriver object
 * @param[in] ep        endpoint number
 *
 * @notapi
 */
void usb_lld_start_in(USBDriver *usbp, usbep_t ep) {
  size_t n;
  USBInEndpointState *isp = usbp->epc[ep]->in_state;

  /* Transfer initialization.*/
  n = isp->txsize;
  if (n > (size_t)usbp->epc[ep]->in_maxsize)
    n = (size_t)usbp->epc[ep]->in_maxsize;

  isp->txlast = n;
  usb_packet_write_from_buffer(ep, isp->txbuf, n);
  USB_EPnAck(ep, n);
}

/**
 * @brief   Brings an OUT endpoint in the stalled state.
 *
 * @param[in] usbp      pointer to the @p USBDriver object
 * @param[in] ep        endpoint number
 *
 * @notapi
 */
void usb_lld_stall_out(USBDriver *usbp, usbep_t ep) {

  USB_EPnStall(ep);

}

/**
 * @brief   Brings an IN endpoint in the stalled state.
 *
 * @param[in] usbp      pointer to the @p USBDriver object
 * @param[in] ep        endpoint number
 *
 * @notapi
 */
void usb_lld_stall_in(USBDriver *usbp, usbep_t ep) {

   USB_EPnStall(ep);

}

/**
 * @brief   Brings an OUT endpoint in the active state.
 *
 * @param[in] usbp      pointer to the @p USBDriver object
 * @param[in] ep        endpoint number
 *
 * @notapi
 */
void usb_lld_clear_out(USBDriver *usbp, usbep_t ep) {

//   __USB_CLRINSTS(mskEP0_OUT);

}

/**
 * @brief   Brings an IN endpoint in the active state.
 *
 * @param[in] usbp      pointer to the @p USBDriver object
 * @param[in] ep        endpoint number
 *
 * @notapi
 */
void usb_lld_clear_in(USBDriver *usbp, usbep_t ep) {

//   __USB_CLRINSTS(mskEP0_IN);

}

#endif /* HAL_USE_USB == TRUE */

/** @} */
