/*-
 * Copyright (c) 2014 Tycho Nightingale <tycho.nightingale@pluribusnetworks.com>
 * Copyright (c) 2017 Intel Corporation
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#define pr_prefix	"vpic: "

#include <hypervisor.h>

#define	VPIC_LOCK_INIT(vpic)	spinlock_init(&((vpic)->lock))
#define	VPIC_LOCK(vpic)		spinlock_obtain(&((vpic)->lock))
#define	VPIC_UNLOCK(vpic)	spinlock_release(&((vpic)->lock))
/* TODO: add spinlock_locked support? */
/*#define VPIC_LOCKED(vpic)	spinlock_locked(&((vpic)->lock))*/

#define vm_pic(vm)	(vm->vpic)

#define true                                          1
#define false                                         0

#define ACRN_DBG_PIC	6

enum irqstate {
	IRQSTATE_ASSERT,
	IRQSTATE_DEASSERT,
	IRQSTATE_PULSE
};

struct pic {
	bool		ready;
	int		icw_num;
	int		rd_cmd_reg;

	bool		aeoi;
	bool		poll;
	bool		rotate;
	bool		sfn;		/* special fully-nested mode */

	int		irq_base;
	uint8_t		request;	/* Interrupt Request Register (IIR) */
	uint8_t		service;	/* Interrupt Service (ISR) */
	uint8_t		mask;		/* Interrupt Mask Register (IMR) */
	uint8_t		smm;		/* special mask mode */

	int		acnt[8];	/* sum of pin asserts and deasserts */
	int		lowprio;	/* lowest priority irq */

	bool		intr_raised;
	uint8_t		elc;
};

struct vpic {
	struct vm		*vm;
	spinlock_t	lock;
	struct pic	pic[2];
};

/*
 * Loop over all the pins in priority order from highest to lowest.
 */
#define	PIC_PIN_FOREACH(pinvar, pic, tmpvar)			\
	for (tmpvar = 0, pinvar = (pic->lowprio + 1) & 0x7;	\
	    tmpvar < 8;						\
	    tmpvar++, pinvar = (pinvar + 1) & 0x7)

static void vpic_set_pinstate(struct vpic *vpic, int pin, bool newstate);

static inline bool master_pic(struct vpic *vpic, struct pic *pic)
{

	if (pic == &vpic->pic[0])
		return true;
	else
		return false;
}

static inline int vpic_get_highest_isrpin(struct pic *pic)
{
	int bit, pin;
	int i;

	PIC_PIN_FOREACH(pin, pic, i) {
		bit = (1 << pin);

		if (pic->service & bit) {
			/*
			 * An IS bit that is masked by an IMR bit will not be
			 * cleared by a non-specific EOI in Special Mask Mode.
			 */
			if (pic->smm && (pic->mask & bit) != 0)
				continue;
			else
				return pin;
		}
	}

	return -1;
}

static inline int vpic_get_highest_irrpin(struct pic *pic)
{
	int serviced;
	int bit, pin, tmp;

	/*
	 * In 'Special Fully-Nested Mode' when an interrupt request from
	 * a slave is in service, the slave is not locked out from the
	 * master's priority logic.
	 */
	serviced = pic->service;
	if (pic->sfn)
		serviced &= ~(1 << 2);

	/*
	 * In 'Special Mask Mode', when a mask bit is set in OCW1 it inhibits
	 * further interrupts at that level and enables interrupts from all
	 * other levels that are not masked. In other words the ISR has no
	 * bearing on the levels that can generate interrupts.
	 */
	if (pic->smm)
		serviced = 0;

	PIC_PIN_FOREACH(pin, pic, tmp) {
		bit = 1 << pin;

		/*
		 * If there is already an interrupt in service at the same
		 * or higher priority then bail.
		 */
		if ((serviced & bit) != 0)
			break;

		/*
		 * If an interrupt is asserted and not masked then return
		 * the corresponding 'pin' to the caller.
		 */
		if ((pic->request & bit) != 0 && (pic->mask & bit) == 0)
			return pin;
	}

	return -1;
}

