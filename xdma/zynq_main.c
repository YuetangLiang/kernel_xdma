#include <linux/init.h>
#include <linux/module.h>
#include <linux/version.h>
#include <linux/kobject.h>
#include "basa.h"

/* module parameters */
/* module tracing messages enabling */
unsigned int zynq_trace_param = ZYNQ_TRACE_PROBE|ZYNQ_TRACE_REG;
unsigned int zynq_bringup_param = 0;
/* Firmware update enabling to update PL fabric image or PS OS image */
unsigned int zynq_fwupdate_param = 0;
/* enabling debug register dump */
unsigned int zynq_dbg_reg_dump_param = 0;
/* log interval in second for statistics log throttling */
unsigned int zynq_stats_log_interval = 1;

/* driver global variables */
spinlock_t zynq_g_lock;
spinlock_t zynq_gps_lock;
static int zynq_instance = 0;
static unsigned int zynq_can_count = 0;
static struct zynq_dev *zynq_dev_list = NULL;
static dev_t zynq_g_dev = 0;
static int zynq_major = 0;
static int zynq_minor = 0;
static struct class *zynq_class = NULL;
static struct kobject *zynq_kobject;

static const char zdev_stats_label[DEV_STATS_NUM][ZYNQ_STATS_LABEL_LEN] = {
	"Interrupt",
	"Invalid interrupt",
	"GPS not locked"
};

/* per channel interrupt clearing routine */
void zdev_clear_intr_ch(zynq_dev_t *zdev, int ch)
{
    if (ch >= zdev->zdev_chan_cnt) {
        return;
    }
    zynq_g_reg_write(zdev, ZYNQ_G_INTR_STATUS_TX, ZYNQ_INTR_CH_TX_ALL(ch));
    zynq_g_reg_write(zdev, ZYNQ_G_INTR_STATUS_RX, ZYNQ_INTR_CH_RX_ALL(ch));
}

/* Clear all error interrupt */
static void zdev_clear_err_intr_all(zynq_dev_t *zdev)
{
    zynq_chan_t *zchan;
    int i;

    zchan = zdev->zdev_chans;
    for (i = 0; i < zdev->zdev_chan_cnt; i++, zchan++) {
        zchan_reg_write(zchan, ZYNQ_CH_ERR_STATUS,
                        zchan_reg_read(zchan, ZYNQ_CH_ERR_STATUS));
    }
}

/* interrupt clearing routine */
static inline void zdev_clear_intr_all(zynq_dev_t *zdev)
{
    zynq_g_reg_write(zdev, ZYNQ_G_INTR_STATUS_TX,
                     zynq_g_reg_read(zdev, ZYNQ_G_INTR_STATUS_TX));
    zynq_g_reg_write(zdev, ZYNQ_G_INTR_STATUS_RX,
                     zynq_g_reg_read(zdev, ZYNQ_G_INTR_STATUS_RX));
    zynq_g_reg_write(zdev, ZYNQ_G_PS_INTR_STATUS,
                     zynq_g_reg_read(zdev, ZYNQ_G_PS_INTR_STATUS));
}

/* interrupt disable/enabling routines */
static inline void zdev_disable_intr_all(zynq_dev_t *zdev)
{
	zynq_g_reg_write(zdev, ZYNQ_G_INTR_MASK_TX, ZYNQ_INTR_TX_ALL);
	zynq_g_reg_write(zdev, ZYNQ_G_INTR_MASK_RX, ZYNQ_INTR_RX_ALL);
	zynq_g_reg_write(zdev, ZYNQ_G_PS_INTR_MASK, ZYNQ_PS_INTR_ALL);
}

static inline void zdev_enable_intr_all(zynq_dev_t *zdev)
{
	int ch, intr_tx_all = 0, intr_rx_all = 0, intr_ps_all = 0;

	if (zynq_fwupdate_param) {
		intr_ps_all = ~ZYNQ_PS_INTR_FW;
	} else {
		for (ch = 0; ch < zdev->zdev_chan_cnt; ch++) {
			intr_tx_all |= ZYNQ_INTR_CH_TX_ALL(ch);
			intr_rx_all |= ZYNQ_INTR_CH_RX_ALL(ch);
		}
		intr_ps_all = ~ZYNQ_PS_INTR_GPS_PPS;
	}

	zynq_g_reg_write(zdev, ZYNQ_G_INTR_UNMASK_TX, intr_tx_all);
	zynq_g_reg_write(zdev, ZYNQ_G_INTR_UNMASK_RX, intr_rx_all);
	/* no unmask register for PS interrupt */
	zynq_g_reg_write(zdev, ZYNQ_G_PS_INTR_MASK, intr_ps_all);
}

