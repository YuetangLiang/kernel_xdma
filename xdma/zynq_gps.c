/*
 * [basa_driver::zynq_create_cdev_all()] -> [basa_main::zynq_create_cdev()] -> zynq_gps_fops
 *
 * zynq_gps_init() -> zynq_gps_thread_init() -> zynq_gps_sync_thread()
 *
 * ZYNQ_IOC_GPS_FPGA_INIT ->  zynq_gps_init_fpga_time()
 */

#include <linux/module.h>
#include <linux/kobject.h>
#include <linux/delay.h>
#include <linux/time.h>
#include <linux/hrtimer.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <uapi/linux/sched/types.h>
#include "basa.h"

//spinlock_t zynq_gps_lock;

static zynq_dev_t *zynq_gps_master_dev = NULL;
static struct task_struct *zynq_gps_taskp = NULL;
static struct completion zynq_gps_wait;
static int zynq_gps_valid_once = 0;

/*
 * GPS sync time interval in seconds: 0 to disable; default 60.
 */
static unsigned int zynq_gps_sync_time = ZYNQ_GPS_SYNC_TIME_DEFAULT;
static unsigned int zynq_gps_smooth_step = ZYNQ_GPS_SMOOTH_STEP_DEFAULT;
static unsigned int zynq_gps_smooth_max = 1000;		/* ms */
static unsigned int zynq_gps_checksum = 0;
/*
 * We define two water marks for GPS/SYS time gap examination (usec per sec).
 */
static unsigned int zynq_gps_high_factor = 100000;
static unsigned int zynq_gps_low_factor = 100;

static int zynq_get_gps_time(zynq_dev_t *zdev, u64 *gps_val)
{
    u32 lo,hi;
    spin_lock(&zdev->zdev_lock);
    lo = zynq_g_reg_read(zdev, ZYNQ_G_NTP_LO);
    hi = zynq_g_reg_read(zdev, ZYNQ_G_NTP_HI);
    gps_val[0] = (u64)hi << 32 | lo;
    if (gps_val[0] == -1) {
        spin_unlock(&zdev->zdev_lock);
        zynq_err("%d %s: Failed to read GPS time\n",
                 zdev->zdev_inst, __FUNCTION__);
        return -1;
    }

    if (!(gps_val[0] & ZYNQ_GPS_TIME_VALID)) {
        if (ZYNQ_CAP(zdev, GPS_SMOOTH) && !zynq_gps_valid_once) {
            u32 val = zynq_g_reg_read(zdev,
                                      ZYNQ_G_GPS_LAST_TIME_LO);
            zynq_trace(ZYNQ_TRACE_PROBE,
                       "%d %s: gps_last_time_lo=0x%x\n",
                       zdev->zdev_inst, __FUNCTION__, val);
            if (val & ZYNQ_GPS_TIME_VALID) {
                zynq_gps_valid_once = 1;
            } else {
                val = zynq_g_reg_read(zdev, ZYNQ_G_GPS_STATUS);
                if (val & ZYNQ_GPS_INIT_SET) {
                    zynq_gps_valid_once = 1;
                }
            }
        }
        if (ZYNQ_CAP(zdev, GPS_SMOOTH) && zynq_gps_valid_once) {
            zynq_trace(ZYNQ_TRACE_PROBE,
                       "%d %s: GPS time is not valid, "
                       "FPGA internal time is used\n",
                       zdev->zdev_inst, __FUNCTION__);
        } else {
            spin_unlock(&zdev->zdev_lock);
            zynq_err("%d %s: GPS time is not valid\n",
                     zdev->zdev_inst, __FUNCTION__);
            return -1;
        }
    } else if (!zynq_gps_valid_once) {
        zynq_gps_valid_once = 1;
    }

	spin_unlock(&zdev->zdev_lock);

	zynq_trace(ZYNQ_TRACE_PROBE,
	    "%d %s: gps_ss_100us_valid=0x%llx\n",
	    zdev->zdev_inst, __FUNCTION__, gps_val[0]);

	return 0;
}

