/******************************************************************************
 *    COPYRIGHT (C) 2013 Czyong. Hisilicon
 *    All rights reserved.
 * ***
 *    Create by Czyong 2013-03-12
 *
******************************************************************************/

#include <linux/sched.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/err.h>
#include <linux/clk.h>
#include <linux/clockchips.h>
#include <linux/platform_device.h>
#include <linux/delay.h>
#include <linux/percpu.h>

#include <asm/localtimer.h>

#include <mach/hardware.h>
#include <mach/platform.h>
#include <mach/irqs.h>
#include <asm/hardware/timer-sp.h>
#include <asm/mach/time.h>
#include <asm/sched_clock.h>
#include <asm/hardware/arm_timer.h>
#include <asm/smp_twd.h>

#ifndef CONFIG_LOCAL_TIMERS
	#undef CONFIG_ARM_LOCAL_TIMER_HI3531A
	#undef CONFIG_SP804_LOCAL_TIMER_HI3531A
#endif

/*****************************************************************************/

struct hi_timer_t {
	int init;          /* timer initialize times */
	int index;         /* timer index */
	char *name;
	unsigned int addr; /* timer control base address */
	struct irqaction irq;
	void *priv;
};

#define HI_TIMER(_index, _irq, _addr) { \
	.init = 0, \
	.index = _index, \
	.name = "timer"#_index, \
	.addr = IO_ADDRESS(_addr), \
	.priv = NULL, \
	.irq.irq  = _irq,\
	.irq.name = "timer"#_index, \
	.irq.flags = (IRQF_TIMER | IRQF_NOBALANCING | IRQF_PERCPU), \
}

static struct hi_timer_t hi_timer[] = {
	HI_TIMER(0, INTNR_TIMER_0, TIMER0_REG_BASE),
	HI_TIMER(1, INTNR_TIMER_1, TIMER1_REG_BASE),
	HI_TIMER(2, INTNR_TIMER_2, TIMER2_REG_BASE),
	HI_TIMER(3, INTNR_TIMER_3, TIMER3_REG_BASE),
	HI_TIMER(4, INTNR_TIMER_4, TIMER4_REG_BASE),
	HI_TIMER(5, INTNR_TIMER_5, TIMER5_REG_BASE),
	HI_TIMER(6, INTNR_TIMER_6, TIMER6_REG_BASE),
	HI_TIMER(7, INTNR_TIMER_7, TIMER7_REG_BASE),
};

#define TIMER(_index)        (&hi_timer[_index])

#define GET_SMP_TIMER(_cpu)  (&hi_timer[((_cpu) << 1) + 4])
/*****************************************************************************/

static long __init sp804_get_clock_rate(const char *name)
{
	struct clk *clk;
	long rate;
	int err;

	clk = clk_get_sys("sp804", name);
	if (IS_ERR(clk)) {
		pr_err("sp804: %s clock not found: %d\n", name,
			(int)PTR_ERR(clk));
		return PTR_ERR(clk);
	}

	err = clk_prepare(clk);
	if (err) {
		pr_err("sp804: %s clock failed to prepare: %d\n", name, err);
		clk_put(clk);
		return err;
	}

	err = clk_enable(clk);
	if (err) {
		pr_err("sp804: %s clock failed to enable: %d\n", name, err);
		clk_unprepare(clk);
		clk_put(clk);
		return err;
	}

	rate = clk_get_rate(clk);
	if (rate < 0) {
		pr_err("sp804: %s clock failed to get rate: %ld\n", name, rate);
		clk_disable(clk);
		clk_unprepare(clk);
		clk_put(clk);
	}

	return rate;
}
#ifdef CONFIG_SP804_LOCAL_TIMER_HI3531A

static unsigned long local_timer_rate;

static void sp804_set_mode(enum clock_event_mode mode,
			   struct clock_event_device *evt)
{
	struct hi_timer_t *timer = GET_SMP_TIMER(smp_processor_id());
	unsigned long ctrl = TIMER_CTRL_32BIT | TIMER_CTRL_IE;
	unsigned int clkevt_base = timer->addr;