static inline void zdev_disable_intr_ch(zynq_dev_t *zdev, u32 ch)
{
	zynq_g_reg_write(zdev, ZYNQ_G_INTR_MASK_TX, ZYNQ_INTR_CH_TX_ALL(ch));
	zynq_g_reg_write(zdev, ZYNQ_G_INTR_MASK_RX, ZYNQ_INTR_CH_RX_ALL(ch));
}

static inline void zdev_enable_intr_ch(zynq_dev_t *zdev, u32 ch)
{
	zynq_g_reg_write(zdev, ZYNQ_G_INTR_UNMASK_TX, ZYNQ_INTR_CH_TX_ALL(ch));
	zynq_g_reg_write(zdev, ZYNQ_G_INTR_UNMASK_RX, ZYNQ_INTR_CH_RX_ALL(ch));
}

static inline void zdev_disable_intr_ch_rx_all(zynq_dev_t *zdev, u32 ch)
{
	zynq_g_reg_write(zdev, ZYNQ_G_INTR_MASK_RX, ZYNQ_INTR_CH_RX_ALL(ch));
}

static inline void zdev_enable_intr_ch_rx_all(zynq_dev_t *zdev, u32 ch)
{
	zynq_g_reg_write(zdev, ZYNQ_G_INTR_UNMASK_RX, ZYNQ_INTR_CH_RX_ALL(ch));
}

static inline void zdev_disable_intr_ch_tx_all(zynq_dev_t *zdev, u32 ch)
{
	zynq_g_reg_write(zdev, ZYNQ_G_INTR_MASK_TX, ZYNQ_INTR_CH_TX_ALL(ch));
}

static inline void zdev_enable_intr_ch_tx_all(zynq_dev_t *zdev, u32 ch)
{
	zynq_g_reg_write(zdev, ZYNQ_G_INTR_UNMASK_TX, ZYNQ_INTR_CH_TX_ALL(ch));
}

int zynq_stats_log(zynq_stats_t *stats, int count, int interval)
{
	unsigned long log_interval;

	if (stats == NULL) {
		return 0;
	}

	stats->cnt += count;

	if ((zynq_trace_param & ZYNQ_TRACE_STATS) == 0) {
		log_interval = (interval >= 0) ? interval :
		    (zynq_stats_log_interval * HZ);

		if ((unsigned long)(jiffies - stats->ts) < log_interval) {
			return 0;
		}
	}

	stats->ts = jiffies;
	return 1;
}

void zynq_chan_err_proc(zynq_chan_t *zchan)
{
}

/* Rx process function on Rx interrupt */
void zynq_chan_rx_proc(zynq_chan_t *zchan)
{
}


void zynq_chan_proc(zynq_chan_t *z, int status)
{
}

