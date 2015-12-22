#ifndef LINUX_SSB_PRIVATE_H_
#define LINUX_SSB_PRIVATE_H_

#include <linux/ssb/ssb.h>
#include <linux/types.h>


#define PFX	"ssb: "

#ifdef CONFIG_SSB_SILENT
# define ssb_printk(fmt, x...)	do {  } while (0)
#else
# define ssb_printk		printk
#endif 

#ifdef CONFIG_SSB_DEBUG
# define ssb_dprintk(fmt, x...)	ssb_printk(fmt , ##x)
#else
# define ssb_dprintk(fmt, x...)	do {  } while (0)
#endif

#ifdef CONFIG_SSB_DEBUG
# define SSB_WARN_ON(x)		WARN_ON(x)
# define SSB_BUG_ON(x)		BUG_ON(x)
#else
static inline int __ssb_do_nothing(int x) { return x; }
# define SSB_WARN_ON(x)		__ssb_do_nothing(unlikely(!!(x)))
# define SSB_BUG_ON(x)		__ssb_do_nothing(unlikely(!!(x)))
#endif


#ifdef CONFIG_SSB_PCIHOST
extern int ssb_pci_switch_core(struct ssb_bus *bus,
			       struct ssb_device *dev);
extern int ssb_pci_switch_coreidx(struct ssb_bus *bus,
				  u8 coreidx);
extern int ssb_pci_xtal(struct ssb_bus *bus, u32 what,
			int turn_on);
extern int ssb_pci_get_invariants(struct ssb_bus *bus,
				  struct ssb_init_invariants *iv);
extern void ssb_pci_exit(struct ssb_bus *bus);
extern int ssb_pci_init(struct ssb_bus *bus);
extern const struct ssb_bus_ops ssb_pci_ops;

#else 

static inline int ssb_pci_switch_core(struct ssb_bus *bus,
				      struct ssb_device *dev)
{
	return 0;
}
static inline int ssb_pci_switch_coreidx(struct ssb_bus *bus,
					 u8 coreidx)
{
	return 0;
}
static inline int ssb_pci_xtal(struct ssb_bus *bus, u32 what,
			       int turn_on)
{
	return 0;
}
static inline void ssb_pci_exit(struct ssb_bus *bus)
{
}
static inline int ssb_pci_init(struct ssb_bus *bus)
{
	return 0;
}
#endif 


#ifdef CONFIG_SSB_PCMCIAHOST
extern int ssb_pcmcia_switch_core(struct ssb_bus *bus,
				  struct ssb_device *dev);
extern int ssb_pcmcia_switch_coreidx(struct ssb_bus *bus,
				     u8 coreidx);
extern int ssb_pcmcia_switch_segment(struct ssb_bus *bus,
				     u8 seg);
extern int ssb_pcmcia_get_invariants(struct ssb_bus *bus,
				     struct ssb_init_invariants *iv);
extern int ssb_pcmcia_hardware_setup(struct ssb_bus *bus);
extern void ssb_pcmcia_exit(struct ssb_bus *bus);
extern int ssb_pcmcia_init(struct ssb_bus *bus);
extern const struct ssb_bus_ops ssb_pcmcia_ops;
#else 
static inline int ssb_pcmcia_switch_core(struct ssb_bus *bus,
					 struct ssb_device *dev)
{
	return 0;
}
static inline int ssb_pcmcia_switch_coreidx(struct ssb_bus *bus,
					    u8 coreidx)
{
	return 0;
}
static inline int ssb_pcmcia_switch_segment(struct ssb_bus *bus,
					    u8 seg)
{
	return 0;
}
static inline int ssb_pcmcia_hardware_setup(struct ssb_bus *bus)
{
	return 0;
}
static inline void ssb_pcmcia_exit(struct ssb_bus *bus)
{
}
static inline int ssb_pcmcia_init(struct ssb_bus *bus)
{
	return 0;
}
#endif 

#ifdef CONFIG_SSB_SDIOHOST
extern int ssb_sdio_get_invariants(struct ssb_bus *bus,
				     struct ssb_init_invariants *iv);

extern u32 ssb_sdio_scan_read32(struct ssb_bus *bus, u16 offset);
extern int ssb_sdio_switch_core(struct ssb_bus *bus, struct ssb_device *dev);
extern int ssb_sdio_scan_switch_coreidx(struct ssb_bus *bus, u8 coreidx);
extern int ssb_sdio_hardware_setup(struct ssb_bus *bus);
extern void ssb_sdio_exit(struct ssb_bus *bus);
extern int ssb_sdio_init(struct ssb_bus *bus);

extern const struct ssb_bus_ops ssb_sdio_ops;
#else 
static inline u32 ssb_sdio_scan_read32(struct ssb_bus *bus, u16 offset)
{
	return 0;
}
static inline int ssb_sdio_switch_core(struct ssb_bus *bus,
					 struct ssb_device *dev)
{
	return 0;
}
static inline int ssb_sdio_scan_switch_coreidx(struct ssb_bus *bus, u8 coreidx)
{
	return 0;
}
static inline int ssb_sdio_hardware_setup(struct ssb_bus *bus)
{
	return 0;
}
static inline void ssb_sdio_exit(struct ssb_bus *bus)
{
}
static inline int ssb_sdio_init(struct ssb_bus *bus)
{
	return 0;
}
#endif 


extern const char *ssb_core_name(u16 coreid);
extern int ssb_bus_scan(struct ssb_bus *bus,
			unsigned long baseaddr);
extern void ssb_iounmap(struct ssb_bus *ssb);


extern
ssize_t ssb_attr_sprom_show(struct ssb_bus *bus, char *buf,
			    int (*sprom_read)(struct ssb_bus *bus, u16 *sprom));
extern
ssize_t ssb_attr_sprom_store(struct ssb_bus *bus,
			     const char *buf, size_t count,
			     int (*sprom_check_crc)(const u16 *sprom, size_t size),
			     int (*sprom_write)(struct ssb_bus *bus, const u16 *sprom));
extern int ssb_fill_sprom_with_fallback(struct ssb_bus *bus,
					struct ssb_sprom *out);


extern u32 ssb_calc_clock_rate(u32 plltype, u32 n, u32 m);
extern struct ssb_bus *ssb_pci_dev_to_bus(struct pci_dev *pdev);
int ssb_for_each_bus_call(unsigned long data,
			  int (*func)(struct ssb_bus *bus, unsigned long data));
extern struct ssb_bus *ssb_pcmcia_dev_to_bus(struct pcmcia_device *pdev);

struct ssb_freeze_context {
	
	struct ssb_bus *bus;
	
	bool device_frozen[SSB_MAX_NR_CORES];
};
extern int ssb_devices_freeze(struct ssb_bus *bus, struct ssb_freeze_context *ctx);
extern int ssb_devices_thaw(struct ssb_freeze_context *ctx);



#ifdef CONFIG_SSB_B43_PCI_BRIDGE
extern int __init b43_pci_ssb_bridge_init(void);
extern void __exit b43_pci_ssb_bridge_exit(void);
#else 
static inline int b43_pci_ssb_bridge_init(void)
{
	return 0;
}
static inline void b43_pci_ssb_bridge_exit(void)
{
}
#endif 

extern u32 ssb_pmu_get_cpu_clock(struct ssb_chipcommon *cc);
extern u32 ssb_pmu_get_controlclock(struct ssb_chipcommon *cc);

#endif 