	writel(ctrl, IOMEM(clkevt_base + TIMER_CTRL));

	switch (mode) {
	case CLOCK_EVT_MODE_PERIODIC:
		writel(DIV_ROUND_CLOSEST(local_timer_rate, HZ),
		       IOMEM(clkevt_base + TIMER_LOAD));
		ctrl |= TIMER_CTRL_PERIODIC | TIMER_CTRL_ENABLE;
		break;

	case CLOCK_EVT_MODE_ONESHOT:
		/* period set, and timer enabled in 'next_event' hook */
		ctrl |= TIMER_CTRL_ONESHOT;
		break;

	case CLOCK_EVT_MODE_UNUSED:
	case CLOCK_EVT_MODE_SHUTDOWN:
	default:
		break;
	}

	writel(ctrl, IOMEM(clkevt_base + TIMER_CTRL));
}

static int sp804_set_next_event(unsigned long next,
				struct clock_event_device *evt)
{
	struct hi_timer_t *timer = GET_SMP_TIMER(smp_processor_id());
	unsigned int clkevt_base = timer->addr;
	unsigned long ctrl = readl(IOMEM(clkevt_base + TIMER_CTRL));

	writel(next, IOMEM(clkevt_base + TIMER_LOAD));
	writel(ctrl | TIMER_CTRL_ENABLE, IOMEM(clkevt_base + TIMER_CTRL));

	return 0;
}

static irqreturn_t sp804_timer_isr(int irq, void *dev_id)
{
	struct hi_timer_t *timer = (struct hi_timer_t *)dev_id;
	unsigned int clkevt_base = timer->addr;
	struct clock_event_device *evt
		= (struct clock_event_device *)timer->priv;

	/* clear the interrupt */
	writel(1, IOMEM(clkevt_base + TIMER_INTCLR));

	evt->event_handler(evt);

	return IRQ_HANDLED;
}
/*****************************************************************************/

/* every should have it's timer irq. */
static int __cpuinit hi3531a_local_timer_setup(struct clock_event_device *evt)
{
	unsigned int cpu = smp_processor_id();
	struct hi_timer_t *timer = GET_SMP_TIMER(cpu);
	struct irqaction *irq = &timer->irq;

	evt->name = timer->name;
	evt->irq  = irq->irq;
	evt->features = CLOCK_EVT_FEAT_PERIODIC | CLOCK_EVT_FEAT_ONESHOT
		| CLOCK_EVT_FEAT_C3STOP;

	evt->set_mode = sp804_set_mode;
	evt->set_next_event = sp804_set_next_event;
	evt->rating = 350;

	timer->priv = (void *)evt;

	clockevents_config_and_register(evt, local_timer_rate, 0xf, 0xffffffff);
	irq_set_affinity(evt->irq, evt->cpumask);
	enable_irq(evt->irq);

	return 0;
}

static void hi3531a_local_timer_stop(struct clock_event_device *evt)
{
	evt->set_mode(CLOCK_EVT_MODE_UNUSED, evt);
	disable_irq(evt->irq);
}
/*****************************************************************************/

static struct local_timer_ops hi3531a_timer_tick_ops __cpuinitdata = {
	.setup	= hi3531a_local_timer_setup,
	.stop	= hi3531a_local_timer_stop,
};

static void __init hi3531a_local_timer_init(void)
{
	unsigned int cpu = 0;
	unsigned int ncores = num_possible_cpus();

	local_timer_rate = sp804_get_clock_rate("sp804");

	for (cpu = 0; cpu < ncores; cpu++) {
		struct hi_timer_t *cpu_timer = GET_SMP_TIMER(cpu);

		cpu_timer->irq.handler = sp804_timer_isr;
		cpu_timer->irq.dev_id = (void *)cpu_timer;
		setup_irq(cpu_timer->irq.irq, &cpu_timer->irq);
		disable_irq(cpu_timer->irq.irq);
	}

	local_timer_register(&hi3531a_timer_tick_ops);
}
#endif /* CONFIG_SP804_LOCAL_TIMER_HI3531A */