static int zynq_get_gprmc(zynq_dev_t *zdev, u32 *gprmc_val)
{
    /* 'CAFE': GPS is connected but not locked */
    u32 inval_gprmc = 0x45464143;
    /* 'DEAD': GPS is not connected */
    u32 dead_gprmc = 0x44414544;
    int i;

    spin_lock(&zdev->zdev_lock);
    gprmc_val[0] = ntohl(zynq_g_reg_read(zdev, ZYNQ_G_GPRMC));
    if (gprmc_val[0] == -1 || gprmc_val[0] == dead_gprmc) {
        spin_unlock(&zdev->zdev_lock);
        zynq_err("%d %s: GPS is not connected\n",
                 zdev->zdev_inst, __FUNCTION__);
        return -1;
    }

    if (gprmc_val[0] == inval_gprmc) {
        spin_unlock(&zdev->zdev_lock);
        zynq_err("%d %s: GPS is not locked\n",
                 zdev->zdev_inst, __FUNCTION__);
        return -1;
    }

    for (i = 1; i < ZYNQ_GPS_GPRMC_VAL_SZ/4; i++) {
        gprmc_val[i] = ntohl(zynq_g_reg_read(zdev, ZYNQ_G_GPRMC));
    }
    spin_unlock(&zdev->zdev_lock);
    
    return 0;
}

/*
 * convert the GPS time from FPGA registers to "struct timespec"
 */
static void zynq_gps_time_ts(u64 *gps_val, struct timespec *ts)
{
    struct timespec b,off;
    long ts_100us;

    //ts_100us = (gps_val[0] & 0xfffffffffff00000);// [19:0] 100 us 
    ts_100us = (gps_val[0] & 0x00000000000fffff);// [19:0] 100 us 
    b.tv_sec  = mktime(1970,1,1,0,0,0);
    b.tv_nsec = 0;
    off.tv_sec = gps_val[0] >> 20; // [39:20] sec
    off.tv_nsec = ts_100us * 100 * NSEC_PER_USEC; 
    *ts = timespec_add(b, off);
}