static void vpic_notify_intr(struct vpic *vpic)
{
	struct pic *pic;
	int pin;

	/*
	 * First check the slave.
	 */
	pic = &vpic->pic[1];
	pin = vpic_get_highest_irrpin(pic);
	if (!pic->intr_raised && pin != -1) {
		dev_dbg(ACRN_DBG_PIC,
		"pic slave notify pin = %d (imr 0x%x irr 0x%x isr 0x%x)\n",
		pin, pic->mask, pic->request, pic->service);

		/*
		 * Cascade the request from the slave to the master.
		 */
		pic->intr_raised = true;
		vpic_set_pinstate(vpic, 2, true);
		vpic_set_pinstate(vpic, 2, false);
	} else {
		dev_dbg(ACRN_DBG_PIC,
		"pic slave no eligible interrupt (imr 0x%x irr 0x%x isr 0x%x)",
		pic->mask, pic->request, pic->service);
	}

	/*
	 * Then check the master.
	 */
	pic = &vpic->pic[0];
	pin = vpic_get_highest_irrpin(pic);
	if (!pic->intr_raised && pin != -1) {
		dev_dbg(ACRN_DBG_PIC,
		"pic master notify pin = %d (imr 0x%x irr 0x%x isr 0x%x)\n",
		pin, pic->mask, pic->request, pic->service);

		/*
		 * From Section 3.6.2, "Interrupt Modes", in the
		 * MPtable Specification, Version 1.4
		 *
		 * PIC interrupts are routed to both the Local APIC
		 * and the I/O APIC to support operation in 1 of 3
		 * modes.
		 *
		 * 1. Legacy PIC Mode: the PIC effectively bypasses
		 * all APIC components.  In this mode the local APIC is
		 * disabled and LINT0 is reconfigured as INTR to
		 * deliver the PIC interrupt directly to the CPU.
		 *
		 * 2. Virtual Wire Mode: the APIC is treated as a
		 * virtual wire which delivers interrupts from the PIC
		 * to the CPU.  In this mode LINT0 is programmed as
		 * ExtINT to indicate that the PIC is the source of
		 * the interrupt.
		 *
		 * 3. Virtual Wire Mode via I/O APIC: PIC interrupts are
		 * fielded by the I/O APIC and delivered to the appropriate
		 * CPU.  In this mode the I/O APIC input 0 is programmed
		 * as ExtINT to indicate that the PIC is the source of the
		 * interrupt.
		 */
		pic->intr_raised = true;
		if (vpic->vm->vpic_wire_mode == VPIC_WIRE_INTR) {
			struct vcpu *vcpu = vcpu_from_vid(vpic->vm, 0);

			ASSERT(vcpu != NULL, "vm%d, vcpu0", vpic->vm->attr.id);
			vcpu_inject_extint(vcpu);
		} else {
			vlapic_set_local_intr(vpic->vm, -1, APIC_LVT_LINT0);
			/* notify vioapic pin0 if existing
			 * For vPIC + vIOAPIC mode, vpic master irq connected
			 * to vioapic pin0 (irq2)
			 * From MPSpec session 5.1
			 */
			vioapic_pulse_irq(vpic->vm, 0);
		}
	} else {
		dev_dbg(ACRN_DBG_PIC,
		"pic master no eligible interrupt (imr 0x%x irr 0x%x isr 0x%x)",
		pic->mask, pic->request, pic->service);
	}
}

