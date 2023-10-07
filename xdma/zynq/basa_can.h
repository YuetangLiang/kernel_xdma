
#ifndef _BASA_CAN_H_
#define	_BASA_CAN_H_

/* create a kernel thread to handle Rx */
#undef	PIO_RX_THREAD
/* enable Rx interrupt to handle Rx */
#define	PIO_RX_INTR

struct zynq_dev;

enum zynq_state {
	ZYNQ_STATE_INVAL,
	ZYNQ_STATE_RESET,
	ZYNQ_STATE_INIT,
	ZYNQ_STATE_START,
	ZYNQ_STATE_STOP
};

/* CAN message format: 16 bytes */
typedef struct zcan_msg {
	/* first word */
	union {
		struct {
			u32 cmsg_id_ertr:1;
			u32 cmsg_id_eid:18;
			u32 cmsg_id_ide:1;
			u32 cmsg_id_srtr:1;
			u32 cmsg_id_sid:11;
		};
		u32 cmsg_id;
	};
	/* second word */
	union {
		struct {
			u32 cmsg_len_rsvd:28;
			u32 cmsg_len_len:4;
		};
		u32 cmsg_len;
	};
	/* third word */
	u32 cmsg_data1; /* MSB: DB0 DB1 DB2 DB3 */
	/* fourth word */
	u32 cmsg_data2; /* MSB: DB4 DB5 DB6 DB7 */
} zcan_msg_t;

#define	CMSG_ID_EID_MASK	0x3FFFF /* 18-bit */
#define	CMSG_ID_SID_MASK	0x7FF /* 11-bit */
#define	CMSG_ID_SID_SHIFT	21
#define	CMSG_LEN_MASK		0xF
#define	CMSG_LEN_SHIFT		28

enum zcan_ip_mode {
	ZCAN_IP_NORMAL,
	ZCAN_IP_LOOPBACK,
	ZCAN_IP_SLEEP
};

#define	ZCAN_IP_TXFIFO_MAX	64 /* up to 64 messages */
#define	ZCAN_IP_TXHPB_MAX	1  /* Tx high priority buffer: 1 message only */
#define	ZCAN_IP_RXFIFO_MAX	64 /* up to 64 messages */
#define	ZCAN_TIMEOUT_MAX	0xffffffff

#define	ZCAN_IP_MSG_MIN		256
#define	ZCAN_IP_MSG_MAX		65536
#define	ZCAN_IP_MSG_NUM		2048

typedef struct zynq_can {
	struct zynq_dev		*zdev;
	struct zynq_chan	*zchan;

	/* FPGA CAN IP registers: 0x200 x N */
	u8 __iomem		*zcan_ip_reg;
	int			zcan_ip_num;
	spinlock_t		zcan_pio_lock; /* pio gen lock */
	spinlock_t		zcan_pio_tx_lock; /* pio tx lock */
	spinlock_t		zcan_pio_tx_hi_lock; /* pio tx hi lock */
	spinlock_t		zcan_pio_rx_lock; /* pio rx lock */
	enum zynq_state		zcan_pio_state;
	int			zcan_pio_loopback;
	unsigned long		zcan_tx_timeout;
	unsigned long		zcan_rx_timeout;

	bcan_msg_t		*zcan_buf_rx_msg;
	int			zcan_buf_rx_num;
	int			zcan_buf_rx_head;
	int			zcan_buf_rx_tail;
	void __user		*zcan_usr_buf;
	int			zcan_usr_buf_num;
	int			zcan_usr_rx_num;
	struct completion	zcan_usr_rx_comp;
	struct task_struct	*zcan_pio_rx_thread;

	int			zcan_usr_rx_seq;
	int			zcan_pio_rx_last_chk_head;
	int			zcan_pio_rx_last_chk_tail;
	int			zcan_pio_rx_last_chk_cnt;
	int			zcan_pio_rx_last_chk_seq;

	struct cdev		zcan_pio_cdev; /* cdev for CAN IP */
	dev_t			zcan_pio_dev;
	int			zcan_pio_opened;
	unsigned long		zcan_pio_baudrate;
	struct cdev		zcan_cdev; /* cdev for CAN DMA channel */
	dev_t			zcan_dev;

	zynq_stats_t		stats[CAN_STATS_NUM];
	char			prefix[ZYNQ_LOG_PREFIX_LEN];
} zynq_can_t;

/* function declaration */
extern int zcan_init(zynq_can_t *zcan);
extern void zcan_fini(zynq_can_t *zcan);
extern int zcan_pio_init(zynq_can_t *zcan, enum zcan_ip_mode mode);
extern void zcan_pio_fini(zynq_can_t *zcan);
extern void zcan_pio_test(zynq_can_t *zcan, int loopback, int hi_pri,
    int msgcnt);
extern void zcan_test(zynq_can_t *zcan, int loopback, int msgcnt);
extern void zcan_rx_proc(zynq_can_t *zcan);
//extern void zcan_err_proc(zynq_can_t *zcan, int ch_err_status);
extern zynq_can_t *can_ip_to_zcan(struct zynq_dev *zdev, u32 can_ip_num);
extern void zynq_dbg_reg_dump_all(struct zynq_dev *zdev);
extern void zcan_pio_dbg_reg_dump_ch(zynq_can_t *zcan);

#endif	/* _BASA_CAN_H_ */
