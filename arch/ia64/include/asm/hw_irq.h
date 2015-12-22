#ifndef _ASM_IA64_HW_IRQ_H
#define _ASM_IA64_HW_IRQ_H

/*
 * Copyright (C) 2001-2003 Hewlett-Packard Co
 *	David Mosberger-Tang <davidm@hpl.hp.com>
 */

#include <linux/interrupt.h>
#include <linux/sched.h>
#include <linux/types.h>
#include <linux/profile.h>

#include <asm/machvec.h>
#include <asm/ptrace.h>
#include <asm/smp.h>

#ifndef CONFIG_PARAVIRT
typedef u8 ia64_vector;
#else
typedef u16 ia64_vector;
#endif

#define IA64_MIN_VECTORED_IRQ		 16
#define IA64_MAX_VECTORED_IRQ		255
#define IA64_NUM_VECTORS		256

#define AUTO_ASSIGN			-1

#define IA64_SPURIOUS_INT_VECTOR	0x0f

#define IA64_CPEP_VECTOR		0x1c	
#define IA64_CMCP_VECTOR		0x1d	
#define IA64_CPE_VECTOR			0x1e	
#define IA64_CMC_VECTOR			0x1f	
extern int ia64_first_device_vector;
extern int ia64_last_device_vector;

#if defined(CONFIG_SMP) && (defined(CONFIG_IA64_GENERIC) || defined (CONFIG_IA64_DIG))
#define IA64_IRQ_MOVE_VECTOR		0x30	
#define IA64_DEF_FIRST_DEVICE_VECTOR	0x31
#else
#define IA64_DEF_FIRST_DEVICE_VECTOR	0x30
#endif
#define IA64_DEF_LAST_DEVICE_VECTOR	0xe7
#define IA64_FIRST_DEVICE_VECTOR	ia64_first_device_vector
#define IA64_LAST_DEVICE_VECTOR		ia64_last_device_vector
#define IA64_MAX_DEVICE_VECTORS		(IA64_DEF_LAST_DEVICE_VECTOR - IA64_DEF_FIRST_DEVICE_VECTOR + 1)
#define IA64_NUM_DEVICE_VECTORS		(IA64_LAST_DEVICE_VECTOR - IA64_FIRST_DEVICE_VECTOR + 1)

#define IA64_MCA_RENDEZ_VECTOR		0xe8	
#define IA64_PERFMON_VECTOR		0xee	
#define IA64_TIMER_VECTOR		0xef	
#define	IA64_MCA_WAKEUP_VECTOR		0xf0	
#define IA64_IPI_LOCAL_TLB_FLUSH	0xfc	
#define IA64_IPI_RESCHEDULE		0xfd	
#define IA64_IPI_VECTOR			0xfe	


#define IA64_IRQ_REDIRECTED		(1 << 31)


#define IA64_IPI_DEFAULT_BASE_ADDR	0xfee00000

enum {
        IA64_IPI_DM_INT =       0x0,    
        IA64_IPI_DM_PMI =       0x2,    
        IA64_IPI_DM_NMI =       0x4,    
        IA64_IPI_DM_INIT =      0x5,    
        IA64_IPI_DM_EXTINT =    0x7,    
};

extern __u8 isa_irq_to_vector_map[16];
#define isa_irq_to_vector(x)	isa_irq_to_vector_map[(x)]

struct irq_cfg {
	ia64_vector vector;
	cpumask_t domain;
	cpumask_t old_domain;
	unsigned move_cleanup_count;
	u8 move_in_progress : 1;
};
extern spinlock_t vector_lock;
extern struct irq_cfg irq_cfg[NR_IRQS];
#define irq_to_domain(x)	irq_cfg[(x)].domain
DECLARE_PER_CPU(int[IA64_NUM_VECTORS], vector_irq);

extern struct irq_chip irq_type_ia64_lsapic;	

#ifdef CONFIG_PARAVIRT_GUEST
#include <asm/paravirt.h>
#else
#define ia64_register_ipi	ia64_native_register_ipi
#define assign_irq_vector	ia64_native_assign_irq_vector
#define free_irq_vector		ia64_native_free_irq_vector
#define register_percpu_irq	ia64_native_register_percpu_irq
#define ia64_resend_irq		ia64_native_resend_irq
#endif

extern void ia64_native_register_ipi(void);
extern int bind_irq_vector(int irq, int vector, cpumask_t domain);
extern int ia64_native_assign_irq_vector (int irq);	
extern void ia64_native_free_irq_vector (int vector);
extern int reserve_irq_vector (int vector);
extern void __setup_vector_irq(int cpu);
extern void ia64_send_ipi (int cpu, int vector, int delivery_mode, int redirect);
extern void ia64_native_register_percpu_irq (ia64_vector vec, struct irqaction *action);
extern int check_irq_used (int irq);
extern void destroy_and_reserve_irq (unsigned int irq);

#if defined(CONFIG_SMP) && (defined(CONFIG_IA64_GENERIC) || defined(CONFIG_IA64_DIG))
extern int irq_prepare_move(int irq, int cpu);
extern void irq_complete_move(unsigned int irq);
#else
static inline int irq_prepare_move(int irq, int cpu) { return 0; }
static inline void irq_complete_move(unsigned int irq) {}
#endif

static inline void ia64_native_resend_irq(unsigned int vector)
{
	platform_send_ipi(smp_processor_id(), vector, IA64_IPI_DM_INT, 0);
}

#ifndef CONFIG_IA64_GENERIC
static inline ia64_vector __ia64_irq_to_vector(int irq)
{
	return irq_cfg[irq].vector;
}

static inline unsigned int
__ia64_local_vector_to_irq (ia64_vector vec)
{
	return __get_cpu_var(vector_irq)[vec];
}
#endif


static inline ia64_vector
irq_to_vector (int irq)
{
	return platform_irq_to_vector(irq);
}

static inline unsigned int
local_vector_to_irq (ia64_vector vec)
{
	return platform_local_vector_to_irq(vec);
}

#endif 