void zynq_chan_tasklet(unsigned long arg)
{
    zynq_chan_t *z = (zynq_chan_t *)arg;
    zynq_dev_t  *zdev  = z->zdev;
    u32 intr_status_gps = 0, status = 0;
    u32 intr_status_tx = zynq_g_reg_read(zdev, ZYNQ_G_INTR_STATUS_TX);
    u32 intr_status_rx = zynq_g_reg_read(zdev, ZYNQ_G_INTR_STATUS_RX);

    u32 chan_id = z->zchan_num;
    if (chan_id == 0) {
        intr_status_gps = zynq_g_reg_read(zdev, ZYNQ_G_PS_INTR_STATUS);
        if (intr_status_gps & ZYNQ_PS_INTR_GPS_PPS_CHG) {
            zynq_gps_pps_changed(zdev);
        }
    }
	zynq_trace(ZYNQ_TRACE_INTR,
	    "%d ch %d %s: rd_intr=0x%x, wr_intr=0x%x, ps_intr=0x%x\n",
	    ZYNQ_INST(z), z->zchan_num, __FUNCTION__,
	    intr_status_tx, intr_status_rx, intr_status_gps);

	if (intr_status_rx & ZYNQ_INTR_CH_RX(chan_id)) {
		ZYNQ_STATS(z, CHAN_STATS_RX_INTR);
		status |= ZYNQ_CHAN_PROC_RX;
	}
	if (intr_status_tx & ZYNQ_INTR_CH_TX(chan_id)) {
		ZYNQ_STATS(z, CHAN_STATS_TX_INTR);
		status |= ZYNQ_CHAN_PROC_TX;
	}
	if (intr_status_rx & ZYNQ_INTR_CH_RX_ERR(chan_id)) {
		ZYNQ_STATS(z, CHAN_STATS_RX_ERR_INTR);
		status |= ZYNQ_CHAN_PROC_RX_ERR;
	}
	if (intr_status_tx & ZYNQ_INTR_CH_TX_ERR(chan_id)) {
		ZYNQ_STATS(z, CHAN_STATS_TX_ERR_INTR);
		status |= ZYNQ_CHAN_PROC_TX_ERR;
	}

	if (status) {
		zynq_chan_proc(z, status);
	}

	if (intr_status_tx) {
		zynq_g_reg_write(zdev, ZYNQ_G_INTR_STATUS_TX,
		    ZYNQ_INTR_CH_TX_ALL(chan_id));
	}
	if (intr_status_rx) {
		zynq_g_reg_write(zdev, ZYNQ_G_INTR_STATUS_RX,
		    ZYNQ_INTR_CH_RX_ALL(chan_id));
	}
	if (intr_status_gps) {
		zynq_g_reg_write(zdev, ZYNQ_G_PS_INTR_STATUS, intr_status_gps);
	}

	zynq_trace(ZYNQ_TRACE_INTR,
	    "%d ch %d %s: rd_intr=0x%x, wr_intr=0x%x, ps_intr=0x%x\n",
	    ZYNQ_INST(z), z->zchan_num, __FUNCTION__,
	    zynq_g_reg_read(zdev, ZYNQ_G_INTR_STATUS_TX),
	    zynq_g_reg_read(zdev, ZYNQ_G_INTR_STATUS_RX),
	    zynq_g_reg_read(zdev, ZYNQ_G_PS_INTR_STATUS));
}

void zynq_tasklet(unsigned long arg)
{
    zynq_dev_t *zdev = (zynq_dev_t *)arg;
    u32 rd_intr, wr_intr, ps_intr;
    u32 ch, status;
    zynq_chan_t *zchan = zdev->zdev_chans;

    /* DMA read interrupts for Tx/Tx error notification */
    rd_intr = zynq_g_reg_read(zdev, ZYNQ_G_INTR_STATUS_TX);
    /* DMA write interrupts for Rx/Rx error notification */
    wr_intr = zynq_g_reg_read(zdev, ZYNQ_G_INTR_STATUS_RX);
    /*
	 * interrupt from ARM core with PS image: currently H/W use it
	 * for GPS/PPS state change notification and firmware image
	 * update notification.
	 */
    ps_intr = zynq_g_reg_read(zdev, ZYNQ_G_PS_INTR_STATUS);

    zynq_trace(ZYNQ_TRACE_INTR,
               "%d %s: rd_intr=0x%x, wr_intr=0x%x, ps_intr=0x%x\n",
               zdev->zdev_inst, __FUNCTION__, rd_intr, wr_intr, ps_intr);
    
    if (!rd_intr && !wr_intr && !ps_intr) {
        ZYNQ_STATS(zdev, DEV_STATS_INTR_INVALID);
        return;
    }

    if (ps_intr & ZYNQ_PS_INTR_GPS_PPS_CHG) {
        zynq_gps_pps_changed(zdev);
    }

    for (ch = 0; ch < zdev->zdev_chan_cnt; ch++, zchan++) {
        status = 0;
        if (wr_intr & ZYNQ_INTR_CH_RX(ch)) {
            ZYNQ_STATS(zchan, CHAN_STATS_RX_INTR);
            status |= ZYNQ_CHAN_PROC_RX;
        }
        if (rd_intr & ZYNQ_INTR_CH_TX(ch)) {
            ZYNQ_STATS(zchan, CHAN_STATS_TX_INTR);
            status |= ZYNQ_CHAN_PROC_TX;
        }
        if (wr_intr & ZYNQ_INTR_CH_RX_ERR(ch)) {
            ZYNQ_STATS(zchan, CHAN_STATS_RX_ERR_INTR);
            status |= ZYNQ_CHAN_PROC_RX_ERR;
        }
        if (rd_intr & ZYNQ_INTR_CH_TX_ERR(ch)) {
            ZYNQ_STATS(zchan, CHAN_STATS_TX_ERR_INTR);
            status |= ZYNQ_CHAN_PROC_TX_ERR;
        }
        
        if (status) {
            zynq_chan_proc(zchan, status);
        }
    }
    
    if (rd_intr) {
        zynq_g_reg_write(zdev, ZYNQ_G_INTR_STATUS_TX, rd_intr);
    }
    if (wr_intr) {
        zynq_g_reg_write(zdev, ZYNQ_G_INTR_STATUS_RX, wr_intr);
    }
    if (ps_intr) {
       zynq_g_reg_write(zdev, ZYNQ_G_PS_INTR_STATUS, ps_intr);
    }
}