static int zynq_gps_ts_check(zynq_dev_t *zdev, struct timespec *ts_gps)
{
    u64             gps_val[3];
    struct timespec ts_sys;
    int diff;
    int delay;
    int thresh;

    if (zdev->zdev_gps_cnt & 0x1) {
        ktime_get_real_ts(&ts_sys);
        if (zynq_get_gps_time(zdev, gps_val)) {
            return -1;
        }
	} else {
		if (zynq_get_gps_time(zdev, gps_val)) {
			return -1;
		}
		ktime_get_real_ts(&ts_sys);
	}

	zynq_gps_time_ts(gps_val, ts_gps);

	/* diff in usec */
	diff = (ts_sys.tv_sec - ts_gps->tv_sec) * USEC_PER_SEC +
	    (ts_sys.tv_nsec - ts_gps->tv_nsec) / NSEC_PER_USEC;

	if (zdev->zdev_gps_ts_first.tv_sec) {
		zdev->zdev_sys_drift += diff;
	}

	zynq_trace(ZYNQ_TRACE_GPS,
	    "%d SYS time: %ld.%09ld, %s time: %ld.%09ld, %dus\n",
	    zdev->zdev_inst, ts_sys.tv_sec, ts_sys.tv_nsec,
	    (gps_val[0] & ZYNQ_GPS_TIME_VALID) ? "GPS" : "FPGA",
	    ts_gps->tv_sec, ts_gps->tv_nsec, diff);

	if (zdev->zdev_gps_smoothing) {
		return 0;
	}

	/*
	 * Check the validity of the GPS time.
	 *
	 * When the gap between the system time and gps time is more than
	 * the low water mark, the system time sync is still performed
	 * while a warning is issued.
	 *
	 * When the gap is more than the high water mark, we'll fail the
	 * system time sync.
	 */

	/* If it is the first sync, bypass the time check */
	if (!zdev->zdev_gps_ts.tv_sec && !zdev->zdev_gps_ts.tv_nsec) {
		return 0;
	}

	/* Firstly find the delay since the last successful sync in seconds */
	delay = ts_gps->tv_sec - zdev->zdev_gps_ts.tv_sec;
	if (delay < 0) {
		zynq_err("%d WARNING! SYS time: %ld.%09ld, %s time: %ld.%09ld, "
		    "Last sync time: %ld.%09ld, abnormal time jump.\n",
		    zdev->zdev_inst, ts_sys.tv_sec, ts_sys.tv_nsec,
		    (gps_val[0] & ZYNQ_GPS_TIME_VALID) ? "GPS" : "FPGA",
		    ts_gps->tv_sec, ts_gps->tv_nsec,
		    zdev->zdev_gps_ts.tv_sec, zdev->zdev_gps_ts.tv_nsec);
		return 0;
	}

	/* Check the high water mark in usec */
   thresh = zynq_gps_high_factor * delay;
   if ((diff > thresh) || (diff < -thresh)) {
		zynq_err("%d WARNING! SYS time: %ld.%09ld, %s time: %ld.%09ld, "
		    "gap %dus exceeds high threshold %dus.\n",
		    zdev->zdev_inst, ts_sys.tv_sec, ts_sys.tv_nsec,
		    (gps_val[0] & ZYNQ_GPS_TIME_VALID) ? "GPS" : "FPGA",
		    ts_gps->tv_sec, ts_gps->tv_nsec, diff, thresh);
		return 0;
	}

	/* Check the low water mark in usec */
	thresh = zynq_gps_low_factor * delay;
	if ((diff > thresh) || (diff < -thresh)) {
		zynq_err("%d WARNING! SYS time: %ld.%09ld, %s time: %ld.%09ld, "
		    "gap %dus exceeds low threshold %dus.\n",
		    zdev->zdev_inst, ts_sys.tv_sec, ts_sys.tv_nsec,
		    (gps_val[0] & ZYNQ_GPS_TIME_VALID) ? "GPS" : "FPGA",
		    ts_gps->tv_sec, ts_gps->tv_nsec, diff, thresh);
	}

	return 0;
}

static int zynq_do_gps_sync(zynq_dev_t *zdev)
{
    struct timespec ts_gps;
    int ret;

    /* get current GPS time */
    if ((ret = zynq_gps_ts_check(zdev, &ts_gps))) {
        return ret;
    }
    if (!zdev->zdev_gps_ts_first.tv_sec) {
        zdev->zdev_gps_ts_first = ts_gps;
    }
    zdev->zdev_gps_ts = ts_gps;
    zdev->zdev_gps_cnt++;

    return 0;
}

/*
 * GPS sync thread: sync system time to GPS time periodically per
 * zynq_gps_sync_time seconds. (Note, we can't use a hrtimer to
 * do this as there are side effects to do do_settimeofday() via
 * a hrtimer per my testing.)
 */
