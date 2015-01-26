/*
 * Copyright 2014, NICTA
 *
 * This software may be distributed and modified according to the terms of
 * the BSD 2-Clause license. Note that NO WARRANTY is provided.
 * See "LICENSE_BSD2.txt" for details.
 *
 * @TAG(NICTA_BSD)
 */

#include <sel4arm-vmm/devices/vusb.h>

#ifdef CONFIG_LIB_USB

#include <vka/capops.h>
#include <vka/object.h>

#include <vspace/vspace.h>
#include <simple/simple.h>
#include <platsupport/io.h>
#include <sel4utils/thread.h>
#include <sel4arm-vmm/vm.h>
#include <stdlib.h>
#include <usb/usb_host.h>
#include <usb/usb.h>
#include <dma/dma.h>

#include "../devices.h"

#include <string.h>

//#define DEBUG_ROOTHUB


#ifdef DEBUG_ROOTHUB
#define DROOTHUB(...) printf("VMM root hub:" __VA_ARGS__)
#else
#define DROOTHUB(...) do{}while(0)
#endif

#define VUSB_NPORTS 3

struct sel4urbt {
    uintptr_t paddr;
    int size;
    int type;
    int res;
} __attribute__ ((packed));

struct sel4urb {
    uint8_t addr;
    uint8_t hub_addr;
    uint8_t hub_port;
    uint8_t speed;
    uint16_t ep;
    uint16_t max_pkt;
    uint16_t rate_ms;
    uint16_t nxact;
    void*    token;
    uint32_t urb_status;
    struct sel4urbt desc[2];
} __attribute__ ((packed));

typedef struct usb_data_regs {
    struct sel4urb sel4urb[10];
} usb_data_regs_t;

typedef struct usb_ctrl_regs {
    uint32_t status;
    uint32_t req_reply;
    uint32_t nPorts;
    struct usbreq req;
} usb_ctrl_regs_t;

struct vframe {
    vka_object_t obj;
    cspacepath_t vm_cap_path, vmm_cap_path;
    void* base;
};

struct vusb_device {
    vm_t* vm;
    virq_handle_t virq;
    usb_host_t* hcd;
    struct xact int_xact;
    usb_data_regs_t* data_regs;
    usb_ctrl_regs_t* ctrl_regs;
};


static inline vusb_device_t* device_to_vusb_dev_data(struct device* d)
{
    return (vusb_device_t*)d->priv;
}

static enum usb_speed
urb_get_speed(struct sel4urb* u)
{
    switch (u->speed) {
    case 3:
        return USBSPEED_HIGH;
    default:
        return USBSPEED_HIGH;
    }
}

static int
desc_to_xact(struct sel4urbt* desc, struct xact* xact)
{
    switch (desc->type) {
    case -1:
        xact->type = PID_SETUP;
        break;
    case 0:
        xact->type = PID_IN;
        break;
    case 1:
        xact->type = PID_OUT;
        break;
    default:
        return -1;
    }
    xact->len = desc->size;
    xact->paddr = desc->paddr;
    xact->vaddr = (void*)0xdeadbeef;
    return 0;
}

static int
sel4urb_to_xact(struct sel4urb* surb, struct xact* xact)
{
    int nxact;
    int err;
    int i;
    nxact = surb->nxact;
    assert(nxact <= 2);
    assert(nxact > 0);
    if (surb->urb_status != (1U << 31)) {
        printf("Notification but no packet!\n");
        return -1;
    }
    /* Fill translate surb to xact */
    for (i = 0; i < nxact; i++) {
        err = desc_to_xact(&surb->desc[i], &xact[i]);
        if (err) {
            return -1;
        }
    }
    /* Check if this should be an INT packet */
    if (surb->rate_ms) {
        xact[0].type = PID_INT;
        assert(nxact == 1);
        return nxact;
    } else {
        /* Terminate with ACK */
        memset(&xact[nxact], 0, sizeof(*xact));
        if (xact[nxact - 1].type == PID_IN) {
            xact[nxact].type = PID_OUT;
        } else {
            xact[nxact].type = PID_IN;
        }
        return nxact + 1;
    }
}

static int
ctrl_to_xact(usb_ctrl_regs_t* ctrl, struct xact* xact)
{
    xact[0].len = sizeof(struct usbreq);
    xact[0].vaddr = (void*)&ctrl->req;
    xact[0].paddr = 0xdeadbeef;
    xact[0].type = PID_SETUP;
    xact[1].len = ctrl->req.wLength;
    xact[1].vaddr = (void*)&ctrl->req_reply;
    xact[1].paddr = 0xdeadbeef;
    if (ctrl->req.bmRequestType & BIT(7)) {
        /* Device to host */
        xact[1].type = PID_IN;
    } else {
        /* Host to device */
        xact[1].type = PID_OUT;
    }
    return 0;
}