static void zdev_stats_init(zynq_dev_t *zdev)
{
	int i;

	for (i = 0; i < DEV_STATS_NUM; i++) {
		zdev->stats[i].label = zdev_stats_label[i];
	}
}

zynq_dev_t *zynq_zdev_init(unsigned short zdev_did)
{
    zynq_dev_t *zdev;
    int can_cnt, video_cnt, chan_cnt;
    unsigned long video_map, can_map;

    switch (zdev_did) {
    case PCI_DEVICE_ID_MOONROVER:
        chan_cnt  = ZYNQ_DMA_CHAN_NUM_MR;
        video_map = ZYNQ_VIDEO_MAP_MR;
        can_map   = ZYNQ_CAN_MAP_MR;
        break;
    default:
        zynq_err("%s: Unknown Device ID 0x%x\n",
            __FUNCTION__, zdev_did);
        return NULL;
    }

    if (zynq_fwupdate_param) {
        switch (zdev_did) {
        case PCI_DEVICE_ID_MOONROVER:
            chan_cnt = 0;
            can_map = 0;
            video_map = 0;
            break;
        default:
            break;
        }
    }

    video_cnt = hweight_long(video_map);
    can_cnt = hweight_long(can_map);
    zynq_trace(ZYNQ_TRACE_PROBE,
        "%s: chan_cnt = %d, video_cnt = %d, can_cnt = %d\n",
        __FUNCTION__, chan_cnt, video_cnt, can_cnt);
    ASSERT(chan_cnt >= (can_cnt + video_cnt));

    /* allocate device and channel structure */
    zdev = kzalloc(sizeof(zynq_dev_t) + 
                   chan_cnt  * sizeof(zynq_chan_t) +
                   can_cnt   * sizeof(zynq_can_t)  + 
                   video_cnt * sizeof(zynq_video_t),
                   GFP_KERNEL);
    if (zdev == NULL) {
        return NULL;
    }

    zdev->zdev_chan_cnt  = chan_cnt;
    zdev->zdev_can_cnt   = can_cnt;
    zdev->zdev_video_cnt = video_cnt;
    zdev->zdev_can_map   = can_map;
    zdev->zdev_video_map = video_map;

    zdev->zdev_chans = (zynq_chan_t *)((char *)zdev + sizeof(zynq_dev_t));
    zdev->zdev_cans = (zynq_can_t *)((char *)zdev->zdev_chans +
        chan_cnt * sizeof(zynq_chan_t));
    zdev->zdev_videos = (zynq_video_t *)((char *)zdev->zdev_cans +
        can_cnt * sizeof(zynq_can_t));

    return (zdev);
}

/*
 * Allocate the driver structure and initialize the device specific
 * parameters and capabilities.
 */