static int zynq_gps_sync_thread(void *arg)
{
    zynq_dev_t *zdev;
    unsigned int delay_time;
    long result;

    while (!kthread_should_stop()) 
    {
        zdev = zynq_gps_master_dev;

        /* sync system time to GPS time */
        (void) zynq_do_gps_sync(zdev);

		/*
		 * Wait for specified zynq_gps_sync_time seconds. We can't
		 * simply call msleep() here as this will cause driver
		 * unloading to wait.
		 */
        if (zdev->zdev_gps_smoothing) {
            delay_time = ZYNQ_GPS_SYNC_TIME_SMOOTH;
        } else {
            delay_time = zynq_gps_sync_time;
        }
        if (delay_time == 0) {
            delay_time = ZYNQ_GPS_SYNC_TIME_MAX;
        }
        result = wait_for_completion_interruptible_timeout(
            &zynq_gps_wait, msecs_to_jiffies(delay_time * 1000));
        if (ZYNQ_CAP(zdev, GPS_SMOOTH)) {
            u32 status;
            spin_lock(&zdev->zdev_lock);
            status = zynq_g_reg_read(zdev, ZYNQ_G_GPS_STATUS);
            zynq_trace(ZYNQ_TRACE_PROBE, "%d %s: gps_status=0x%x\n",
                       zdev->zdev_inst, __FUNCTION__, status);
            if (status & ZYNQ_GPS_SMOOTH_IN_PROGRESS) {
                zdev->zdev_gps_smoothing = 2;
            } else if (zdev->zdev_gps_smoothing) {
                zdev->zdev_gps_smoothing--;
            }
            spin_unlock(&zdev->zdev_lock);
        }
        if (result > 0) {
            /* Restart the completion wait */
            reinit_completion(&zynq_gps_wait);
        } else if (result != 0) {
            /* Interrupted or module is being unloaded */
            break;
        }
    }

    return 0;
}

/*
 * create the GPS sync thread and schedule it with the second highest priority
 * that is lower than the watchdog (99) and higher than the interrupts (50).
 */
static void zynq_gps_thread_init(zynq_dev_t *zdev)
{
    struct sched_param param = { .sched_priority = 60 };

    init_completion(&zynq_gps_wait);
    zynq_gps_taskp = kthread_run(zynq_gps_sync_thread, NULL,
                                 "zynq_gps_sync_thread");
    /* set the thread scheduling class as SCHED_FIFO  */
    sched_setscheduler(zynq_gps_taskp, SCHED_FIFO, &param);
}

/*
 * destroy the GPS sync thread
 */
static void zynq_gps_thread_fini(zynq_dev_t *zdev)
{
	complete(&zynq_gps_wait);
	if (zynq_gps_taskp) {
		kthread_stop(zynq_gps_taskp);
	}
}

static int zynq_gps_init_fpga_time(zynq_dev_t *zdev)
{
	struct timespec ts = { 0 };
	struct tm       t;
	u64             gps_val[3];
	u32             val;

	if (!ZYNQ_CAP(zdev, FPGA_TIME_INIT)) {
       zynq_err("%d %s: "
                "FPGA time init is not supported on this device\n",
                zdev->zdev_inst, __FUNCTION__);
       return -EPERM;
   }

   (void) zynq_get_gps_time(zdev, gps_val);

	spin_lock(&zdev->zdev_lock);
   if (zynq_gps_valid_once) {
       spin_unlock(&zdev->zdev_lock);
       /* GPS time valid once, do nothing */
       zynq_err("%d %s: FPGA time already initialized\n",
                zdev->zdev_inst, __FUNCTION__);
       return -EAGAIN;
	}
   
   /* Get the current system time */
   ktime_get_real_ts(&ts);
   zynq_log("%d %s: init FPGA time with SYS time: %ld.%09ld\n",
            zdev->zdev_inst, __FUNCTION__, ts.tv_sec, ts.tv_nsec);

   /* Get the broken-down time from the Epoch time */
   time_to_tm(ts.tv_sec, 0, &t);

   /* Update the FPGA initial time */
   val = zynq_g_reg_read(zdev, ZYNQ_G_GPS_CONFIG_2);
   val = SET_BITS(ZYNQ_GPS_TIME_DAY, val, t.tm_mday) |
       SET_BITS(ZYNQ_GPS_TIME_MON, val, t.tm_mon + 1) |
       SET_BITS(ZYNQ_GPS_TIME_YEAR, val, t.tm_year - 100);
   zynq_g_reg_write(zdev, ZYNQ_G_GPS_CONFIG_2, val);

   val = SET_BITS(ZYNQ_GPS_TIME_SEC, 0, t.tm_sec) |
       SET_BITS(ZYNQ_GPS_TIME_MIN, 0, t.tm_min) |
	    SET_BITS(ZYNQ_GPS_TIME_HOUR, 0, t.tm_hour);
   zynq_g_reg_write(zdev, ZYNQ_G_GPS_TIME_HI_INIT, val);

   val = SET_BITS(ZYNQ_GPS_TIME_NSEC, 0,
                  (ts.tv_nsec % NSEC_PER_USEC) >> 3) |
       SET_BITS(ZYNQ_GPS_TIME_USEC, 0,
                (ts.tv_nsec / NSEC_PER_USEC) % USEC_PER_MSEC) |
       SET_BITS(ZYNQ_GPS_TIME_MSEC, 0,
                ts.tv_nsec / NSEC_PER_MSEC);
   val |= ZYNQ_GPS_TIME_VALID;
   zynq_g_reg_write(zdev, ZYNQ_G_GPS_TIME_LO_INIT, val);
   spin_unlock(&zdev->zdev_lock);

   return 0;
}