static int vpic_icw1(__unused struct vpic *vpic, struct pic *pic, uint8_t val)
{
	dev_dbg(ACRN_DBG_PIC, "vm 0x%x: pic icw1 0x%x\n",
		vpic->vm, val);

	pic->ready = false;

	pic->icw_num = 1;
	pic->request = 0;
	pic->mask = 0;
	pic->lowprio = 7;
	pic->rd_cmd_reg = 0;
	pic->poll = 0;
	pic->smm = 0;

	if ((val & ICW1_SNGL) != 0) {
		dev_dbg(ACRN_DBG_PIC, "vpic cascade mode required\n");
		return -1;
	}

	if ((val & ICW1_IC4) == 0) {
		dev_dbg(ACRN_DBG_PIC, "vpic icw4 required\n");
		return -1;
	}

	pic->icw_num++;

	return 0;
}

static int vpic_icw2(__unused struct vpic *vpic, struct pic *pic, uint8_t val)
{
	dev_dbg(ACRN_DBG_PIC, "vm 0x%x: pic icw2 0x%x\n",
		vpic->vm, val);

	pic->irq_base = val & 0xf8;

	pic->icw_num++;

	return 0;
}

static int vpic_icw3(__unused struct vpic *vpic, struct pic *pic,
		__unused uint8_t val)
{
	dev_dbg(ACRN_DBG_PIC, "vm 0x%x: pic icw3 0x%x\n",
		vpic->vm, val);

	pic->icw_num++;

	return 0;
}

static int vpic_icw4(struct vpic *vpic, struct pic *pic, uint8_t val)
{
	dev_dbg(ACRN_DBG_PIC, "vm 0x%x: pic icw4 0x%x\n",
		vpic->vm, val);

	if ((val & ICW4_8086) == 0) {
		dev_dbg(ACRN_DBG_PIC,
			"vpic microprocessor mode required\n");
		return -1;
	}

	if ((val & ICW4_AEOI) != 0)
		pic->aeoi = true;

	if ((val & ICW4_SFNM) != 0) {
		if (master_pic(vpic, pic)) {
			pic->sfn = true;
		} else {
			dev_dbg(ACRN_DBG_PIC,
			"Ignoring special fully nested mode on slave pic: %#x",
			val);
		}
	}

	pic->icw_num = 0;
	pic->ready = true;

	return 0;
}

bool vpic_is_pin_mask(struct vpic *vpic, uint8_t virt_pin)
{
	struct pic *pic;

	if (virt_pin < 8)
		pic = &vpic->pic[0];
	else if (virt_pin < 16) {
		pic = &vpic->pic[1];
		virt_pin -= 8;
	} else
		return true;

	if (pic->mask & (1 << virt_pin))
		return true;
	else
		return false;
}

static int vpic_ocw1(struct vpic *vpic, struct pic *pic, uint8_t val)
{
	int pin, i, bit;
	uint8_t old = pic->mask;

	dev_dbg(ACRN_DBG_PIC, "vm 0x%x: pic ocw1 0x%x\n",
		vpic->vm, val);

	pic->mask = val & 0xff;

	/* query and setup if pin/irq is for passthrough device */
	PIC_PIN_FOREACH(pin, pic, i) {
		bit = (1 << pin);

		/* remap for active: interrupt mask -> unmask
		 * remap for deactive: when vIOAPIC take it over
		 */
		if (((pic->mask & bit) == 0) && (old & bit)) {
			struct ptdev_intx_info intx;

			/* master pic pin2 connect with slave pic,
			 * not device, so not need pt remap
			 */
			if ((pin == 2) && master_pic(vpic, pic))
				continue;

			intx.virt_pin = pin;
			intx.vpin_src = PTDEV_VPIN_PIC;
			if (!master_pic(vpic, pic))
				intx.virt_pin += 8;
			ptdev_intx_pin_remap(vpic->vm, &intx);
		}
	}

	return 0;
}