static zynq_dev_t *zdev_alloc(unsigned short zdev_did)
{
	zynq_dev_t   *zdev;
	zynq_chan_t  *zchan;
	zynq_video_t *zvideo;
	zynq_can_t   *zcan;
	unsigned long chan_map;
	int i, v, c;

	zdev = zynq_zdev_init(zdev_did);
	if (zdev == NULL) {
		return NULL;
	}

	zchan  = zdev->zdev_chans;
	zcan   = zdev->zdev_cans;
	zvideo = zdev->zdev_videos;
	c = 0;
	v = 0;
	for (i = 0; i < zdev->zdev_chan_cnt; i++, zchan++) {
		zchan->zdev = zdev;
		zchan->zchan_num = i;
		zchan->zchan_tx_ring.zchan = zchan;
		zchan->zchan_rx_tbl.zchan = zchan;

		chan_map = 1 << i;
		if (zdev->zdev_can_map & chan_map) {
			zchan->zchan_type = ZYNQ_CHAN_CAN;
			zchan->zchan_dev = zcan;

			zcan->zdev = zdev;
			zcan->zchan = zchan;
			zcan->zcan_ip_num = c;

			zynq_trace(ZYNQ_TRACE_PROBE,
			    "%d %s ch%d can%d\n",
			    zdev->zdev_inst, __FUNCTION__,
			    zchan->zchan_num, zcan->zcan_ip_num);
			c++;
			zcan++;
		} else if (zdev->zdev_video_map & chan_map) {
			zchan->zchan_type = ZYNQ_CHAN_VIDEO;
			zchan->zchan_dev = zvideo;

			zvideo->zdev = zdev;
			zvideo->zchan = zchan;
			zvideo->index = v;

			zynq_trace(ZYNQ_TRACE_PROBE,
			    "%d %s ch%d video%d\n",
			    zdev->zdev_inst, __FUNCTION__,
			    zchan->zchan_num, zvideo->index);
			v++;
			zvideo++;
		} else {
			continue;
		}
	}
	ASSERT(c == zdev->zdev_can_cnt);
	ASSERT(v == zdev->zdev_video_cnt);

	return zdev;
}

/*
 * create /dev/<devname>
 */
dev_t zynq_create_cdev(void *drvdata, struct cdev *cdev,
    struct file_operations *fops, char *devname)
{
	int err;
	dev_t dev;
	struct device *devicep;

	dev = MKDEV(zynq_major, zynq_minor++);
	devicep = device_create(zynq_class, NULL, dev, drvdata, devname);
	if (devicep == NULL) {
		zynq_err("%s failed to create device /dev/%s\n",
		    __FUNCTION__, devname);
		return 0;
	}

	cdev_init(cdev, fops);
	err = cdev_add(cdev, dev, 1);
	if (err) {
		zynq_err("%s failed to add cdev for /dev/%s\n",
		    __FUNCTION__, devname);
		device_destroy(zynq_class, dev);
		return 0;
	}

	zynq_trace(ZYNQ_TRACE_PROBE, "%s created device /dev/%s\n",
	    __FUNCTION__, devname);
	return dev;
}

void zynq_destroy_cdev(dev_t dev, struct cdev *cdev)
{
	cdev_del(cdev);
	device_destroy(zynq_class, dev);
}

static zynq_dev_t  *zdev;

/*
 * Device probe and driver initialization
 */
