/*
 * Copyright 2017, Data61
 * Commonwealth Scientific and Industrial Research Organisation (CSIRO)
 * ABN 41 687 119 230.
 *
 * This software may be distributed and modified according to the terms of
 * the BSD 2-Clause license. Note that NO WARRANTY is provided.
 * See "LICENSE_BSD2.txt" for details.
 *
 * @TAG(DATA61_BSD)
 */

#define ZF_LOG_LEVEL ZF_LOG_DEBUG

#include "otgusbtty.h"
#include <usb/usb.h>
#include "../services.h"

#include <stdio.h>
#include <string.h>

struct otg_usbtty {
    usb_otg_t otg;
    ps_dma_man_t* dman;
    void* desc;
    uintptr_t pdesc;
};

struct free_token {
    void* vaddr;
    size_t size;
    ps_dma_man_t* dman;
};

static struct device_desc otg_usbtty_device_desc = {
    .bLength                = 0x12,
    .bDescriptorType        = 0x1,
    .bcdUSB                 = 0x110,
    .bDeviceClass           = 0x0,
    .bDeviceSubClass        = 0x0,
    .bDeviceProtocol        = 0x0,
    .bMaxPacketSize0        = 0x40,
    .idVendor               = 0x067b,
    .idProduct              = 0x2303,
    .bcdDevice              = 0x300,
    .iManufacturer          = 0x0 /* 0x1 */,
    .iProduct               = 0x0 /* 0x2 */,
    .iSerialNumber          = 0x0,
    .bNumConfigurations     = 0x1
};

static struct config_desc otg_usbtty_config_desc = {
    .bLength                = 0x9,
    .bDescriptorType        = 0x2,
    .wTotalLength           = 0x27,
    .bNumInterfaces         = 0x1,
    .bConfigurationValue    = 0x1,
    .iConfigurationIndex    = 0x0,
    .bmAttributes           = 0x80,
    .bMaxPower              = 0x32
};

static struct iface_desc otg_usbtty_iface_desc = {
    .bLength                = 0x9,
    .bDescriptorType        = 0x4,
    .bInterfaceNumber       = 0x0,
    .bAlternateSetting      = 0x0,
    .bNumEndpoints          = 0x3,
    .bInterfaceClass        = 0xff,
    .bInterfaceSubClass     = 0x0,
    .bInterfaceProtocol     = 0x0,
    .iInterface             = 0x0
};

static struct endpoint_desc otg_usbtty_ep1_desc = {
    .bLength                = 0x7,
    .bDescriptorType        = 0x5,
    .bEndpointAddress       = 0x81,
    .bmAttributes           = 0x3,
    .wMaxPacketSize         = 0xa,
    .bInterval              = 0x1
};

static struct endpoint_desc otg_usbtty_ep2_desc = {
    .bLength                = 0x7,
    .bDescriptorType        = 0x5,
    .bEndpointAddress       = 0x2,
    .bmAttributes           = 0x2,
    .wMaxPacketSize         = 0x40,
    .bInterval              = 0x0
};

static struct endpoint_desc otg_usbtty_ep3_desc = {
    .bLength                = 0x7,
    .bDescriptorType        = 0x5,
    .bEndpointAddress       = 0x83,
    .bmAttributes           = 0x2,
    .wMaxPacketSize         = 0x40,
    .bInterval              = 0x0
};


static void
freebuf_cb(usb_otg_t otg, void* token,
           enum usb_xact_status stat)
{
    struct free_token* t;

    if (stat != XACTSTAT_SUCCESS) {
	    ZF_LOGF("Transaction failed\n");
    }
    t = (struct free_token*)token;
    ps_dma_free_pinned(t->dman, t->vaddr, t->size);
    usb_free(t);
}