static void zynq_gps_config(zynq_dev_t *zdev)
{
    u32 step;
    u32 val;

    if (!ZYNQ_CAP(zdev, GPS_SMOOTH)) {
        /* GPS time sync smoothing is not supported for MoonRover */
        return;
    }

    spin_lock(&zdev->zdev_lock);
    val = zynq_g_reg_read(zdev, ZYNQ_G_GPS_CONFIG);
    if (zynq_gps_smooth_max == 0) {
        val |= ZYNQ_GPS_DISABLE_SMOOTH;
    } else {
        if (zynq_gps_smooth_step < ZYNQ_GPS_SMOOTH_STEP_MIN) {
            step = 0xFFFF;
        } else if (zynq_gps_smooth_step > ZYNQ_GPS_SMOOTH_STEP_MAX) {
            step = 1;
        } else {
            step = USEC_PER_SEC / zynq_gps_smooth_step - 1;
        }
        val = SET_BITS(ZYNQ_GPS_ADJ_STEP, val, step);
        val = SET_BITS(ZYNQ_GPS_MAX_TOLERANCE, val, zynq_gps_smooth_max);
        val &= ~ZYNQ_GPS_DISABLE_SMOOTH;
    }
    val &= ~ZYNQ_GPS_LOOPBACK_EN;
    zynq_g_reg_write(zdev, ZYNQ_G_GPS_CONFIG, val);

    zynq_trace(ZYNQ_TRACE_PROBE, "%d %s: gps_config=0x%x\n",
               zdev->zdev_inst, __FUNCTION__, val);

    val = zynq_g_reg_read(zdev, ZYNQ_G_GPS_CONFIG_2);
    if (zynq_gps_checksum) {
        val |= ZYNQ_GPS_CHECKSUM_CHECK;
    } else {
        val &= ~ZYNQ_GPS_CHECKSUM_CHECK;
    }
    zynq_g_reg_write(zdev, ZYNQ_G_GPS_CONFIG_2, val);
    spin_unlock(&zdev->zdev_lock);
}

static u32 zynq_gps_status(zynq_dev_t *zdev)
{
    u32 status;

    status = zynq_g_reg_read(zdev, ZYNQ_G_STATUS);
    zynq_log("%d PPS is %s, GPS %s locked\n", zdev->zdev_inst,
             (status & ZYNQ_STATUS_PPS_LOCKED) ? "ON" : "OFF",
             (status & ZYNQ_STATUS_GPS_LOCKED) ? "is" : "is NOT");

    return status;
}

void zynq_gps_init(zynq_dev_t *zdev)
{
	if (zynq_fwupdate_param) {
		return;
	}

    spin_lock_init(&zynq_gps_lock);


	spin_lock(&zynq_gps_lock);
	if (zynq_gps_master_dev != NULL) {
		spin_unlock(&zynq_gps_lock);
		return;
	}

	zynq_trace(ZYNQ_TRACE_PROBE, "%d %s: GPS master device set\n",
	    zdev->zdev_inst, __FUNCTION__);
	zynq_gps_master_dev = zdev;
	spin_unlock(&zynq_gps_lock);

	zynq_gps_config(zdev);

	zynq_gps_status(zdev);

	zynq_gps_thread_init(zdev);
}