/*****************************************************************************/
#ifdef CONFIG_ARM_LOCAL_TIMER_HI3531A
DEFINE_TWD_LOCAL_TIMER(twd_localtimer,
		(resource_size_t)(A9_PERI_BASE + REG_A9_PERI_PRI_TIMER_WDT),
		(resource_size_t)IRQ_LOCALTIMER);
#endif

/*****************************************************************************/

struct hi3531a_clocksource {
	void __iomem *base;
	struct clocksource clksrc;
};

static struct hi3531a_clocksource hi3531a_clocksource = {0};

static inline struct hi3531a_clocksource
			*to_hi3531a_clksrc(struct clocksource *cs)
{
	return container_of(cs, struct hi3531a_clocksource, clksrc);
}

static void hi3531a_clocksource_start(void __iomem *base)
{
	writel(0, IOMEM(base + TIMER_CTRL));
	writel(0xffffffff, IOMEM(base + TIMER_LOAD));
	writel(0xffffffff, IOMEM(base + TIMER_VALUE));
	writel(TIMER_CTRL_32BIT | TIMER_CTRL_ENABLE | TIMER_CTRL_PERIODIC,
		IOMEM(base + TIMER_CTRL));
}

static cycle_t hi3531a_clocksource_read(struct clocksource *cs)
{
	return ~readl_relaxed(to_hi3531a_clksrc(cs)->base + TIMER_VALUE);
}

static notrace u32 hi3531a_sched_clock_read(void)
{
	return ~readl_relaxed(hi3531a_clocksource.base + TIMER_VALUE);
}

static void hi3531a_clocksource_resume(struct clocksource *cs)
{
	hi3531a_clocksource_start(to_hi3531a_clksrc(cs)->base);
}

static void __init hi3531a_clocksource_init(void __iomem *base,
						const char *name)
{
	long rate = sp804_get_clock_rate(name);
	struct clocksource *clksrc = &hi3531a_clocksource.clksrc;

	if (rate < 0)
		return;

	clksrc->name   = name;
	clksrc->rating = 200;
	clksrc->read   = hi3531a_clocksource_read;
	clksrc->mask   = CLOCKSOURCE_MASK(32),
	clksrc->flags  = CLOCK_SOURCE_IS_CONTINUOUS,
	clksrc->resume = hi3531a_clocksource_resume,

	hi3531a_clocksource.base = base;

	hi3531a_clocksource_start(base);

	clocksource_register_hz(clksrc, rate);

	setup_sched_clock(hi3531a_sched_clock_read, 32, rate);
}

void __init hi3531a_timer_init(void)
{
	unsigned int regval;

	/* set the bus clock for all timer */
	regval = readl(__io_address(SYS_CTRL_BASE + REG_SC_CTRL));
	regval |= SC_CTRL_TIMER0_CLK_SEL(TIMER_CLK_BUS)
		| SC_CTRL_TIMER1_CLK_SEL(TIMER_CLK_BUS)
		| SC_CTRL_TIMER2_CLK_SEL(TIMER_CLK_BUS)
		| SC_CTRL_TIMER3_CLK_SEL(TIMER_CLK_BUS)
		| SC_CTRL_TIMER4_CLK_SEL(TIMER_CLK_BUS)
		| SC_CTRL_TIMER5_CLK_SEL(TIMER_CLK_BUS)
		| SC_CTRL_TIMER6_CLK_SEL(TIMER_CLK_BUS)
		| SC_CTRL_TIMER7_CLK_SEL(TIMER_CLK_BUS);
	writel(regval, __io_address(SYS_CTRL_BASE + REG_SC_CTRL));

#ifdef CONFIG_ARM_LOCAL_TIMER_HI3531A
	twd_local_timer_register(&twd_localtimer);
#endif

#ifdef CONFIG_SP804_LOCAL_TIMER_HI3531A
	hi3531a_local_timer_init();
#endif

	hi3531a_clocksource_init((void *)TIMER(0)->addr,
		TIMER(0)->name);

	sp804_clockevents_init((void *)TIMER(1)->addr,
		TIMER(1)->irq.irq, TIMER(1)->name);

}