static int vpic_ocw2(struct vpic *vpic, struct pic *pic, uint8_t val)
{
	dev_dbg(ACRN_DBG_PIC, "vm 0x%x: pic ocw2 0x%x\n",
		vpic->vm, val);

	pic->rotate = ((val & OCW2_R) != 0);

	if ((val & OCW2_EOI) != 0) {
		int isr_bit;

		if ((val & OCW2_SL) != 0) {
			/* specific EOI */
			isr_bit = val & 0x7;
		} else {
			/* non-specific EOI */
			isr_bit = vpic_get_highest_isrpin(pic);
		}

		if (isr_bit != -1) {
			pic->service &= ~(1 << isr_bit);

			if (pic->rotate)
				pic->lowprio = isr_bit;
		}

		/* if level ack PTDEV */
		if (pic->elc & (1 << (isr_bit & 0x7))) {
			ptdev_intx_ack(vpic->vm,
				master_pic(vpic, pic) ? isr_bit : isr_bit + 8,
				PTDEV_VPIN_PIC);
		}
	} else if ((val & OCW2_SL) != 0 && pic->rotate == true) {
		/* specific priority */
		pic->lowprio = val & 0x7;
	}

	return 0;
}

static int vpic_ocw3(__unused struct vpic *vpic, struct pic *pic, uint8_t val)
{
	dev_dbg(ACRN_DBG_PIC, "vm 0x%x: pic ocw3 0x%x\n",
		vpic->vm, val);

	if (val & OCW3_ESMM) {
		pic->smm = val & OCW3_SMM ? 1 : 0;
		dev_dbg(ACRN_DBG_PIC, "%s pic special mask mode %s\n",
		    master_pic(vpic, pic) ? "master" : "slave",
		    pic->smm ?  "enabled" : "disabled");
	}

	if (val & OCW3_RR) {
		/* read register command */
		pic->rd_cmd_reg = val & OCW3_RIS;

		/* Polling mode */
		pic->poll = ((val & OCW3_P) != 0);
	}

	return 0;
}

static void vpic_set_pinstate(struct vpic *vpic, int pin, bool newstate)
{
	struct pic *pic;
	int oldcnt, newcnt;
	bool level;

	ASSERT(pin >= 0 && pin < 16,
	    "vpic_set_pinstate: invalid pin number");

	pic = &vpic->pic[pin >> 3];

	oldcnt = pic->acnt[pin & 0x7];
	if (newstate)
		pic->acnt[pin & 0x7]++;
	else
		pic->acnt[pin & 0x7]--;
	newcnt = pic->acnt[pin & 0x7];

	if (newcnt < 0) {
		pr_warn("pic pin%d: bad acnt %d\n", pin, newcnt);
	}

	level = ((vpic->pic[pin >> 3].elc & (1 << (pin & 0x7))) != 0);

	if ((oldcnt == 0 && newcnt == 1) || (newcnt > 0 && level == true)) {
		/* rising edge or level */
		dev_dbg(ACRN_DBG_PIC, "pic pin%d: asserted\n", pin);
		pic->request |= (1 << (pin & 0x7));
	} else if (oldcnt == 1 && newcnt == 0) {
		/* falling edge */
		dev_dbg(ACRN_DBG_PIC, "pic pin%d: deasserted\n", pin);
		if (level)
			pic->request &= ~(1 << (pin & 0x7));
	} else {
		dev_dbg(ACRN_DBG_PIC,
			"pic pin%d: %s, ignored, acnt %d\n",
			pin, newstate ? "asserted" : "deasserted", newcnt);
	}

	vpic_notify_intr(vpic);
}

static int vpic_set_irqstate(struct vm *vm, int irq, enum irqstate irqstate)
{
	struct vpic *vpic;
	struct pic *pic;

	if (irq < 0 || irq > 15)
		return -EINVAL;

	vpic = vm_pic(vm);
	pic = &vpic->pic[irq >> 3];

	if (pic->ready == false)
		return 0;

	VPIC_LOCK(vpic);
	switch (irqstate) {
	case IRQSTATE_ASSERT:
		vpic_set_pinstate(vpic, irq, true);
		break;
	case IRQSTATE_DEASSERT:
		vpic_set_pinstate(vpic, irq, false);
		break;
	case IRQSTATE_PULSE:
		vpic_set_pinstate(vpic, irq, true);
		vpic_set_pinstate(vpic, irq, false);
		break;
	default:
		ASSERT(0, "vpic_set_irqstate: invalid irqstate");
	}
	VPIC_UNLOCK(vpic);

	return 0;
}