int zynq_probe(struct pci_dev *pdev, const struct pci_device_id *ent)
{
    zynq_chan_t *zchan;
    u64 bar_start;
    u32 bar_len;
    int i, err;

    zdev = zdev_alloc(pdev->device);
    if (zdev == NULL) {
        zynq_err("%s failed to alloc zdev.\n", __FUNCTION__);
        return -ENOMEM;
    }

    spin_lock_init(&zdev->zdev_lock);
    spin_lock_init(&zdev->zdev_i2c_lock);

    spin_lock(&zynq_g_lock);
    zdev->zdev_inst = zynq_instance;
    zdev->zdev_can_num_start = zynq_can_count;
    zynq_can_count += zdev->zdev_can_cnt;
    zynq_instance++;
    spin_unlock(&zynq_g_lock);

    zdev->zdev_pdev = pdev;
    pci_read_config_word(pdev, PCI_VENDOR_ID, &zdev->zdev_vid);
    pci_read_config_word(pdev, PCI_DEVICE_ID, &zdev->zdev_did);


    /* BAR0: Global and per channal configuration */
    bar_start = pci_resource_start(pdev, 0);
    bar_len   = pci_resource_len(pdev, 0);
    zdev->zdev_bar0     = ioremap(bar_start, bar_len);
    zdev->zdev_bar0_len = bar_len;
    if (!zdev->zdev_bar0) {
        zynq_err("%s ioremap failed for BAR0\n", __FUNCTION__);
        err = -EFAULT;
        goto err_ioremap_bar0;
    }
    zynq_trace(ZYNQ_TRACE_PROBE, "%s map bar0: bar_start=0x%llx, "
               "bar_len=%d, bar_va=0x%p\n", __FUNCTION__,
               bar_start, bar_len, zdev->zdev_bar0);

    zdev->zdev_version = zynq_g_reg_read(zdev, ZYNQ_G_VERSION);
    if (zdev->zdev_version == (unsigned int)-1) {
        zynq_err("%s Bar0 register access error, invalid version=-1, "
                 "please try rebooting ...\n", __FUNCTION__);
		//err = -EFAULT;
        //goto err_ioremap_bar2;
    }
    zynq_trace(ZYNQ_TRACE_PROBE, "zdev_version:0x%x\n", zdev->zdev_version);


    /* the string lengh should be less than ZYNQ_DEV_NAME_LEN */
    snprintf(zdev->zdev_name, sizeof(zdev->zdev_name),
             "%s%d-%s-%04x-%02x-%02x-%x", ZYNQ_DRV_NAME, zdev->zdev_inst,
             zdev->zdev_code_name, pci_domain_nr(pdev->bus),
             pdev->bus->number, pdev->devfn >> 3, pdev->devfn & 0x7);
    zynq_trace(ZYNQ_TRACE_PROBE, "FPGA device <%x,%x> %s\n",
               pdev->vendor, pdev->device, zdev->zdev_name);

    snprintf(zdev->prefix, ZYNQ_LOG_PREFIX_LEN, "%d", zdev->zdev_inst);
    zdev_stats_init(zdev);

    /* create all the cdevs */
    if (zynq_create_cdev_all(zdev) != 0) {
        goto err_create_cdev;
    }

    init_completion(&zdev->zdev_gpspps_event_comp);
    pci_set_master(pdev);
#if LINUX_VERSION_CODE < KERNEL_VERSION(3,18,0)
    if (pci_set_dma_mask(pdev, DMA_64BIT_MASK)) {
#else
    if (pci_set_dma_mask(pdev, DMA_BIT_MASK(64))) {
#endif
        zynq_err("%s failed to set DMA mask 64-bit\n", __FUNCTION__);

#if LINUX_VERSION_CODE < KERNEL_VERSION(3,18,0)
        if (pci_set_dma_mask(pdev, DMA_32BIT_MASK)) {
#else
        if (pci_set_dma_mask(pdev, DMA_BIT_MASK(32))) {
#endif
            zynq_err("%s failed to set DMA mask 32-bit\n",
                     __FUNCTION__);
            err = -EPERM;
            goto err_dma_mask;
        }
    }

#if LINUX_VERSION_CODE < KERNEL_VERSION(3,18,0)
    if (pci_set_consistent_dma_mask(pdev, DMA_64BIT_MASK)) {
#else
    if (pci_set_consistent_dma_mask(pdev, DMA_BIT_MASK(64))) {
#endif
        zynq_err("%s failed to set DMA coherent mask 64-bit\n",
                 __FUNCTION__);
        err = -EPERM;
        goto err_dma_mask;
    }
    //pci_set_drvdata(pdev, zdev);
    zdev_enable_intr_all(zdev);

    spin_lock(&zynq_g_lock);
    zdev->zdev_next = zynq_dev_list;
    zynq_dev_list = zdev;
    spin_unlock(&zynq_g_lock);

    zynq_gps_init(zdev);

    zynq_log("%s found, device %s, firmware version=%08x.\n",
             zdev->zdev_code_name, zdev->zdev_name, zdev->zdev_version);

    return 0;

    err_dma_mask:
    //pci_clear_master(pdev);
    zynq_destroy_cdev_all(zdev);

    err_create_cdev:
    //zynq_sysfs_fini(zdev);

    err_sysfs_init:
    //zynq_free_irq(zdev);

    err_chan_init:
    while (i--) {
        zchan--;
        //zchan_fini(zchan);
    }
    //iounmap(zdev->zdev_bar2);
    //zdev->zdev_bar2 = NULL;

  //err_ioremap_bar2:
    iounmap(zdev->zdev_bar0);
    zdev->zdev_bar0 = NULL;

    err_ioremap_bar0:
    //pci_disable_device(pdev);

    err_enable_pci:
    kfree(zdev);

    return err;
}

void zynq_remove(struct pci_dev *pdev)
{
	zynq_chan_t *zchan;
	int i;

	//zdev = pci_get_drvdata(pdev);
	if (zdev == NULL) {
		return;
	}

	zynq_gps_fini(zdev);
	zdev_disable_intr_all(zdev);
	zynq_destroy_cdev_all(zdev);


	iounmap(zdev->zdev_bar0);
	//pci_disable_device(pdev);
	//pci_set_drvdata(pdev, NULL);
	kfree(zdev);
	zdev = NULL;

	zynq_trace(ZYNQ_TRACE_PROBE, "%s done.\n", __FUNCTION__);
}

static int zynq_suspend(struct pci_dev *pdev, pm_message_t state)
{
	zynq_trace(ZYNQ_TRACE_PROBE, "%s done.\n", __FUNCTION__);
	return 0;
}

static int zynq_resume(struct pci_dev *pdev)
{
	zynq_trace(ZYNQ_TRACE_PROBE, "%s done.\n", __FUNCTION__);
	return 0;
}

static void zynq_shutdown(struct pci_dev *pdev)
{
	zynq_trace(ZYNQ_TRACE_PROBE, "%s done.\n", __FUNCTION__);
}

static struct pci_driver zynq_pci_driver = {
	.name = ZYNQ_DRV_NAME,
	.id_table = zynq_pci_dev_tbl,
	.probe = zynq_probe,
	.remove = zynq_remove,
	.suspend = zynq_suspend,
	.resume = zynq_resume,
	.shutdown = zynq_shutdown
};

/*
 * sysfs support for zdev_all
 */
static ssize_t zynq_zdev_all_show(struct kobject *kobj,
    struct kobj_attribute *attr, char *buf)
{
	//zynq_dev_t *zdev;
	int len = 0;

	spin_lock(&zynq_g_lock);
	//zdev = zynq_dev_list;
	if (zdev == NULL) {
		spin_unlock(&zynq_g_lock);
		return sprintf(buf + len, "\nNo FPGA device is found!\n");
	}

	if (zdev) {
		zdev->zdev_version = zynq_g_reg_read(zdev, ZYNQ_G_VERSION);
		len += sprintf(buf + len, "\n%s: %s card, F/W version=0x%08x, "
		    "driver version=%s\n", zdev->zdev_name, zdev->zdev_code_name,
		    (zdev->zdev_version & 0x7FFFFFFF), ZYNQ_MOD_VER);
		len += sprintf(buf + len, "    vendorID=0x%x, deviceID=0x%x, "
		    "instance=%d\n", zdev->zdev_vid, zdev->zdev_did,
		    zdev->zdev_inst);
		len += sprintf(buf + len, "    REG device: "
		    ZYNQ_DEV_NAME_REG"%d\n", zdev->zdev_inst);
		if (zdev->zdev_hw_cap & ZYNQ_HW_CAP_I2C) {
			len += sprintf(buf + len, "    I2C device: "
			    ZYNQ_DEV_NAME_I2C"%d\n", zdev->zdev_inst);
		}
		if (zdev->zdev_hw_cap & ZYNQ_HW_CAP_CAN) {
			len += sprintf(buf + len, "    CAN devices: "
			    ZYNQ_DEV_NAME_CAN"[%u - %u]\n",
			    zdev->zdev_can_num_start,
			    zdev->zdev_can_num_start + zdev->zdev_can_cnt - 1);
		}
		if (zdev->zdev_hw_cap & ZYNQ_HW_CAP_GPS) {
			len += sprintf(buf + len, "    GPS device: "
			    ZYNQ_DEV_NAME_GPS"%d\n", zdev->zdev_inst);
		}
		if (zdev->zdev_hw_cap & ZYNQ_HW_CAP_FW) {
			len += sprintf(buf + len, "    FW device: "
			    ZYNQ_DEV_NAME_FW"%d\n", zdev->zdev_inst);
		}
		if (zdev->zdev_hw_cap & ZYNQ_HW_CAP_TRIGGER) {
			len += sprintf(buf + len, "    Trigger device: "
			    ZYNQ_DEV_NAME_TRIGGER"%d\n", zdev->zdev_inst);
		}
		if (zdev->zdev_hw_cap & ZYNQ_HW_CAP_VIDEO) {
			zynq_video_t *zvideo;
			int i;

			zvideo = zdev->zdev_videos;
			for (i = 0; i < zdev->zdev_video_cnt; i++, zvideo++) {
				if (!zvideo->caps.link_up) {
					continue;
				}
				len += sprintf(buf + len, "    Video device: "
				    "video%d (vid%d)", zvideo->vdev.num, i);
				if (zdev->zdev_video_port_map) {
					len += sprintf(buf + len, " (port%d)\n",
					    zdev->zdev_video_port_map[i]);
				} else {
					len += sprintf(buf + len, "\n");
				}
			}
		}

		//zdev = zdev->zdev_next;
	}
	spin_unlock(&zynq_g_lock);

	return len;
}

static char *zynq_devnode(struct device *dev, umode_t *mode)
{
	if (mode) {
		*mode = 0666;
	}

	return NULL;
}

static struct kobj_attribute zdev_all_attribute =
    __ATTR(zdev_all, 0440, zynq_zdev_all_show, NULL);

/* driver module load entry point */
int zynq_cdev_init(void)
{
	dev_t dev;
	struct module *zmod;
	int error;

	spin_lock_init(&zynq_g_lock);
	spin_lock_init(&zynq_gps_lock);

	/* allocate a major number */
    error = alloc_chrdev_region(&dev,
                                zynq_minor,
                                ZYNQ_MINOR_COUNT,
                                ZYNQ_DRV_NAME);
    if (error) {
        zynq_err("%s: failed %d to alloc major, count=%d\n",
                 __FUNCTION__, error, ZYNQ_MINOR_COUNT);
        return error;
    }

	zynq_major = MAJOR(dev);
	zynq_minor = MINOR(dev);
	zynq_g_dev = dev;

	zynq_class = class_create(THIS_MODULE, ZYNQ_DRV_NAME); // from module_init()
	if (zynq_class == NULL) {
		unregister_chrdev_region(dev, ZYNQ_MINOR_COUNT);
		zynq_err("%s: failed to create class\n", __FUNCTION__);
		return -1;
	}
	zynq_class->devnode = zynq_devnode;

    return 0;
}

void zynq_cdev_cleanup(void)
{
	unregister_chrdev_region(zynq_g_dev, ZYNQ_MINOR_COUNT);
	class_destroy(zynq_class);

	zynq_trace(ZYNQ_TRACE_PROBE, "%s done.\n", __FUNCTION__);
}

/* module parameters */
module_param_named(trace, zynq_trace_param, uint, S_IRUGO|S_IWUSR);
MODULE_PARM_DESC(trace, "Trace level bitmask");
module_param_named(bringup, zynq_bringup_param, uint, S_IRUGO|S_IWUSR);
MODULE_PARM_DESC(bringup, "bringup mode");
module_param_named(fwupdate, zynq_fwupdate_param, uint, S_IRUGO|S_IWUSR);
MODULE_PARM_DESC(fwupdate, "card firmware update enabling");
module_param_named(dbgregdump, zynq_dbg_reg_dump_param, uint, S_IRUGO|S_IWUSR);
MODULE_PARM_DESC(dbgregdump, "enable debug register dump");
module_param_named(statslog, zynq_stats_log_interval, uint, S_IRUGO|S_IWUSR);
MODULE_PARM_DESC(statslog, "statistics log interval");


static const int video_port_map_mr[] = { 1, 2, 3, 4, 5 };
static unsigned int zynq_mr_cnt = 0;



int zynq_create_cdev_all(zynq_dev_t *zdev)
{
    char cdev_name[32];
    //unsigned int inst;

    /* create /dev/zynq_gps%d */
    sprintf(cdev_name, ZYNQ_DEV_NAME_GPS"%d", zdev->zdev_inst);
    zdev->zdev_dev_gps = zynq_create_cdev(zdev,
                                          &zdev->zdev_cdev_gps,
                                          &zynq_gps_fops,
                                          cdev_name);
    if (zdev->zdev_dev_gps == 0) {
        goto gps_fail;
    }

    return 0;

    zynq_destroy_cdev(zdev->zdev_dev_gps, &zdev->zdev_cdev_gps);
  gps_fail:
    return -1;
}

void zynq_destroy_cdev_all(zynq_dev_t *zdev)
{
    if (zdev->zdev_dev_gps) {
        zynq_destroy_cdev(zdev->zdev_dev_gps, &zdev->zdev_cdev_gps);
    }
}

/* Supported Sensor FPGA */
const struct pci_device_id zynq_pci_dev_tbl[] = {
    /* Vendor, Device, SubSysVendor, SubSysDev, Class, ClassMask, DrvDat */
    /* MoonRover, Sensor Box */
    { PCI_VENDOR_ID_BAIDU, PCI_DEVICE_ID_MOONROVER,
        PCI_ANY_ID, PCI_ANY_ID, 0, 0, 0 },
    { }    /* terminate list */
};

MODULE_DEVICE_TABLE(pci, zynq_pci_dev_tbl);