void zynq_gps_fini(zynq_dev_t *zdev)
{
	spin_lock(&zynq_gps_lock);
	zynq_gps_master_dev = NULL;
	spin_unlock(&zynq_gps_lock);

	zynq_gps_thread_fini(zdev);
}

void zynq_gps_pps_changed(zynq_dev_t *zdev)
{
    u32 status = zynq_gps_status(zdev);

    /* notify the use space waiting thread on ioctl:ZYNQ_IOC_REG_GPSPPS_EVENT_WAIT */
    complete(&zdev->zdev_gpspps_event_comp);

    if (!(status & ZYNQ_STATUS_GPS_LOCKED)) {
        ZYNQ_STATS_LOGX(zdev, DEV_STATS_GPS_UNLOCK, 1, 0);
    }

    if (ZYNQ_CAP(zdev, GPS_SMOOTH) && (status & ZYNQ_STATUS_GPS_LOCKED)) {
        spin_lock(&zynq_gps_lock);
        if (zdev != zynq_gps_master_dev) {
            spin_unlock(&zynq_gps_lock);
            return;
        }
        spin_unlock(&zynq_gps_lock);

        zynq_gps_config(zdev);
        
        spin_lock(&zdev->zdev_lock);
        status = zynq_g_reg_read(zdev, ZYNQ_G_GPS_STATUS);
        spin_unlock(&zdev->zdev_lock);
        zynq_trace(ZYNQ_TRACE_PROBE, "%d %s: gps_status=0x%x\n",
                   zdev->zdev_inst, __FUNCTION__, status);
        if (status & ZYNQ_GPS_SMOOTH_IN_PROGRESS) {
            complete(&zynq_gps_wait);
        }
    }
}

/*
 * GPS support ioctls
 */
static long zynq_gps_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    zynq_dev_t *zdev = (typeof(zdev))filp->private_data;
    int err = 0;

    switch (cmd) {
    case ZYNQ_IOC_GPS_GET:
        {
            u64 gps_val[3];

            if (zynq_get_gps_time(zdev, gps_val)) {
                err = -EAGAIN;
                break;
            }

            if (copy_to_user((void __user *)arg, gps_val,
                             ZYNQ_GPS_VAL_SZ)) {
                err = -EFAULT;
                break;
            }

            return 0;
        }

    case ZYNQ_IOC_GPS_GPRMC_GET:
        {
            u8 gprmc_buf[ZYNQ_GPS_GPRMC_VAL_SZ + 1];
            u32 *gprmc_val = (u32 *)gprmc_buf;

            if (zynq_get_gprmc(zdev, gprmc_val)) {
                err = -EAGAIN;
                break;
            }

            gprmc_buf[ZYNQ_GPS_GPRMC_VAL_SZ] = '\0';

            if (copy_to_user((void __user *)arg, gprmc_val,
                             ZYNQ_GPS_GPRMC_VAL_SZ)) {
                err = -EFAULT;
                break;
            }
            zynq_trace(ZYNQ_TRACE_PROBE, "%d %s: GPRMC data: %s\n",
                       zdev->zdev_inst, __FUNCTION__, (char *)gprmc_val);
            return 0;
        }

    case ZYNQ_IOC_GPS_SYNC:
        spin_lock(&zynq_gps_lock);
        if (zdev != zynq_gps_master_dev) {
            spin_unlock(&zynq_gps_lock);
            zynq_err("%d %s: it's not the GPS master device "
                     "for GPS/SYS time sync support\n",
                     zdev->zdev_inst, __FUNCTION__);
            err = -EPERM;
            break;
        }
        spin_unlock(&zynq_gps_lock);

        if (zynq_do_gps_sync(zdev)) {
            err = -EAGAIN;
        }
        break;
        
    case ZYNQ_IOC_GPS_FPGA_INIT:
        spin_lock(&zynq_gps_lock);
        if (zdev != zynq_gps_master_dev) {
            spin_unlock(&zynq_gps_lock);
            zynq_err("%d %s: it's not the GPS master device "
                     "for FPGA time init support\n",
                     zdev->zdev_inst, __FUNCTION__);
            err = -EPERM;
            break;
        }
        spin_unlock(&zynq_gps_lock);
        err = zynq_gps_init_fpga_time(zdev);
        break;

    default:
        err = -EINVAL;
        break;
    }
    
    zynq_trace(ZYNQ_TRACE_PROBE, "%d %s: error = %d\n",
               zdev->zdev_inst, __FUNCTION__, err);
    return err;
}