/* hypervisor interface: assert/deassert/pulse irq */
int vpic_assert_irq(struct vm *vm, int irq)
{
	return vpic_set_irqstate(vm, irq, IRQSTATE_ASSERT);
}

int vpic_deassert_irq(struct vm *vm, int irq)
{
	return vpic_set_irqstate(vm, irq, IRQSTATE_DEASSERT);
}

int vpic_pulse_irq(struct vm *vm, int irq)
{
	return vpic_set_irqstate(vm, irq, IRQSTATE_PULSE);
}

int vpic_set_irq_trigger(struct vm *vm, int irq, enum vpic_trigger trigger)
{
	struct vpic *vpic;

	if (irq < 0 || irq > 15)
		return -EINVAL;

	/*
	 * See comment in vpic_elc_handler.  These IRQs must be
	 * edge triggered.
	 */
	if (trigger == LEVEL_TRIGGER) {
		switch (irq) {
		case 0:
		case 1:
		case 2:
		case 8:
		case 13:
			return -EINVAL;
		}
	}

	vpic = vm_pic(vm);

	VPIC_LOCK(vpic);

	if (trigger == LEVEL_TRIGGER)
		vpic->pic[irq >> 3].elc |=  1 << (irq & 0x7);
	else
		vpic->pic[irq >> 3].elc &=  ~(1 << (irq & 0x7));

	VPIC_UNLOCK(vpic);

	return 0;
}

int vpic_get_irq_trigger(struct vm *vm, int irq, enum vpic_trigger *trigger)
{
	struct vpic *vpic;

	if (irq < 0 || irq > 15)
		return -EINVAL;

	vpic = vm_pic(vm);
	if (!vpic)
		return -EINVAL;

	if (vpic->pic[irq>>3].elc & (1 << (irq & 0x7)))
		*trigger = LEVEL_TRIGGER;
	else
		*trigger = EDGE_TRIGGER;
	return 0;
}

void vpic_pending_intr(struct vm *vm, int *vecptr)
{
	struct vpic *vpic;
	struct pic *pic;
	int pin;

	vpic = vm_pic(vm);

	pic = &vpic->pic[0];

	VPIC_LOCK(vpic);

	pin = vpic_get_highest_irrpin(pic);
	if (pin == 2) {
		pic = &vpic->pic[1];
		pin = vpic_get_highest_irrpin(pic);
	}

	/*
	 * If there are no pins active at this moment then return the spurious
	 * interrupt vector instead.
	 */
	if (pin == -1) {
		*vecptr = -1;
		VPIC_UNLOCK(vpic);
		return;
	}

	ASSERT(pin >= 0 && pin <= 7, "invalid pin");
	*vecptr = pic->irq_base + pin;

	dev_dbg(ACRN_DBG_PIC, "Got pending vector 0x%x\n", *vecptr);

	VPIC_UNLOCK(vpic);
}

static void vpic_pin_accepted(struct pic *pic, int pin)
{
	pic->intr_raised = false;

	if ((pic->elc & (1 << pin)) == 0) {
		/*only used edge trigger mode*/
		pic->request &= ~(1 << pin);
	}

	if (pic->aeoi == true) {
		if (pic->rotate == true)
			pic->lowprio = pin;
	} else {
		pic->service |= (1 << pin);
	}
}

void vpic_intr_accepted(struct vm *vm, int vector)
{
	struct vpic *vpic;
	int pin;

	vpic = vm_pic(vm);

	VPIC_LOCK(vpic);

	pin = vector & 0x7;

	if ((vector & ~0x7) == vpic->pic[1].irq_base) {
		vpic_pin_accepted(&vpic->pic[1], pin);
		/*
		 * If this vector originated from the slave,
		 * accept the cascaded interrupt too.
		 */
		vpic_pin_accepted(&vpic->pic[0], 2);
	} else {
		vpic_pin_accepted(&vpic->pic[0], pin);
	}

	vpic_notify_intr(vpic);

	VPIC_UNLOCK(vpic);
}

