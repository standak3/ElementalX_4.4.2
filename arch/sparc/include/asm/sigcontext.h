#ifndef __SPARC_SIGCONTEXT_H
#define __SPARC_SIGCONTEXT_H

#ifdef __KERNEL__
#include <asm/ptrace.h>

#ifndef __ASSEMBLY__

#define __SUNOS_MAXWIN   31

struct sigcontext32 {
	int sigc_onstack;      
	int sigc_mask;         
	int sigc_sp;           
	int sigc_pc;           
	int sigc_npc;          
	int sigc_psr;          
	int sigc_g1;           
	int sigc_o0;           

	int sigc_oswins;       

	
	unsigned sigc_spbuf[__SUNOS_MAXWIN];

	
	struct reg_window32 sigc_wbuf[__SUNOS_MAXWIN];
};



typedef struct {
	struct {
		unsigned int psr;
		unsigned int pc;
		unsigned int npc;
		unsigned int y;
		unsigned int u_regs[16]; 
	}			si_regs;
	int			si_mask;
} __siginfo32_t;

#define __SIGC_MAXWIN	7

typedef struct {
	unsigned long locals[8];
	unsigned long ins[8];
} __siginfo_reg_window;

typedef struct {
	int			wsaved;
	__siginfo_reg_window	reg_window[__SIGC_MAXWIN];
	unsigned long		rwbuf_stkptrs[__SIGC_MAXWIN];
} __siginfo_rwin_t;

#ifdef CONFIG_SPARC64
typedef struct {
	unsigned   int si_float_regs [64];
	unsigned   long si_fsr;
	unsigned   long si_gsr;
	unsigned   long si_fprs;
} __siginfo_fpu_t;

struct sigcontext {
	
	char			sigc_info[128];
	struct {
		unsigned long	u_regs[16]; 
		unsigned long	tstate;
		unsigned long	tpc;
		unsigned long	tnpc;
		unsigned int	y;
		unsigned int	fprs;
	}			sigc_regs;
	__siginfo_fpu_t *	sigc_fpu_save;
	struct {
		void	*	ss_sp;
		int		ss_flags;
		unsigned long	ss_size;
	}			sigc_stack;
	unsigned long		sigc_mask;
	__siginfo_rwin_t *	sigc_rwin_save;
};

#else

typedef struct {
	unsigned long si_float_regs [32];
	unsigned long si_fsr;
	unsigned long si_fpqdepth;
	struct {
		unsigned long *insn_addr;
		unsigned long insn;
	} si_fpqueue [16];
} __siginfo_fpu_t;
#endif 


#endif 

#endif 

#endif 