static int root_hub_ctrl_start(usb_host_t* hcd, usb_ctrl_regs_t* ctrl)
{
    struct xact xact[2];
    int len;
    int err;
    err = ctrl_to_xact(ctrl, xact);
    if (err) {
        return -1;
    }
    len = usb_hcd_schedule(hcd, 1, -1, 0, 0, 0, 64, 0, xact, 2, NULL, NULL);
    DROOTHUB("usb ctrl complete len %d\n", len);
    return len;
}

static int
handle_vusb_fault(struct device* d, vm_t* vm, fault_t* fault)
{
    vusb_device_t* vusb;
    usb_ctrl_regs_t *ctrl_regs;
    uint32_t* reg;
    int offset;

    assert(d->priv);
    offset = fault->addr - d->pstart - 0x1000;
    vusb = device_to_vusb_dev_data(d);
    ctrl_regs = vusb->ctrl_regs;
    reg = (uint32_t*)((void*)ctrl_regs + (offset & ~0x3));
    if (fault_is_read(fault)) {
        if (reg != &ctrl_regs->status) {
            fault->data = *reg;
        }
    } else {
        if (reg == &ctrl_regs->status) {
            /* start a transfer */
            root_hub_ctrl_start(vusb->hcd, ctrl_regs);
        } else if ((void*)reg >= (void*)&ctrl_regs->req) {
            *reg = fault_emulate(fault, *reg);
        }
    }
    return advance_fault(fault);
}

const struct device dev_vusb = {
    .devid = DEV_CUSTOM,
    .name = "virtual usb",
    .pstart = 0xDEADBEEF,
    .size = 0x2000,
    .handle_page_fault = handle_vusb_fault,
    .priv = NULL
};

/* Called by the VM to ACK a virtual IRQ */
static void
vusb_ack(void* token)
{
    /* TODO */
}

/* Callback for root hub status changes */
static int
usb_sts_change(void* token, enum usb_xact_status stat)
{
    /* TODO */
    return 1;
}

void
vm_vusb_notify(vusb_device_t* vusb)
{
    struct sel4urb *u;
    struct xact xact[3];
    enum usb_speed speed;
    int len;
    int nxact;
    u = vusb->data_regs->sel4urb;
    if (u->rate_ms) {
        printf("Currently ignoring INT packets...\n");
        return;
    }
    speed = urb_get_speed(u);
    nxact = sel4urb_to_xact(u, xact);
    if (nxact < 0) {
        printf("urb error\n");
        assert(0);
        return;
    }
    len = usb_hcd_schedule(vusb->hcd, u->addr, u->hub_addr, u->hub_port, speed, u->ep, u->max_pkt,
                           u->rate_ms, xact, nxact, NULL, NULL);
    printf("VMM) usb complete with len %d\n", len);
    u->urb_status = len;
    vm_inject_IRQ(vusb->virq);
}

vusb_device_t*
vm_install_vusb(vm_t* vm, usb_host_t* hcd, uintptr_t pbase, int virq,
                seL4_CPtr vmm_ncap, seL4_CPtr vm_ncap, int badge)
{
    vusb_device_t* vusb;
    struct device d;
    int err;

    /* Setup book keeping */
    vusb = malloc(sizeof(*vusb));
    if (vusb == NULL) {
        return NULL;
    }
    vusb->vm = vm;
    vusb->hcd = hcd;

    /* Map registers */
    vusb->data_regs = map_shared_page(vm, pbase, seL4_AllRights);
    if (vusb->data_regs == NULL) {
        /* TODO clean up ctrl_regs mapping */
        free(vusb);
        return NULL;
    }
    vusb->ctrl_regs = map_shared_page(vm, pbase + 0x1000, seL4_CanRead);
    if (vusb->ctrl_regs == NULL) {
        free(vusb);
        return NULL;
    }
    /* Initialise virtual registers */
    vusb->ctrl_regs->nPorts = VUSB_NPORTS;
    vusb->ctrl_regs->req_reply = 0;
    vusb->ctrl_regs->status = 0;

    /* Initialise virtual IRQ */
    vusb->virq = vm_virq_new(vm, virq, &vusb_ack, NULL);
    if (vusb->virq == NULL) {
        return NULL;
    }

    /* Install the device */
    d = dev_vusb;
    d.pstart = pbase;
    d.size = 0x2000;
    d.priv = vusb;
    err = vm_add_device(vm, &d);
    if (err) {
        assert(!err);
        return NULL;
    }
    err = vm_install_service(vm, vmm_ncap, vm_ncap, badge);
    if (err) {
        assert(!err);
        return NULL;
    }

    /* Schedule Root hub INT packet */
    vusb->int_xact.type = PID_INT;
    vusb->int_xact.len = (vusb->ctrl_regs->nPorts + 7) / 8;
    vusb->int_xact.paddr = 0xdeadbeef;
    vusb->int_xact.vaddr = (void*)&vusb->ctrl_regs->status;
    err = usb_hcd_schedule(vusb->hcd, 1, -1, 0, 0, 1, 2, 10 /* ms */,
                           &vusb->int_xact, 1, usb_sts_change, vusb);
    if (err) {
        assert(!err);
        return NULL;
    } else {
        return vusb;
    }
}

#endif /* CONFIG_LIB_USB */