static int vpic_read(struct vpic *vpic, struct pic *pic,
		int port, uint32_t *eax)
{
	int pin;

	VPIC_LOCK(vpic);

	if (pic->poll) {
		pic->poll = 0;
		pin = vpic_get_highest_irrpin(pic);
		if (pin >= 0) {
			vpic_pin_accepted(pic, pin);
			*eax = 0x80 | pin;
		} else {
			*eax = 0;
		}
	} else {
		if (port & ICU_IMR_OFFSET) {
			/* read interrupt mask register */
			*eax = pic->mask;
		} else {
			if (pic->rd_cmd_reg == OCW3_RIS) {
				/* read interrupt service register */
				*eax = pic->service;
			} else {
				/* read interrupt request register */
				*eax = pic->request;
			}
		}
	}

	VPIC_UNLOCK(vpic);

	return 0;
}

static int vpic_write(struct vpic *vpic, struct pic *pic,
		int port, uint32_t *eax)
{
	int error;
	uint8_t val;

	error = 0;
	val = *eax;

	VPIC_LOCK(vpic);

	if (port & ICU_IMR_OFFSET) {
		switch (pic->icw_num) {
		case 2:
			error = vpic_icw2(vpic, pic, val);
			break;
		case 3:
			error = vpic_icw3(vpic, pic, val);
			break;
		case 4:
			error = vpic_icw4(vpic, pic, val);
			break;
		default:
			error = vpic_ocw1(vpic, pic, val);
			break;
		}
	} else {
		if (val & (1 << 4))
			error = vpic_icw1(vpic, pic, val);

		if (pic->ready) {
			if (val & (1 << 3))
				error = vpic_ocw3(vpic, pic, val);
			else
				error = vpic_ocw2(vpic, pic, val);
		}
	}

	if (pic->ready)
		vpic_notify_intr(vpic);

	VPIC_UNLOCK(vpic);

	return error;
}

static int vpic_master_handler(struct vm *vm, bool in, int port, int bytes,
		uint32_t *eax)
{
	struct vpic *vpic;
	struct pic *pic;

	vpic = vm_pic(vm);
	pic = &vpic->pic[0];

	if (bytes != 1)
		return -1;

	if (in)
		return vpic_read(vpic, pic, port, eax);

	return vpic_write(vpic, pic, port, eax);
}

static uint32_t vpic_master_io_read(__unused struct vm_io_handler *hdlr,
		struct vm *vm, uint16_t addr, size_t width)
{
	uint32_t val = 0;

	if (vpic_master_handler(vm, true, (int)addr, (int)width, &val) < 0)
		pr_err("pic master read port 0x%x width=%d failed\n",
				addr, width);
	return val;
}

static void vpic_master_io_write(__unused struct vm_io_handler *hdlr,
		struct vm *vm, uint16_t addr, size_t width, uint32_t v)
{
	uint32_t val = v;

	if (vpic_master_handler(vm, false, (int)addr, (int)width, &val) < 0)
		pr_err("%s: write port 0x%x width=%d value 0x%x failed\n",
				__func__, addr, width, val);
}

static int vpic_slave_handler(struct vm *vm, bool in, int port, int bytes,
		uint32_t *eax)
{
	struct vpic *vpic;
	struct pic *pic;

	vpic = vm_pic(vm);
	pic = &vpic->pic[1];

	if (bytes != 1)
		return -1;

	if (in)
		return vpic_read(vpic, pic, port, eax);

	return vpic_write(vpic, pic, port, eax);
}

static uint32_t vpic_slave_io_read(__unused struct vm_io_handler *hdlr,
		struct vm *vm, uint16_t addr, size_t width)
{
	uint32_t val = 0;

