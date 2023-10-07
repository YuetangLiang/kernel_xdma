
#ifndef _BASA_DEBUG_H_
#define	_BASA_DEBUG_H_

/* ------------------------------------------------------------------------
 * Debugging, printing and logging
 */

#define	ZYNQ_TRACE_PROBE		(1 << 0)
#define	ZYNQ_TRACE_CHAN			(1 << 1)
#define	ZYNQ_TRACE_CAN			(1 << 2)
#define	ZYNQ_TRACE_CAN_PIO		(1 << 3)
#define	ZYNQ_TRACE_CAN_MSG		(1 << 4)
#define	ZYNQ_TRACE_SYSFS		(1 << 5)
#define	ZYNQ_TRACE_FW			(1 << 6)
#define	ZYNQ_TRACE_GPS			(1 << 7)
#define	ZYNQ_TRACE_VIDEO		(1 << 8)
#define	ZYNQ_TRACE_INTR			(1 << 9)
#define	ZYNQ_TRACE_STATS		(1 << 10)
#define	ZYNQ_TRACE_REG			(1 << 29)
#define	ZYNQ_TRACE_BUF			(1 << 30)
#define	ZYNQ_TRACE_ERR			(1 << 31)

#define	zynq_trace(flag, msg...) \
	do { \
		if (zynq_trace_param & flag) \
			printk(KERN_DEBUG ZYNQ_DRV_NAME ": " msg); \
	} while (0)

#define	zynq_warn(msg...) \
	printk(KERN_WARNING ZYNQ_DRV_NAME ": " msg)

#define	zynq_err(msg...) \
	printk(KERN_ERR ZYNQ_DRV_NAME ": " msg)

#define	zynq_log(msg...) \
	printk(KERN_ERR ZYNQ_DRV_NAME ": " msg)

#ifdef ZYNQ_DEBUG
#define	ASSERT(x)							\
	if (!(x)) {							\
		printk(KERN_WARNING "ASSERT FAILED %s:%d: %s\n",	\
		    __FILE__, __LINE__, #x);				\
	}
#else
#define	ASSERT(x)
#endif

//#define zynq_stats_log(...) ({0;})
#define	ZYNQ_STATS_LOGX(p, id, count, interval)			\
		zynq_err("%s: WARNING! %s, %lu\n",		\
		    (p)->prefix, (p)->stats[id].label,	\
		    (p)->stats[id].cnt)

#define	ZYNQ_STATS_LOG(p, id)	ZYNQ_STATS_LOGX(p, id, 1, -1)
#define	ZYNQ_STATS(p, id)	((p)->stats[id].cnt++)

#define	ZYNQ_LOG_PREFIX_LEN		32
#define	ZYNQ_STATS_LABEL_LEN		24

typedef struct zynq_stats {
	unsigned long		cnt;	/* count of errors */
	unsigned long		ts;	/* jiffies of last report */
	const char		*label;
} zynq_stats_t;

//extern int zynq_stats_log(zynq_stats_t *stats, int count, int interval);

extern unsigned int zynq_trace_param;
//#define zynq_trace_param    0
//#define zynq_bringup_param  0
extern unsigned int zynq_bringup_param;

#endif	/* _BASA_DEBUG_H_ */