static void
send_desc(otg_usbtty_t tty, enum DescriptorType type, int index,
          int maxlen)
{
    struct anon_desc* d = NULL;
    /* Not handling index yet... */
    if (index != 0) {
	    ZF_LOGF("Index not implemented\n");
    }
    /* Find the descriptor */
    switch (type) {
    case DEVICE:
        d = (struct anon_desc*)&otg_usbtty_device_desc;
        if (maxlen >= d->bLength) {
            ZF_LOGD("device descriptor read: all(%d)",  d->bLength);
        } else {
            ZF_LOGD("device descriptor read: %d", maxlen);
        }
        break;
    case CONFIGURATION:
        ZF_LOGD("config");
        break;
    case STRING:
        ZF_LOGD("string");
        break;
    case INTERFACE:
        ZF_LOGD("interface");
        break;
    case ENDPOINT:
        ZF_LOGD("endpoint");
        break;
    case DEVICE_QUALIFIER:
        ZF_LOGD("device qualifier");
        break;
    case OTHER_SPEED_CONFIGURATION:
        ZF_LOGD("other speed");
        break;
    case INTERFACE_POWER:
        ZF_LOGD("interface power");
        break;
    case HID:
        ZF_LOGD("hid");
        break;
    case HUB:
        ZF_LOGD("Hub");
        break;
    default:
        ZF_LOGD("Unhandled descriptor request");
    }

    /* Send the descriptor */
    if (d != NULL) {
        struct free_token* t = NULL;
        uintptr_t pbuf;
        int err;

        t = usb_malloc(sizeof(*t));
        ZF_LOGF_IF(t == NULL, "Out of memory");

        t->dman = tty->dman;
        ZF_LOGF_IF(t->dman == NULL, "DMA manager not set");

        /* limit size to prevent babble */
        t->size = d->bLength;
        if (maxlen < t->size) {
            t->size = maxlen;
        }
        /* Copy in */
        t->vaddr = ps_dma_alloc_pinned(t->dman, t->size, 32, 0, PS_MEM_NORMAL, &pbuf);
        ZF_LOGF_IF(t->vaddr == NULL, "Out of DMA memory\n");

        memcpy(t->vaddr, d, t->size);

        /* Send the packet */
        err = otg_prime(tty->otg, 0, PID_IN, t->vaddr, pbuf, t->size, freebuf_cb, t);
        if (err) {
            ps_dma_free_pinned(tty->dman, t->vaddr, t->size);
            ZF_LOGF("OTG device error\n");
        }
        /* Status phase */
        err = otg_prime(tty->otg, 0, PID_OUT, NULL, 0, 0, freebuf_cb, t);
        if (err) {
            ps_dma_free_pinned(tty->dman, t->vaddr, t->size);
            ZF_LOGF("OTG device error\n");
        }
    }
}


static void
usbtty_setup_cb(usb_otg_t otg, void* token, struct usbreq* req)
{
    otg_usbtty_t tty = (otg_usbtty_t)token;
    (void)otg_usbtty_config_desc;
    (void)otg_usbtty_iface_desc;
    (void)otg_usbtty_ep1_desc;
    (void)otg_usbtty_ep2_desc;
    (void)otg_usbtty_ep3_desc;
    switch (req->bRequest) {
    case GET_DESCRIPTOR:
        ZF_LOGD("get desc");
        send_desc(tty, req->wValue >> 8, req->wValue & 0xff,
                  req->wLength);
        break;
    case GET_CONFIGURATION:
        ZF_LOGD("get conf");
        break;
    case GET_STATUS:
        ZF_LOGD("get stat");
        break;
    case CLR_FEATURE:
        ZF_LOGD("Clear feat");
        break;
    case SET_FEATURE:
        ZF_LOGD("Set feature");
        break;
    case SET_ADDRESS:
        ZF_LOGD("Set address");
        break;
    case SET_DESCRIPTOR:
        ZF_LOGD("Set descriptor");
        break;
    case SET_CONFIGURATION:
        ZF_LOGD("Set config");
        break;
    case GET_INTERFACE:
        ZF_LOGD("Get interface");
        break;
    case SET_INTERFACE:
        ZF_LOGD("Set interface");
        break;
    default:
        ZF_LOGD("Unhandled request %d", req->bRequest);
    }
}

int
otg_usbtty_init(usb_otg_t otg, ps_dma_man_t* dman,
                otg_usbtty_t* usbtty)
{
    otg_usbtty_t tty;
    int err;

    if (!dman || !usbtty || !otg) {
        ZF_LOGF("Invalid arguments %p:%p:%p\n", dman, usbtty, otg);
    }

    /* Allocate memory */
    tty = usb_malloc(sizeof(*tty));
    if (tty == NULL) {
        return -1;
    }
    tty->dman = dman;
    tty->otg = otg;
    /* Initialise the control endpoint */
    err = otg_ep0_setup(otg, usbtty_setup_cb, tty);
    if (err) {
        usb_free(tty);
        return -1;
    }
    *usbtty = tty;
    return 0;
}