	if (vpic_slave_handler(vm, true, (int)addr, (int)width, &val) < 0)
		pr_err("pic slave read port 0x%x width=%d failed\n",
				addr, width);
	return val;
}

static void vpic_slave_io_write(__unused struct vm_io_handler *hdlr,
		struct vm *vm, uint16_t addr, size_t width, uint32_t v)
{
	uint32_t val = v;

	if (vpic_slave_handler(vm, false, (int)addr, (int)width, &val) < 0)
		pr_err("%s: write port 0x%x width=%d value 0x%x failed\n",
				__func__, addr, width, val);
}

static int vpic_elc_handler(struct vm *vm, bool in, int port, int bytes,
		uint32_t *eax)
{
	struct vpic *vpic;
	bool is_master;

	vpic = vm_pic(vm);
	is_master = (port == IO_ELCR1);

	if (bytes != 1)
		return -1;

	VPIC_LOCK(vpic);

	if (in) {
		if (is_master)
			*eax = vpic->pic[0].elc;
		else
			*eax = vpic->pic[1].elc;
	} else {
		/*
		 * For the master PIC the cascade channel (IRQ2), the
		 * heart beat timer (IRQ0), and the keyboard
		 * controller (IRQ1) cannot be programmed for level
		 * mode.
		 *
		 * For the slave PIC the real time clock (IRQ8) and
		 * the floating point error interrupt (IRQ13) cannot
		 * be programmed for level mode.
		 */
		if (is_master)
			vpic->pic[0].elc = (*eax & 0xf8);
		else
			vpic->pic[1].elc = (*eax & 0xde);
	}

	VPIC_UNLOCK(vpic);

	return 0;
}

static uint32_t vpic_elc_io_read(__unused struct vm_io_handler *hdlr,
		struct vm *vm, uint16_t addr, size_t width)
{
	uint32_t val = 0;

	if (vpic_elc_handler(vm, true, (int)addr, (int)width, &val) < 0)
		pr_err("pic elc read port 0x%x width=%d failed", addr, width);
	return val;
}

static void vpic_elc_io_write(__unused struct vm_io_handler *hdlr,
		struct vm *vm, uint16_t addr, size_t width, uint32_t v)
{
	uint32_t val = v;

	if (vpic_elc_handler(vm, false, (int)addr, (int)width, &val) < 0)
		pr_err("%s: write port 0x%x width=%d value 0x%x failed\n",
				__func__, addr, width, val);
}

void vpic_register_io_handler(struct vm *vm)
{
	struct vm_io_range master_range = {
		.flags = IO_ATTR_RW,
		.base = 0x20,
		.len = 2
	};
	struct vm_io_range slave_range = {
		.flags = IO_ATTR_RW,
		.base = 0xa0,
		.len = 2
	};
	struct vm_io_range elcr_range = {
		.flags = IO_ATTR_RW,
		.base = 0x4d0,
		.len = 2
	};

	register_io_emulation_handler(vm, &master_range,
			&vpic_master_io_read, &vpic_master_io_write);
	register_io_emulation_handler(vm, &slave_range,
			&vpic_slave_io_read, &vpic_slave_io_write);
	register_io_emulation_handler(vm, &elcr_range,
			&vpic_elc_io_read, &vpic_elc_io_write);
}

void *vpic_init(struct vm *vm)
{
	struct vpic *vpic;

	vpic_register_io_handler(vm);

	vpic = calloc(1, sizeof(struct vpic));
	ASSERT(vpic != NULL, "");
	vpic->vm = vm;
	vpic->pic[0].mask = 0xff;
	vpic->pic[1].mask = 0xff;

	VPIC_LOCK_INIT(vpic);

	return vpic;
}

void vpic_cleanup(struct vm *vm)
{
	if (vm->vpic) {
		free(vm->vpic);
		vm->vpic = NULL;
	}
}