static int zynq_gps_open(struct inode *inode, struct file *filp)
{
	zynq_dev_t *zdev;

	zdev = container_of(inode->i_cdev, zynq_dev_t, zdev_cdev_gps);
	filp->private_data = zdev;

	zynq_trace(ZYNQ_TRACE_PROBE, "%d %s done.",
	    zdev->zdev_inst, __FUNCTION__);
	return 0;
}

static int zynq_gps_release(struct inode *inode, struct file *filp)
{
	return 0;
}


/* maps the PCIe BAR into user space for memory-like access using mmap() */
int zynq_mmap(struct file *filp, struct vm_area_struct *vma)
{
	zynq_dev_t *zdev = (typeof(zdev))filp->private_data;
    struct pci_dev *pdev;
    int bar = 0; // zdev_bar0
    u64 bar_start;
    u32 bar_len;
	unsigned long off;
	unsigned long phys;
	unsigned long vsize;
	unsigned long psize;
	int rv;

    if(zdev == NULL) {
        zynq_trace(ZYNQ_TRACE_PROBE, "zdev:0x%px is NULL \n", zdev);
        return -EINVAL;
    }

	off   = vma->vm_pgoff << PAGE_SHIFT;
	vsize = vma->vm_end - vma->vm_start;

	/* BAR physical address */
    pdev = zdev->zdev_pdev;
    bar_start = pci_resource_start(pdev, bar);
    bar_len   = pci_resource_len(pdev, bar);

	phys  = bar_start + off;
	psize = bar_len - off;

	zynq_trace(ZYNQ_TRACE_PROBE, "mmap(): zdev    = 0x%px\n", zdev);
	zynq_trace(ZYNQ_TRACE_PROBE, "mmap(): pci_dev = 0x%px\n", pdev);
	zynq_trace(ZYNQ_TRACE_PROBE,
               "bar%d start:0x%llx + off:0x%lx => phys:0x%lx \n",
               bar, bar_start, off, phys);


	if (vsize > psize) {
        zynq_trace(ZYNQ_TRACE_PROBE, "vsize:%lu > psize:%lu \n", vsize, psize);
        return -EINVAL;
    }

	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
	/*
	 * prevent touching the pages (byte access) for swap-in,
	 * and prevent the pages from being swapped out
	 */
	vma->vm_flags |= VM_IO | VM_DONTEXPAND | VM_DONTDUMP; //VMEM_FLAGS;
	/* make MMIO accessible to user space MUST 4K aligned */
	rv = io_remap_pfn_range(vma,
                            vma->vm_start,
                            phys >> PAGE_SHIFT,
                            vsize,
                            vma->vm_page_prot);

	zynq_trace(ZYNQ_TRACE_PROBE,
               "vma=0x%p, vma->vm_start=0x%lx, phys=0x%lx, size=%lu, noncached, returned:%d\n",
               vma,
               vma->vm_start,
               phys >> PAGE_SHIFT,
               vsize,
               rv);

	if (rv) {
        return -EAGAIN;
    }

	return 0;
}

