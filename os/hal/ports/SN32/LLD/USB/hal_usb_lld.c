/*
    ChibiOS - Copyright (C) 2006..2016 Giovanni Di Sirio

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
#include "hal.h"
#include "usb.h"
#include "usbhw.h"
#include "usbsystem.h"
#include "usbuser.h"
#include "usbepfunc.h"
#include "usbram.h"

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

// uint8_t *const usbd_sram = (uint8_t *)(SN_USB_BASE + 0x100);

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
 * @brief   Buffer for the EP0 setup packets.
 */
static uint8_t ep0setup_buffer[8];

/**
 * @brief   EP0 initialization structure.
 */
static const USBEndpointConfig ep0config = {
  USB_EP_MODE_TYPE_CTRL,
  _usb_ep0setup,
  _usb_ep0in,
  _usb_ep0out,
  0x40,
  0x40,
  &ep0_state.in,
  &ep0_state.out,
  1,
  ep0setup_buffer
};

/*===========================================================================*/
/* Driver local variables and types.                                         */
/*===========================================================================*/

/*===========================================================================*/
/* Driver local functions.                                                   */
/*===========================================================================*/

/**
 * @brief   Resets the packet memory allocator.
 *
 * @param[in] usbp      pointer to the @p USBDriver object
 */
static void usb_pm_reset(USBDriver *usbp) {
    /* The first 64 bytes are reserved for the descriptors table. The effective
       available RAM for endpoint buffers is just 448 bytes.*/
    usbp->pmnext = 64;
}

/**
 * @brief   Resets the packet memory allocator.
 *
 * @param[in] usbp      pointer to the @p USBDriver object
 * @param[in] size      size of the packet buffer to allocate
 * @return              The packet buffer address.
 */
static uint32_t usb_pm_alloc(USBDriver *usbp, size_t size) {
    uint32_t next;

    next = usbp->pmnext;
    usbp->pmnext += (size + 1) & ~1;
    osalDbgAssert(usbp->pmnext <= SN32_USB_PMA_SIZE, "PMA overflow");
    return next;
}

/**
 * @brief   Reads from a dedicated packet buffer.
 *
 * @param[in] ep        endpoint number
 * @param[out] buf      buffer where to copy the packet data
 * @return              The size of the receivee packet.
 *
 * @notapi
 */
static size_t usb_packet_read_to_buffer(usbep_t ep, uint8_t *buf) {
    size_t                  i, n;
    sn32_usb_descriptor_t   *udp  = USB_GET_DESCRIPTOR(ep);
    sn32_usb_pma_t *        pmap =  USB_ADDR2PTR(udp->RXADDR0);

    n = (size_t)udp->RXCOUNT0 & RXCOUNT_COUNT_MASK;

    i = n;

    while (i >= 2) {
        uint32_t w = *pmap++;
        *buf++     = (uint8_t)w;
        *buf++     = (uint8_t)(w >> 8);
        i -= 2;
    }

    if (i >= 1) {
        *buf = (uint8_t)*pmap;
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
    sn32_usb_descriptor_t *udp  = USB_GET_DESCRIPTOR(ep);
    sn32_usb_pma_t *       pmap = USB_ADDR2PTR(udp->TXADDR0);
    int                     i    = (int)n;

    udp->TXCOUNT0 = (sn32_usb_pma_t)n;

    while (i > 0) {
        uint32_t w;

        w = *buf++;
        w |= *buf++ << 8;
        *pmap++ = (sn32_usb_pma_t)w;
        i -= 2;
    }
}

/**
 * @brief   Common ISR code, serves the EP-related interrupts.
 *
 * @param[in] usbp      pointer to the @p USBDriver object
 * @param[in] ep        endpoint number
 *
 * @notapi
 */
static void usb_serve_endpoints(USBDriver *usbp, uint32_t ep) {
    size_t                   n;
    uint32_t                 iwIntFlag = SN_USB->INSTS;
    const USBEndpointConfig *epcp = usbp->epc[ep];

    if (iwIntFlag & mskEP0_IN) {
        /* IN endpoint, transmission.*/
        USBInEndpointState *isp = epcp->in_state;

        // EPR_CLEAR_CTR_TX(ep);
        __USB_CLRINSTS((mskEP0_SETUP|mskEP0_PRESETUP|mskEP0_OUT_STALL|mskEP0_IN_STALL));

        isp->txcnt += isp->txlast;
        n = isp->txsize - isp->txcnt;
        if (n > 0) {
            /* Transfer not completed, there are more packets to send.*/
            if (n > epcp->in_maxsize) n = epcp->in_maxsize;

            /* Writes the packet from the defined buffer.*/
            isp->txbuf += isp->txlast;
            isp->txlast = n;
            usb_packet_write_from_buffer(ep, isp->txbuf, n);

            /* Starting IN operation.*/
            // EPR_SET_STAT_TX(ep, EPR_STAT_TX_VALID);
        } else {
            /* Transfer completed, invokes the callback.*/
            _usb_isr_invoke_in_cb(usbp, ep);
        }
    }
    if (iwIntFlag & mskEP0_OUT) {
        /* OUT endpoint, receive.*/

        // EPR_CLEAR_CTR_RX(ep);
        __USB_CLRINSTS(mskEP0_IN);

        if (iwIntFlag & mskEP0_SETUP) {
            /* Setup packets handling, setup packets are handled using a
               specific callback.*/
            _usb_isr_invoke_setup_cb(usbp, ep);
        } else {
            USBOutEndpointState *osp = epcp->out_state;

            /* Reads the packet into the defined buffer.*/
            n = usb_packet_read_to_buffer(ep, osp->rxbuf);
            osp->rxbuf += n;

            /* Transaction data updated.*/
            osp->rxcnt += n;
            osp->rxsize -= n;
            osp->rxpkts -= 1;

            /* The transaction is completed if the specified number of packets
               has been received or the current packet is a short packet.*/
            if ((n < epcp->out_maxsize) || (osp->rxpkts == 0)) {
                /* Transfer complete, invokes the callback.*/
                _usb_isr_invoke_out_cb(usbp, ep);
            } else {
                /* Transfer not complete, there are more packets to receive.*/
                EPR_SET_STAT_RX(ep, EPR_STAT_RX_VALID);
            }
        }
    }
}

/*===========================================================================*/
/* Driver interrupt handlers and threads.                                    */
/*===========================================================================*/
// static void serve_endpoint_irq(USBDriver *usbp,
//                                uint32_t endpoint_mask,
//                                uint32_t epstatus)
// {
//   for (int hwEp = 0; hwEp < USB_MAX_ENDPOINTS; hwEp++) {
//     if (endpoint_mask  & (1 << hwEp)) {
//       uint32_t bus_status = ((epstatus >> (hwEp*(USBD_EPSTS_EPSTS1_Pos - USBD_EPSTS_EPSTS0_Pos))) & USBD_EPSTS_EPSTS0_Msk) >> USBD_EPSTS_EPSTS0_Pos;
//       usbep_t ep = (USBD->EP[hwEp].CFG & USBD_CFG_EP_NUM_Msk) >> USBD_CFG_EP_NUM_Pos;

//       switch (bus_status) {
//       case 0: /* In ACK */
//         {
//           USBInEndpointState *iesp = usbp->epc[ep]->in_state;
//           iesp->txcnt += USBD->EP[hwEp].MXPLD;
//           if (iesp->txcnt >= iesp->txsize) {
//             _usb_isr_invoke_in_cb(usbp, ep);
//           }
//           else {
//             uint32_t txcnt;
//             if ((iesp->txcnt + usbp->epc[ep]->in_maxsize) > iesp->txsize) {
//               txcnt = iesp->txsize - iesp->txcnt;
//             }
//             else {
//               txcnt = usbp->epc[ep]->in_maxsize;
//             }
//             for (uint32_t n = 0; n < txcnt; n++) {
//               usbd_sram[(USBD->EP[hwEp].BUFSEG) + n] = iesp->txbuf[iesp->txcnt + n];
//             }
//             _toggle_dsq(hwEp, &(iesp->dsq));
//             USBD->EP[hwEp].MXPLD = txcnt;
//           }
//         }
//         break;
//       case 0x1: /* In NAK */
//         break;
//       case 0x2: /* Out Packet Data 0 ACK */
//       case 0x6: /* Out Packet Data 1 ACK */
//         {
//           USBOutEndpointState *oesp = usbp->epc[ep]->out_state;
//           for (uint32_t n = 0; n < USBD->EP[hwEp].MXPLD; n++) {
//             oesp->rxbuf[oesp->rxcnt] = usbd_sram[USBD->EP[hwEp].BUFSEG + n];
//             oesp->rxcnt++;
//           }
//           if (oesp->rxcnt == oesp->rxsize) {
//             _usb_isr_invoke_out_cb(usbp, ep);
//           }
//           else {
//             _toggle_dsq(hwEp, &(oesp->dsq));
//             if ((oesp->rxcnt + usbp->epc[ep]->out_maxsize) > oesp->rxsize) {
//               USBD->EP[hwEp].MXPLD = oesp->rxsize - oesp->rxcnt;
//             }
//             else {
//               USBD->EP[hwEp].MXPLD = usbp->epc[ep]->out_maxsize;
//             }
//           }
//         }
//         break;
//       case 0x3: /* Setup ACK */
//         break;
//       case 0x7: /* Isochronous transfer end */
//       default:
//           osalDbgAssert(FALSE, "not supported");
//       }
//     }
//   }
// }

// static void serve_usb_irq(USBDriver *usbp) {
//   uint32_t iwIntFlag = SN_USB->INSTS;
//   uint32_t epsts     = SN_USB->EP0CTL;


//   if (iwIntFlag & (mskBUS_RESET|mskBUS_SUSPEND|mskBUS_WAKEUP)) {
//     if (iwIntFlag & mskBUS_RESET) {
//       _usb_reset(usbp);
//     }
//     if (iwIntFlag & mskBUS_SUSPEND) {
//       if (usbp->state == USB_ACTIVE) {
//         _usb_suspend(usbp);
//       }
//     }
//     if (iwIntFlag & mskBUS_WAKEUP) {
//       (usbp)->receiving = 0;
//       (usbp)->transmitting = 0;
//       _usb_wakeup(usbp);
//     }
//   }

//   if (iwIntFlag & (mskEP6_ACK|mskEP5_ACK|mskEP4_ACK|mskEP3_ACK|mskEP2_ACK|mskEP1_ACK)) {
//     /* check endpoints first */
//     // serve_endpoint_irq(usbp, (iwIntFlag & USBD_INTSTS_EPEVT_Msk) >> USBD_INTSTS_EPEVT_Pos, epsts);
//     SN_USB->INSTS = iwIntFlag & USBD_INTSTS_EPEVT_Msk;
//     /* then handle setup packets */
//     if (iwIntFlag & mskEP0_SETUP) {
//       SN_USB->INSTS |= mskEP0_SETUP;
//       USBInEndpointState *iesp = usbp->epc[0]->in_state;
//       iesp->dsq = 0;
//       _usb_isr_invoke_setup_cb(usbp, 0);
//     }
//   }
//   if (iwIntFlag & mskBUS_WAKEUP) {
//     // USBD->INTSTS |= USBD_INTSTS_WAKEUP_STS_Msk;
//     // SN_USB->INSTS |= mskBUS_WAKEUP;
//     __USB_CLRINSTS(mskBUS_WAKEUP);

//     // USBD->ATTR |= USBD_ATTR_PHY_EN_Msk;
//     // SN_USB->CFG |= mskPHY_EN;
//     SN_USB->CFG |= (mskESD_EN|mskPHY_EN);

//   }
// }

OSAL_IRQ_HANDLER(SN32_USB_IRQ_VECTOR) {
    uint32_t   iwIntFlag;
    USBDriver *usbp = &USBD1;

    OSAL_IRQ_PROLOGUE();

    iwIntFlag = SN_USB->INSTS; /* Get Interrupt Status and clear immediately. */

	if (iwIntFlag & mskBUS_WAKEUP) /* Wakeup */
	{
	    __USB_CLRINSTS(mskBUS_WAKEUP);		    // Clear BUS_WAKEUP
        SN_USB->CFG |= (mskESD_EN|mskPHY_EN);	// enable ESD_EN & PHY_EN
        (usbp)->receiving = 0;
        (usbp)->transmitting = 0;
		_usb_wakeup(usbp);
        __asm__ __volatile__("bkpt");
	}

	if ((iwIntFlag & mskUSB_SOF) & (SN_USB->INTEN & mskUSB_SOF_IE)) /* SOF */
	{
        __USB_CLRINSTS(mskUSB_SOF);
		_usb_isr_invoke_sof_cb(usbp);
        __asm__ __volatile__("bkpt");
	}

	/* Device Status Interrupt (BusReset, Suspend, Resume) */
	if (iwIntFlag & (mskBUS_RESET|mskBUS_SUSPEND|mskBUS_RESUME))
	{
		if (iwIntFlag & mskBUS_RESET) /* BusReset */
		{
            //  __asm__ __volatile__("bkpt");
            // uint32_t	wLoop;
            // __USB_CLRINSTS(0x00000000);		// Clear all USB Event status
            // SN_USB->INSTSC = 0xFFFFFFFF;
            // // sUSB_EumeData.wUSB_Status = mskBUSRESET;	// Set BusReset = 1
            // __USB_SETADDRESS(0);					// Set USB address = 0
            // USB_EPnStall(USB_EP0);			// Set EP0 enable & INOUTSTALL

            // for (wLoop=USB_EP1; wLoop<=USB_EP4; wLoop++)
            //     USB_EPnDisable(wLoop);		// Set EP1~EP6 disable & NAK

			_usb_reset(usbp);
            // usb_lld_reset(usbp);
		}
		else if (iwIntFlag & mskBUS_SUSPEND) /* Suspend */
		{
            if (usbp->state == USB_ACTIVE) {
                __USB_CLRINSTS(mskBUS_SUSPEND);
                _usb_suspend(usbp);
                 __asm__ __volatile__("bkpt");
            }
		}
		else if	( (iwIntFlag & mskBUS_RESUME)) /* Resume */
		{
			__USB_CLRINSTS(mskBUS_RESUME);
            _usb_wakeup(usbp);
             __asm__ __volatile__("bkpt");
		}
	}
	if (iwIntFlag & (mskEP0_SETUP|mskEP0_IN|mskEP0_OUT|mskEP0_IN_STALL|mskEP0_OUT_STALL)) /* Device Status Interrupt (SETUP, IN, OUT) */
	{
        __asm__ __volatile__("bkpt");
		if (iwIntFlag & mskEP0_SETUP) /* SETUP */
		{
			usb_serve_endpoints(usbp, 0);
            // USB_EP0SetupEvent();
		}
		else if (iwIntFlag & mskEP0_IN)
		{									/* IN */
			usb_serve_endpoints(usbp, 0);
		}
		else if (iwIntFlag & mskEP0_OUT)
		{									/* OUT */
			usb_serve_endpoints(usbp, 0);
		}
		else if (iwIntFlag & (mskEP0_IN_STALL|mskEP0_OUT_STALL))
		{
			SN_USB->INSTSC = (mskEP0_IN_STALL|mskEP0_OUT_STALL);
			usb_serve_endpoints(usbp, 0);
		}
	}
	if (iwIntFlag & (mskEP4_ACK|mskEP3_ACK|mskEP2_ACK|mskEP1_ACK))
	{
        __asm__ __volatile__("bkpt");
		if (iwIntFlag & mskEP1_ACK)
		{									/* EP1 ACK */
			usb_serve_endpoints(usbp, 1);
		}
		if (iwIntFlag & mskEP2_ACK)
		{									/* EP2 ACK */
			usb_serve_endpoints(usbp, 2);
		}
		if (iwIntFlag & mskEP3_ACK)
		{									/* EP3 ACK */
			usb_serve_endpoints(usbp, 3);
		}
		if (iwIntFlag & mskEP4_ACK)
		{									/* EP4 ACK */
			usb_serve_endpoints(usbp, 4);
		}
	}

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
  /* Driver initialization.*/
  usbObjectInit(&USBD1);
}

/**
 * @brief   Configures and activates the USB peripheral.
 *
 * @param[in] usbp      pointer to the @p USBDriver object
 *
 * @notapi
 */
void usb_lld_start(USBDriver *usbp) {
    uint32_t wTmp, i;
    // uint32_t wTmp;

    if (usbp->state == USB_STOP) {
        /* Enables the peripheral.*/
    #if PLATFORM_USB_USE_USB1 == TRUE
        if (&USBD1 == usbp) {
            SN_FLASH->LPCTRL = 0x5AFA0000;		        //Disable Slow mode power saving

            SN_SYS0->ANBCTRL |= USB_IHRC_EN;			//enable IHRC
            while ((SN_SYS0->CSST & 0x01) != 0x01);		//check IHRC ready
            SN_SYS0->CLKCFG = 0x00;						//switch IHRC
            while ((SN_SYS0->CLKCFG & 0x70) != 0x00);

            SN_SYS1->AHBCLKEN |= (0x1 << 4);           // Enable USBCLKEN
            SN_USB->CFG |= (mskESD_EN | mskPHY_EN);    // Enable ESD_EN & PHY_EN
            SN_USB->INTEN = (mskBUS_IE | mskUSB_IE | mskUSB_BUSWK_IE | mskUSB_SOF_IE);  // Enable the USB Interrupt
            SN_USB->SGCTL = mskBUS_J_STATE;            // BUS_DRVEN = 0, BUS_DP = 1, BUS_DN = 0

            // VREG33_EN = 1, PHY_EN = 1, DPPU_EN = 1, SIE_EN = 1, ESD_EN = 1, FLTDET_PUEN = 1
            wTmp = (mskVREG33_EN | mskPHY_EN | mskDPPU_EN | mskSIE_EN | mskESD_EN | mskFLTDET_PUEN_DISABLE);
            SN_USB->CFG = wTmp;
            for (i = 0; i < DISCHARE_DELAY; i++);
            // chThdSleepMilliseconds(50);
            SN_USB->CFG    = (wTmp & (~mskVREG33DIS_EN)) | mskDPPU_EN;
            SN_USB->PHYPRM = (0x01U << 31);

            nvicEnableVector(USB_IRQn, 5);
        }
    #endif
        // usb_lld_reset(usbp);
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
__asm__ __volatile__("bkpt");
  if (usbp->state == USB_READY) {
    /* Resets the peripheral.*/

    /* Disables the peripheral.*/
#if PLATFORM_USB_USE_USB1 == TRUE
    if (&USBD1 == usbp) {
        nvicDisableVector(USB_IRQn);
        SN_SYS1->AHBCLKEN = (0x0 << 4);  // Disable USBCLKEN
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
    // // __asm__ __volatile__("bkpt");
    // /* Post reset initialization.*/
    // SN_USB->INSTSC = 0xFFFFFFFF;  // Clear all USB Event status
    // SN_USB->ADDR = 0;             // Set USB address = 0

    // /* Resets the packet memory allocator.*/
    // usb_pm_reset(usbp);

    // /* EP0 initialization.*/
    // usbp->epc[0] = &ep0config;
    // usb_lld_init_endpoint(usbp, 0);
  /* Post reset initialization.*/
  /* EP0 initialization.*/
    usbp->epc[0] = &ep0config;
    /* NUC123 has 512b SRAM for EP-buffers; the first 8b are reserved for setup packets */
    // usbp->bufnext = 0;
    // usbp->ep0next = 0;

    usb_lld_init_endpoint(usbp, 0);
    // usbConnectBus(usbp);
    // USBD->FADDR = 0;
    SN_USB->ADDR = 0;
}

/**
 * @brief   Sets the USB address.
 *
 * @param[in] usbp      pointer to the @p USBDriver object
 *
 * @notapi
 */
void usb_lld_set_address(USBDriver *usbp) {
    SN_USB->ADDR = (uint32_t)(usbp->address);
}

// uint32_t usb_alloc_buf(USBDriver *usbp, size_t size) {
//   uint32_t buf;
//   buf = usbp->bufnext;
//   usbp->bufnext += size;
//   osalDbgAssert(usbp->bufnext <= 512, "usb buffer space full");
//   return buf;
// }

/**
 * @brief   Enables an endpoint.
 *
 * @param[in] usbp      pointer to the @p USBDriver object
 * @param[in] ep        endpoint number
 *
 * @notapi
 */
void usb_lld_init_endpoint(USBDriver *usbp, usbep_t ep) {
    // uint16_t                epr;
    // sn32_usb_descriptor_t   *dp;
    const USBEndpointConfig *epcp = usbp->epc[ep];
    uint8_t hwep;

    if (epcp->in_state != NULL) {
        hwep = usbp->epnext;
        usbp->epnext += 1;
        osalDbgAssert(usbp->epnext <= USB_MAX_ENDPOINTS, "No endpoints left");
        // uint32_t address = ep * 64;
        // USB_EPnBufferOffset(ep,address);
        // USBD->EP[hwep].BUFSEG = usb_alloc_buf(usbp, epcp->in_maxsize);
        SN_USB->EP1BUFOS = usb_pm_alloc(usbp, epcp->in_maxsize);
        if (epcp->ep_mode == USB_EP_MODE_TYPE_CTRL)
        /* 2 == in */
        // USBD->EP[hwep].CFG = (ep << USBD_CFG_EP_NUM_Pos) | (2 << USBD_CFG_STATE_Pos) |  USBD_CFG_CSTALL_Msk;
        // SN_USB->CFG =
        else
        // USBD->EP[hwep].CFG = (ep << USBD_CFG_EP_NUM_Pos) | (2 << USBD_CFG_STATE_Pos);
        // SN_USB->CFG =
        epcp->in_state->hwEp = hwep;
    }

    if (epcp->out_state != NULL) {
        hwep = usbp->epnext;
        usbp->epnext += 1;
        osalDbgAssert(usbp->epnext <= USB_MAX_ENDPOINTS, "No endpoints left");

        // USBD->EP[hwep].BUFSEG = usb_alloc_buf(usbp, epcp->out_maxsize);
        SN_USB->EP1BUFOS = usb_pm_alloc(usbp, epcp->in_maxsize);
        if (epcp->ep_mode == USB_EP_MODE_TYPE_CTRL)
        /* 1 == Out */
        // USBD->EP[hwep].CFG = (ep << USBD_CFG_EP_NUM_Pos) | (1 << USBD_CFG_STATE_Pos) | USBD_CFG_CSTALL_Msk;
        // SN_USB->CFG =
        else
        // USBD->EP[hwep].CFG = (ep << USBD_CFG_EP_NUM_Pos) | (1 << USBD_CFG_STATE_Pos);
        // SN_USB->CFG =
        epcp->out_state->hwEp = hwep;
    }


    // __asm__ __volatile__("bkpt");

    /* Setting the endpoint type. Note that isochronous endpoints cannot be
       bidirectional because it uses double buffering and both transmit and
       receive descriptor fields are used for either direction.*/
    // switch (epcp->ep_mode & USB_ENDPOINT_TYPE_MASK) {
    //     // case USB_ISOCHRONOUS_MODE:
    //     //     osalDbgAssert(false, "isochronous support disabled");
    //     //     /* Falls through.*/
    //     case USB_BULK_MODE:
    //         epr = EPR_EP_TYPE_BULK;
    //         break;
    //     case USB_INTERRUPT_MODE:
    //         epr = EPR_EP_TYPE_INTERRUPT;
    //         break;
    //     default:
    //         epr = EPR_EP_TYPE_CONTROL;
    // }

    // dp = USB_GET_DESCRIPTOR(ep);

    // /* IN endpoint handling.*/
    // if (epcp->in_state != NULL) {
    //     dp->TXCOUNT0 = 0;
    //     dp->TXADDR0  = usb_pm_alloc(usbp, epcp->in_maxsize);

    //     // epr |= EPR_STAT_TX_NAK;
    //     USB_EPnNak(ep);
    // }

    /* OUT endpoint handling.*/
    // if (epcp->out_state != NULL) {
    //     uint16_t nblocks;

    //     /* Endpoint size and address initialization.*/
    //     if (epcp->out_maxsize > 62)
    //         nblocks = (((((epcp->out_maxsize - 1) | 0x1f) + 1) / 32) << 10) | 0x8000;
    //     else
    //         nblocks = ((((epcp->out_maxsize - 1) | 1) + 1) / 2) << 10;
    //     dp->RXCOUNT0 = nblocks;
    //     dp->RXADDR0  = usb_pm_alloc(usbp, epcp->out_maxsize);
    //     // epr |= EPR_STAT_RX_NAK;
    //     USB_EPnNak(ep);
    // }

    // /* Resetting the data toggling bits for this endpoint.*/
    // if (SN32_USB->EPR[ep] & EPR_DTOG_RX) {
    //     epr |= EPR_DTOG_RX;
    // }

    // if (SN32_USB->EPR[ep] & EPR_DTOG_TX) {
    //     epr |= EPR_DTOG_TX;
    // }

    // USB_ClrEPnToggle(ep);

    /* EPxR register setup.*/
    // EPR_SET(ep, epr | ep);
    // EPR_TOGGLE(ep, epr);
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

    /* Resets the packet memory allocator.*/
    usb_pm_reset(usbp);

    /* Disabling all endpoints.*/
    for (i = 1; i <= USB_ENDOPOINTS_NUMBER; i++) {
        // EPR_TOGGLE(i, 0);
        USB_ClrEPnToggle(i);
        // EPR_SET(i, 0);
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
  switch (SN_USB->INSTS) {
      case mskEP0_IN:
          return EP_STATUS_DISABLED;
      case mskEP0_IN_STALL:
          return EP_STATUS_STALLED;
      default:
          return EP_STATUS_ACTIVE;
  }
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
  switch (SN_USB->INSTS) {
    case mskEP0_OUT:
      return EP_STATUS_DISABLED;
    case mskEP0_OUT_STALL:
      return EP_STATUS_STALLED;
    default:
      return EP_STATUS_ACTIVE;
  }
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
    sn32_usb_pma_t *       pmap;
    sn32_usb_descriptor_t *udp;
    uint32_t                n;

    (void)usbp;
    udp  = USB_GET_DESCRIPTOR(ep);
    pmap = USB_ADDR2PTR(udp->RXADDR0);
    for (n = 0; n < 4; n++) {
        *(uint16_t *)buf = (uint16_t)*pmap++;
        buf += 2;
    }
}

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
    if (osp->rxsize == 0) /* Special case for zero sized packets.*/
        osp->rxpkts = 1;
    else
        osp->rxpkts = (uint16_t)((osp->rxsize + usbp->epc[ep]->out_maxsize - 1) / usbp->epc[ep]->out_maxsize);

    EPR_SET_STAT_RX(ep, EPR_STAT_RX_VALID);
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
    size_t              n;
    USBInEndpointState *isp = usbp->epc[ep]->in_state;

    /* Transfer initialization.*/
    n = isp->txsize;
    if (n > (size_t)usbp->epc[ep]->in_maxsize) n = (size_t)usbp->epc[ep]->in_maxsize;

    isp->txlast = n;
    usb_packet_write_from_buffer(ep, isp->txbuf, n);

    EPR_SET_STAT_TX(ep, EPR_STAT_TX_VALID);
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
    (void)usbp;

    EPR_SET_STAT_RX(ep, EPR_STAT_RX_STALL);
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
    (void)usbp;

    EPR_SET_STAT_TX(ep, EPR_STAT_TX_STALL);
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
    (void)usbp;

    /* Makes sure to not put to NAK an endpoint that is already
       transferring.*/
    if ((SN32_USB->EPR[ep] & EPR_STAT_RX_MASK) != EPR_STAT_RX_VALID) EPR_SET_STAT_TX(ep, EPR_STAT_RX_NAK);
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
    (void)usbp;

    /* Makes sure to not put to NAK an endpoint that is already
       transferring.*/
    if ((SN32_USB->EPR[ep] & EPR_STAT_TX_MASK) != EPR_STAT_TX_VALID) EPR_SET_STAT_TX(ep, EPR_STAT_TX_NAK);
}

#endif /* HAL_USE_USB == TRUE */

/** @} */