ssize_t zynq_read(struct file *filp, char __user *buf, size_t count, loff_t *pos)
{
    u32         off  = (typeof(off))*pos;
    zynq_dev_t *zdev = (typeof(zdev))filp->private_data;
    u32 w;
    int rv;

    if(zdev == NULL) {
        zynq_trace(ZYNQ_TRACE_PROBE, "zdev:0x%px is NULL \n", zdev);
        return -EINVAL;
    }

    /* only 32-bit aligned and 32-bit multiples */
    if (off & 3) {
        return -EPROTO;
    }

    /* first address is BAR base plus file position offset */
    //void *reg;
    //reg = zdev->zdev_bar0 + *pos;
    //w = read_register(reg);
    //w = ioread32(reg);
    w = zynq_g_reg_read(zdev, off);
    zynq_trace(ZYNQ_TRACE_PROBE,
               "zynq_read(count=%ld, pos=%u) value = 0x%08x\n",
               (long)count,
               off,
               w);
    rv = copy_to_user(buf, &w, 4);
    if (rv) {
        zynq_trace(ZYNQ_TRACE_PROBE, "Copy to userspace failed but continuing\n");
    }


    *pos += 4;
    return 4;
}

static ssize_t zynq_write(struct file *filp, const char __user *buf,
                          size_t count, loff_t *pos)
{
    u32         off  = (typeof(off))*pos;
    zynq_dev_t *zdev = (typeof(zdev))filp->private_data;
    u32 w;
    int rv;

    if(zdev == NULL) {
        zynq_trace(ZYNQ_TRACE_PROBE, "zdev:0x%px is NULL \n", zdev);
        return -EINVAL;
    }

    /* only 32-bit aligned and 32-bit multiples */
    if (off & 3) {
        return -EPROTO;
    }

    /* first address is BAR base plus file position offset */
    //reg = xdev->bar[xcdev->bar] + *pos;
    rv = copy_from_user(&w, buf, 4);
    if (rv) {
        pr_info("copy from user failed %d/4, but continuing.\n", rv);
    }

    zynq_trace(ZYNQ_TRACE_PROBE,
               "zynq_write(0x%08x, count=%ld, pos=%u)\n",
               w, (long)count, off);

    //write_register(w, reg);
    //iowrite32(w, reg);
    zynq_g_reg_write(zdev, off, w);

    *pos += 4;
    return 4;
}

struct file_operations zynq_gps_fops = {
    .owner          = THIS_MODULE,
    .unlocked_ioctl = zynq_gps_ioctl,
    .open           = zynq_gps_open,
    .release        = zynq_gps_release,
    .mmap           = zynq_mmap,
    .read           = zynq_read,
    .write          = zynq_write
};

module_param_named(gpssynctime, zynq_gps_sync_time, uint, S_IRUGO|S_IWUSR);
MODULE_PARM_DESC(gpssynctime, "GPS sync interval in seconds");
module_param_named(gpssmoothstep, zynq_gps_smooth_step, uint, S_IRUGO|S_IWUSR);
MODULE_PARM_DESC(gpssmoothstep, "GPS sync smoothing step length in usec");
module_param_named(gpssmoothmax, zynq_gps_smooth_max, uint, S_IRUGO|S_IWUSR);
MODULE_PARM_DESC(gpssmoothmax, "GPS sync smoothing max tolerance in msec");
module_param_named(gpschecksum, zynq_gps_checksum, uint, S_IRUGO|S_IWUSR);
MODULE_PARM_DESC(gpschecksum, "GPRMC checksum validation");
module_param_named(gpshifactor, zynq_gps_high_factor, uint, S_IRUGO|S_IWUSR);
MODULE_PARM_DESC(gpshifactor, "high water mark factor for GPS time check");
module_param_named(gpslofactor, zynq_gps_low_factor, uint, S_IRUGO|S_IWUSR);
MODULE_PARM_DESC(gpslofactor, "low water mark factor for GPS time check");

