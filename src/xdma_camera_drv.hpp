#ifndef __LIB_ZYNQ_CAM_H__
#define __LIB_ZYNQ_CAM_H__

#ifndef ZYNQ_SYNC_MODE
#define ZYNQ_SYNC_MODE 0 //0:threaded
#endif

#ifndef BASE_CAMERA
#define BASE_CAMERA 0
#endif

#define NR_CAMS 12
#define NR_BUFS 4

#define idx_of_cam(i) ((cams_cfg()->cam[(i)].images.index + NR_BUFS -1) % NR_BUFS)
#define ptr_of_cam(i) cams_cfg()->cam[(i)].images.image[idx_of_cam(i)].start


#ifdef __cplusplus
//extern "C" {
#endif
#include <stdint.h> ///usr/include/stdint.h
#include <sys/time.h>
#include <sys/types.h>
#include <sys/select.h>
#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <poll.h>
#include <dlfcn.h>
#include <sys/mman.h>

#include <map>
using namespace std;

// #include <linux/videodev2.h>
// #include <pthread.h>
// #include <sys/ioctl.h>
// #include <assert.h>
// #include <malloc.h>
//#include "osal.h"
#define __E(fmt, args...)  ({fprintf(stderr, "E/%3d: %s(): " fmt "\n",  __LINE__, __FUNCTION__, ## args);})
#define __W(fmt, args...)  ({fprintf(stderr, "W/%3d: %s(): " fmt "\n",  __LINE__, __FUNCTION__, ## args);})
#define __I(fmt, args...)  ({fprintf(stderr, "I/%3d: %s(): " fmt "\n",  __LINE__, __FUNCTION__, ## args);})
#define __D(fmt, args...)  ({fprintf(stderr, "D/%3d: %s(): " fmt "\n",  __LINE__, __FUNCTION__, ## args);})

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

typedef struct completion {
    pthread_mutex_t   mutex;
    pthread_cond_t    cond;
} completion_t;
static inline void init_completion(struct completion *x)
{
    pthread_mutex_init(&x->mutex, NULL);
    pthread_cond_init(&x->cond, NULL);
}
static inline void wait_for_completion(completion_t* x)
{
    pthread_mutex_lock(&x->mutex);
    pthread_cond_wait(&x->cond, &x->mutex);
    pthread_mutex_unlock(&x->mutex);
}
static inline void complete(completion_t* x)
{
    pthread_mutex_lock(&x->mutex);
    pthread_cond_signal(&x->cond);
    pthread_mutex_unlock(&x->mutex);
}

typedef struct {
    pthread_t           pth;
    pthread_attr_t      attr;
    int                 policy;
    struct sched_param  sched;

    void               *pth_ret;
    int                 stop;
    completion_t        wait;
    char                name[64];
} task_t;

static inline int task_run(task_t *t, void* (*th)(void *d), void *d)
{
    init_completion(&t->wait);
    pthread_create(&t->pth, NULL, th, d);
    sched_yield();

    return 0;
}


#define TH_GEN(tname)                               \
    static int    tname ## ret;                     \
    static task_t tname ## tsk;                     \
    static void* tname ## _thread(void *arg)        \
    {                                               \
        task_t *t = &tname ## tsk;                  \
        while(!t->stop) {                           \
            wait_for_completion(&t->wait);          \
            if(t->stop) break;                      \
            tname ## ret = tname();                 \
        }                                           \
        return NULL;                                \
    }                                               \
    static int tname ## _threaded()                 \
    {                                               \
        task_t *t = &tname ## tsk;                  \
        if(t->pth == 0) {                           \
            task_run(t, tname ## _thread, NULL);    \
            usleep(100000);                         \
        }                                           \
        complete(&t->wait);                         \
        return tname ## ret;                        \
    }


// 32 bytes
typedef struct {
    uint8_t        vc_num;     // 0x00
    uint8_t        err_flag;   // 0x01
    uint16_t       frame_num;  // 0x02 - 0x03
    uint64_t       time_stamp_100us:20;   // 0x04[19:0]
    uint64_t       time_stamp_sec:44;     // 0x04[63:20]

    // 0x0C
    uint16_t       frame_line_cnt;
    int8_t          reserved[18];
    // 32 bytes
} __attribute__((packed)) zynq_frame_info_t;

typedef struct {
    char          *base;
    char          *start;
    zynq_frame_info_t *info;
    size_t         length;
    int            width;
    int            height;
    struct timeval timestamp;
    int            bytes_per_pixel;
    int            image_size;
    char          *rgb; //image
    int            is_new;
    void          *gpu;
} camera_image_t;

typedef struct {
    int            vc_wr_index;
    int            index;
    camera_image_t image[NR_BUFS];
    int            nr_images;
} camera_images_t;

typedef struct {
    uint64_t        height; //if from fpga
    uint64_t        width;  //if from fpga
} __attribute__((packed)) zynq_fmt_pix_t;

typedef struct {
    int8_t        mipi_0:2; // bit[1:0] 4 cameras each mipi
    int8_t        mipi_1:2; // bit[3:2]
    int8_t        mipi_2:2; // bit[5:4]
    int8_t        mipi_3:2; // bit[7:6]

} __attribute__((packed)) zynq_ts_mode_t;

// fpga_addr:0xA0010000 - (0xA0010100 - 1)
typedef struct {
    uint64_t        vc_en;         // 0x00 [15:0]
    uint64_t        vc_wr_index_0:3;   // 0x08 [47:0]
    uint64_t        vc_wr_index_1:3;   // 0x08 [47:0]
    uint64_t        vc_wr_index_2:3;   // 0x08 [47:0]
    uint64_t        vc_wr_index_3:3;   // 0x08 [47:0]
    uint64_t        vc_wr_index_4:3;   // 0x08 [47:0]
    uint64_t        vc_wr_index_5:3;   // 0x08 [47:0]
    uint64_t        vc_wr_index_6:3;   // 0x08 [47:0]
    uint64_t        vc_wr_index_7:3;   // 0x08 [47:0]
    uint64_t        vc_wr_index_8:3;   // 0x08 [47:0]
    uint64_t        vc_wr_index_9:3;   // 0x08 [47:0]
    uint64_t        vc_wr_index_a:3;   // 0x08 [47:0]
    uint64_t        vc_wr_index_b:3;   // 0x08 [47:0]
    uint64_t        vc_wr_index_hi:28;
    zynq_fmt_pix_t  fmt_pix[4];    // 0x10 fmt_pix[0] - [3]
    zynq_ts_mode_t  ts_mode;       // 0x50
    int8_t          reserved0[7];
    uint64_t        intr_en;         // 0x58 [15:0]

    // 0x60
    int8_t          reserved[160];  // 0x100 - 0x60
    // 0x100
} __attribute__((packed)) zynq_mipi_t;

typedef struct {
    uint64_t        addr_start;      // 0x00 [40:0]
    uint64_t        buf_size;        // 0x08 [40:0] = 0x180000 TODO: w*h*bytesperpixel = width * height * 3(rgb)
    uint64_t        buf_number;      // 0x10        = 0x2      TODO: 3 - 7
    int64_t         frame_info_offset:40; // 0x18 addr_start + frame_info_offset = fpga_addr  -> zynq_frame_info_t
    int64_t         frame_info_offset_hi:24;
    uint64_t        kp_cur_buf;      // 0x20 1:keep using current buf -> fpga set 0
    uint64_t        rec_frame_count; // 0x28 frame_counter from 0 when vc_en:1

    //TODO 0x30
    int8_t          reserved[208];  // 0x100 - 0x30
    // 0x100
} __attribute__((packed)) zynq_vc_ctl_reg_t;

typedef struct {
    int     id;
    char    camera_dev[20];
    int     framerate;
    int     fd;
    camera_images_t images;
    void (*cb)(void *d);
    task_t t;
    //void *private;
} zynq_cam_t;

typedef struct {
    char      camera_dev[30];
    int       fd;
    char     *bar_start;
    size_t    bar_len;

    zynq_mipi_t        *mipi;        // 0xA0010000
    zynq_vc_ctl_reg_t  *vc_ctl_reg;  //[16];  // 0xA0010100
    // 0x1100

    int     nr_cams;
    zynq_cam_t cam[NR_CAMS];
    task_t  t;

    int     buf_size; //for each big buf of fpga
    char   *base;

    int     c2h_fd[4];

    //image_of_cam[0]->bytes_per_pixel
    int bytes_per_pixel;
    int width;
    int height;

} zynq_cams_t;

static zynq_cams_t m_zynq_cams;
zynq_cams_t* cams_cfg()
{
    return &m_zynq_cams;
}



typedef struct {
    int fd;
    pthread_mutex_t mut;
    int idx;
    void *dest;
    off_t fpga_src;
    size_t n;
    task_t t;
} c2h_t;

typedef struct {
    int   idx;
    c2h_t c2h[3];
} xdma_t;
static xdma_t xdma;

void xdma_signal_handler(int sig)
{
    int idx;
    for (idx = 0; idx < ARRAY_SIZE(xdma.c2h); idx++) {
        pthread_mutex_lock(&xdma.c2h[idx].mut);
    }
    
    __I("ros::shutdown ...");
    exit(0);
}
int c2h_memcpy(void *dest, off_t fpga_src, size_t n)
{
    char name[100];
    int idx;
    c2h_t *c2h = xdma.c2h;

    if (c2h[1].fd == 0)
    {
        for (idx = 0; idx < ARRAY_SIZE(xdma.c2h); idx++) {
            pthread_mutex_init(&c2h[idx].mut, NULL);
        }
        signal(SIGINT, xdma_signal_handler);
    }

  next:
    idx = xdma.idx;
    idx = (idx + 1) % ARRAY_SIZE(xdma.c2h);
    xdma.idx = idx;
    c2h = &xdma.c2h[idx];

    pthread_mutex_lock(&c2h->mut);

    sprintf(name, "/dev/xdma0_c2h_%d", idx);
    if(c2h->fd == 0) {
        c2h->fd = open(name, O_RDONLY);
    }

    if(c2h->fd == -1) {
        __E("%s open failed \n", name);
        pthread_mutex_unlock(&c2h->mut);
        goto next;
    }

    int ret = pread(c2h->fd, dest, n, fpga_src);
    pthread_mutex_unlock(&c2h->mut);

    return ret;
}

static void* c2h_thread(void *arg)
{
    char name[100];
    c2h_t *c2h = (typeof(c2h))arg;
    task_t *t   = &c2h->t;


    sprintf(name, "/dev/xdma0_c2h_%d", c2h->idx);
    if(c2h->fd == 0) {
        c2h->fd = open(name, O_RDONLY);
    }

    if(c2h->fd == -1) {
        __E("%s open failed \n", name);
        return NULL;
    }


    while(!t->stop)
    {
        wait_for_completion(&t->wait);
        if (t->stop) {
            break;
        }

        pthread_mutex_lock(&c2h->mut);
        int ret = pread(c2h->fd, c2h->dest, c2h->n, c2h->fpga_src);
        pthread_mutex_unlock(&c2h->mut);

    }
    return NULL;
}

int c2h_memcpy_threaded(void *dest, off_t fpga_src, size_t n)
{
    int idx;
    c2h_t *c2h = xdma.c2h;

    if (c2h[1].fd == 0)
    {
        for (idx = 0; idx < ARRAY_SIZE(xdma.c2h); idx++) {
            pthread_mutex_init(&c2h[idx].mut, NULL);
        }
        signal(SIGINT, xdma_signal_handler);
    }

    idx = xdma.idx;
    idx = (idx + 1) % ARRAY_SIZE(xdma.c2h);
    xdma.idx = idx;
    c2h = &xdma.c2h[idx];

    c2h->idx      = idx;
    c2h->fpga_src = fpga_src;
    c2h->dest     = dest;
    c2h->n        = n;

    task_t *t = &c2h->t;
    if (t->pth == 0) {
        task_run(t, c2h_thread, (void *)c2h);
    }
    complete(&t->wait);

    return 0;
}

typedef enum irqreturn {
    IRQ_NONE        = (0 << 0),
    IRQ_HANDLED     = (1 << 0),
    IRQ_WAKE_THREAD = (1 << 1),
} irqreturn_t;
typedef irqreturn_t (*irq_handler_t)(int, void *);
typedef struct irqaction {
#define IRQF_SHARED     0x00000080
#define IRQF_NO_THREAD  0x00010000
    unsigned long flags;

    irq_handler_t handler;
    task_t t;
    //char *name;
    void *dev_id;
    int fd;
    unsigned int irq;
} irqaction_t;

typedef struct irq_manage {
    task_t t;
#define MIPI_0_IRQ 0
#define MIPI_1_IRQ 1
#define MIPI_2_IRQ 2
#define NR_IRQ     16
    irqaction_t irqactions[16];
    struct pollfd fds[16];
    int nr_fds;
    int timeout;
    map<int,void*> fd2action;
} irq_manage_t;
static irq_manage_t m_irq_manage;

static void* irq_thread(void *arg)
{
    irqaction_t *action = (typeof(action))arg;
    task_t *t   = &action->t;
    __I("irq:%u", action->irq);
    while(!t->stop)
    {
        wait_for_completion(&t->wait);
        if (t->stop) {
            break;
        }

        if(action->handler) {
            //__I("Interrupt:%u", action->irq);
            action->handler(action->irq, action->dev_id);
        }

        //uint32_t events_usr = 0;
        //int rc = pread(action->fd, &events_usr, sizeof(events_usr), 0);
    }

    return NULL;
}

static void* irq_manager(void *arg)
{
    irq_manage_t *im = (typeof(im))arg;
    task_t *t   = &im->t;
    int rc,rd, i;
    uint32_t events_usr;

    while(!t->stop &&
          (rd = poll(im->fds, im->nr_fds, im->timeout)) != -1)
    {
        if (t->stop) {
            break;
        }

        if(rd <= 0) {
            __I("..."); //No Interrupts
            continue;
        }

#if 0
        for(i = 0; i < im->nr_fds; i++) {
            fprintf(stderr, "[%d: %d] ", i, im->fds[i].revents);
        }
        fprintf(stderr, "\n");
#endif

        for(i = 0; i < im->nr_fds; i++)
        {
            if(im->fds[i].revents & POLLIN)
            {
                int fd = im->fds[i].fd;
                irqaction_t *action = (typeof(action))im->fd2action[fd];
                //__I("[%d/%d] fd:%d ", i, im->nr_fds, fd);

#if 0
                //ARRAY solution
                for(irq = 0; irq < ARRAY_SIZE(im->irqactions); irq++)
                {
                    irqaction_t *action = &im->irqactions[irq];
#endif


                    if(action->handler == NULL ||
                       action->fd      != fd)
                    {
                        __E("..."); //Err Interrupts
                        return NULL;
                        continue;
                    }

                    if(action->flags == IRQF_NO_THREAD) {
                        //__I("Interrupt:%d", action->irq);
                        if(action->handler) {
                            action->handler(action->irq, action->dev_id);
                        }

                        rc = pread(fd, &events_usr, sizeof(events_usr), 0);
                        continue;
                    }

                    //multithread
                    task_t *t = &action->t;
                    if(t->pth == 0) {
                        task_run(t, irq_thread, (void*)action);
                    }
                    complete(&t->wait);

                    rc = pread(fd, &events_usr, sizeof(events_usr), 0);

#if 0
                }//foreach ARRAY
#endif

            }//endif
        }

    }

    return NULL;
}

int request_irq(unsigned int irq, irq_handler_t handler,
                unsigned long flags, char *name, void *dev_id)
{
    irq_manage_t *im = &m_irq_manage;

    char dev_name[100];
    sprintf(dev_name, "/dev/%s_events_%d", name, irq);
    __I("opening %s", dev_name);
    int fd = open(dev_name, O_RDWR);
    if(fd == -1) {
        __E("invalid interrupt:%d \n", irq);
        return -1;
    }


    irqaction_t *action = &im->irqactions[irq];
    action->handler = handler;
    action->flags   = flags;
    //action->name    = name;
    action->dev_id  = dev_id;
    action->irq     = irq;
    action->fd      = fd;
    im->fds[im->nr_fds].fd     = fd;
    im->fds[im->nr_fds].events = POLLIN;
    im->fd2action[fd] = (void*)action;
    im->nr_fds++;

    task_t *t = &im->t;
    if(t->pth == 0) {
        im->timeout = 10000;//10s
        task_run(t, irq_manager, (void*)im);
    }
    complete(&t->wait);
    return 0;
}

static void free_irq(unsigned int irq)
{
    irq_manage_t *im    = &m_irq_manage;
    irqaction_t *action = &im->irqactions[irq];
    action->handler = NULL;
    action->t.stop = 1;
    action->t.pth  = 0;
}

void uninit_device(zynq_cams_t *cams)
{
    munmap(cams->bar_start, cams->bar_len);
    close(cams->fd);
}

int init_device(zynq_cams_t *cams)
{
    int i,j;

    if(strlen(cams->camera_dev) == 0) {
        sprintf(cams->camera_dev, "/dev/zynq_gps0");
    }

    if (-1 == (cams->fd = open(cams->camera_dev, O_RDWR | O_SYNC))) {
        __E("Cannot open %s \n", cams->camera_dev);
        return -1;
    }

    cams->bar_len = 512*1024UL;
    cams->bar_start = (char*)mmap(0,
                           cams->bar_len, PROT_READ | PROT_WRITE, MAP_SHARED,
                           cams->fd, 0);
	if (cams->bar_start == (void *)-1) {
        return -1;
    }
    cams->mipi       = (typeof(cams->mipi))       (cams->bar_start + 0x00010000);
    cams->vc_ctl_reg = (typeof(cams->vc_ctl_reg)) (cams->bar_start + 0x00010100);

    zynq_vc_ctl_reg_t  *reg = cams->vc_ctl_reg;
    camera_image_t     *image_of_cam[NR_CAMS];

    image_of_cam[0] = cams->cam[0].images.image;

    if (cams->width == 0) {
        image_of_cam[0]->width  = 1920; //default,cams->mipi->fmt_pix[0].width;
    } else {
        image_of_cam[0]->width  = cams->width;
    }
    if (cams->height == 0) {
        image_of_cam[0]->height = 1080; //default
    } else {
        image_of_cam[0]->height = cams->height;
    }
    if (cams->bytes_per_pixel == 0) {
        image_of_cam[0]->bytes_per_pixel = 2; //default: 2 bytes(yuv422/raw12)
    } else {
        image_of_cam[0]->bytes_per_pixel = cams->bytes_per_pixel;
    }

    image_of_cam[0]->length = 0x20 + image_of_cam[0]->width * image_of_cam[0]->height * image_of_cam[0]->bytes_per_pixel;

    cams->buf_size = image_of_cam[0]->length * NR_CAMS; //+ cam[]->length
    if(posix_memalign((void**)&cams->base, 4096, cams->buf_size * NR_BUFS + 4096)) {
        __E("OOM %lu \n", cams->buf_size + 4096);
        return -1;
    }

    for (i = 0; i < ARRAY_SIZE(cams->cam); i++)
    {
        camera_image_t *image = cams->cam[i].images.image;

        if (i == 0)
        {
            image_of_cam[0]->base  = cams->base;

            reg[0].frame_info_offset = -0x20;
            reg[0].buf_number = NR_BUFS;
            reg[0].buf_size   = cams->buf_size;
            reg[0].addr_start = 0x5000000020;
        }
        else
        {
            image_of_cam[i] = &image[0];//save image[0] of EachCamera
            image[0] = *image_of_cam[0];
            image[0].base  = image_of_cam[i-1]->base + image_of_cam[i-1]->length;

            reg[i] = reg[i-1];
            reg[i].addr_start = reg[i-1].addr_start + image_of_cam[i-1]->length;
        }

        image[0].info  = (zynq_frame_info_t*)image[0].base;
        image[0].start = image[0].base + 0x20;
        for(j = 1; j < ARRAY_SIZE(cams->cam->images.image); j++) {
            image[j] = image[0];
            image[j].base = image[j-1].base + cams->buf_size;
            image[j].info = (zynq_frame_info_t*)image[j].base;
            image[j].start = image[j].base + 0x20;
        }
    }

    cams->mipi->vc_en = 0xFFF;
    __I("MIPI: vc_en = 0x%X, vc_wr_index = 0x%X",
        cams->mipi[0].vc_en,
        cams->mipi[0].vc_wr_index_0);


    return 0;
}

// trigger cams_cfg()->cam[CAM_ID].t.stop = 1;
void stop(zynq_cam_t *cam)
{
    int i;
    zynq_cams_t *cams = &m_zynq_cams;

    if (cam == NULL)
    {
        //trigger stop ALL
        for(i = 0; i < ARRAY_SIZE(cams->cam); i++)
        {
            cam = &cams->cam[i];

            if (cam->t.pth) {
                cam->t.stop = 1;
            }
        }
        return;
    }

    //cams->mipi[0].vc_en &= ~(1 << cam->id);

    cam->cb = NULL;
}

static int start(zynq_cam_t *cam)
{
    int i;
    zynq_cams_t *cams = &m_zynq_cams;
    cams->mipi[0].vc_en |= 1 << cam->id;
    //sleep(1);
    return 0;
    //cams->mipi[0].vc_en &= ~(1 << cam->id);
}

void zynq_cam(zynq_cam_t *cam)
{
    int idx = cam->images.index;
    camera_image_t *img = &cam->images.image[idx];
    struct tm tm = {0};
    struct timeval ts_1970;

    tm.tm_year = 1970;
    tm.tm_mon  = 0; // 1
    tm.tm_mday = 1;
    ts_1970.tv_sec = mktime(&tm);

    img->timestamp = ts_1970;
    img->timestamp.tv_sec = img->info->time_stamp_sec;
    img->timestamp.tv_usec = img->info->time_stamp_100us * 100;

    if(cam->cb) {
        cam->cb((void*)cam);
    }
}

static void* zynq_cam_thread(void *arg)
{
    zynq_cams_t  *cams = &m_zynq_cams;

    zynq_cam_t  *cam = (typeof(cam))arg;
    task_t *t   = &cam->t;


    if(start(cam) == -1) {
        __E("start cam:%d failed\n", cam->id);
        goto err;
    }

    usleep((cam->id) * 50000);
    //__I("cam_id:%d ", cam->id);
    while(!t->stop)
    {
        wait_for_completion(&t->wait);
        if (t->stop) {
            break;
        }


        //char cam_name[20];
        //sprintf(cam_name, "cam%d_idx", cam->id);
        zynq_cam(cam);
    }

    stop(cam);
    t->pth  = 0;
    t->stop = 0;

  err:
    return NULL;
}

#define IDX(i)  ((idx + NR_BUFS + (i)) % NR_BUFS)
#define chk_cam(cam_id) ({\
                cam = &cams->cam[0x ##cam_id];                          \
                cam->images.vc_wr_index = cams->mipi->vc_wr_index_ ## cam_id; \
                idx = cam->images.vc_wr_index;                          \
                idx = IDX(-1);                                          \
                cam->images.index = idx;                                \
            })
#define sizeof_cams(nums) (cams->cam->images.image->length * (nums))
#define FPGA_BASE         (cams->vc_ctl_reg->addr_start - 0x20)

static irqreturn_t mipi0_isr(int irq, void *dev_id)
{
    zynq_vc_ctl_reg_t *vc;
    zynq_cams_t  *cams = (typeof(cams))dev_id;
    zynq_cam_t   *cam;
    int i, idx, off, base_camera;
    base_camera = 0;

    if ((cams->mipi->vc_en & 0xF) == 0) {
        goto done;
    }

    chk_cam(0);
    chk_cam(1);
    chk_cam(2);
    chk_cam(3);


    for(i = 0; i < 4; i++)
    {
        if ((cams->mipi->vc_en & (1 << i)) == 0) {
            continue;
        }

        vc  = &cams->vc_ctl_reg[i];

        if(idx_of_cam(base_camera) != idx_of_cam(i)) {
            //__I("[base_camera:%d]%d != [camera:%d]%d", base_camera, idx_of_cam(base_camera), i, idx_of_cam(i));
            vc->kp_cur_buf = 1;
        }
    }


    idx = cams->cam[base_camera].images.index;
    off = idx * cams->buf_size;
    c2h_memcpy(cams->base+off,
               FPGA_BASE+off,
               sizeof_cams(4)); // cp for 0-3 cameras

    for(i = 0; i < 4; i++)
    {
        cam = &cams->cam[i];

        if(cam->cb)
        {
            if(ZYNQ_SYNC_MODE) {
                zynq_cam(cam);
                continue;//next cam
            }
            
            task_t *t = &cam->t;
            //start cam en:1 first
            if (t->pth == 0) {
                task_run(t, zynq_cam_thread, (void*)cam);
            }
            complete(&t->wait);
        }
    }

  done:
    return IRQ_HANDLED;
}

static irqreturn_t mipi1_isr(int irq, void *dev_id)
{
    zynq_vc_ctl_reg_t *vc;
    zynq_cams_t  *cams = (typeof(cams))dev_id;
    zynq_cam_t   *cam;
    int i, idx, off, base_camera;
    base_camera = 4;

    if ((cams->mipi->vc_en & 0xF0) == 0) {
        goto done;
    }

    chk_cam(4);
    chk_cam(5);
    chk_cam(6);
    chk_cam(7);

    for(i = 4; i < 8; i++)
    {
        if ((cams->mipi->vc_en & (1 << i)) == 0) {
            continue;
        }

        vc  = &cams->vc_ctl_reg[i];

        if(idx_of_cam(base_camera) != idx_of_cam(i)) {
            //__I("[base_camera:%d]%d != [camera:%d]%d", base_camera, idx_of_cam(base_camera), i, idx_of_cam(i));
            vc->kp_cur_buf = 1;
        }
    }


    idx = cams->cam[base_camera].images.index;
    off = idx * cams->buf_size + sizeof_cams(4);// skip 0,1,2,...3 cameras
    c2h_memcpy(cams->base+off,
               FPGA_BASE+off,
               sizeof_cams(4)); // cp for 4-7 cameras

    for(i = 4; i < 8; i++)
    {
        cam = &cams->cam[i];

        if(cam->cb)
        {
            if(ZYNQ_SYNC_MODE) {
                zynq_cam(cam);
                continue;//next cam
            }

            task_t *t = &cam->t;

            //start cam en:1 first
            if (t->pth == 0) {
                task_run(t, zynq_cam_thread, (void*)cam);
            }
            complete(&t->wait);
        }
    }

  done:
    return IRQ_HANDLED;
}

static irqreturn_t mipi2_isr(int irq, void *dev_id)
{
    zynq_vc_ctl_reg_t *vc;
    zynq_cams_t  *cams = (typeof(cams))dev_id;
    zynq_cam_t   *cam;
    int i, idx, off, base_camera;
    base_camera = 8;

    if ((cams->mipi->vc_en & 0xF00) == 0) {
        goto done;
    }

    chk_cam(8);
    chk_cam(9);
    chk_cam(a);
    chk_cam(b);
    // NR_CAMS

    for(i = 8; i < 12; i++)
    {
        if ((cams->mipi->vc_en & (1 << i)) == 0) {
            continue;
        }

        vc  = &cams->vc_ctl_reg[i];

        if(idx_of_cam(base_camera) != idx_of_cam(i)) {
            //__I("[base_camera:%d]%d != [camera:%d]%d", base_camera, idx_of_cam(base_camera), i, idx_of_cam(i));
            vc->kp_cur_buf = 1;
        }
    }

    idx = cams->cam[base_camera].images.index;
    off = idx * cams->buf_size + sizeof_cams(8); // skip 0,1,2,...7 cameras
    c2h_memcpy(cams->base+off,
               FPGA_BASE+off,
               sizeof_cams(4)); // cp for 8-b cameras

    for(i = 8; i < 12; i++)
    {
        cam = &cams->cam[i];

        if(cam->cb)
        {
            if(ZYNQ_SYNC_MODE) {
                zynq_cam(cam);
                continue;//next cam
            }

            task_t *t = &cam->t;
            //cam->images.vc_wr_index should counting

            //start cam en:1 first
            if (t->pth == 0) {
                task_run(t, zynq_cam_thread, (void*)cam);
            }
            complete(&t->wait);
        }
    }

  done:
    return IRQ_HANDLED;
}

static void* zynq_cams_thread(void *arg)
{
    int ret, base_camera;//, i, off
    zynq_cams_t  *cams = (typeof(cams))arg;
    task_t *t   = &cams->t;



    if(init_device(cams) == -1) {
        return NULL;
    }

    ret = request_irq(MIPI_0_IRQ, mipi0_isr, IRQF_SHARED, "xdma0", (void*)cams);
    ret = request_irq(MIPI_1_IRQ, mipi1_isr, IRQF_SHARED, "xdma0", (void*)cams);
    ret = request_irq(MIPI_2_IRQ, mipi2_isr, IRQF_SHARED, "xdma0", (void*)cams);

    while(!t->stop)
    {
        //int          idx;
        //zynq_cam_t  *cam;
        if (cams->mipi->vc_en & 0xF) {
            base_camera = 0;
            cams->mipi->intr_en |= 1 << base_camera;
        }

        if (cams->mipi->vc_en & 0xF0) {
            base_camera = 4;
            cams->mipi->intr_en |= 1 << base_camera;
        }

        if (cams->mipi->vc_en & 0xF00) {
            base_camera = 8;
            cams->mipi->intr_en |= 1 << base_camera;
        }

        //RATE(30);
        sleep(1);
    }

    free_irq(MIPI_0_IRQ);
    free_irq(MIPI_1_IRQ);
    free_irq(MIPI_2_IRQ);
    stop(NULL);
    uninit_device(cams);

    t->pth  = 0;
    t->stop = 0;

    //err:
    return NULL;
}

// trigger cams_cfg()->t.stop = 1;
int subscribe_zynq_cam(int cam_id, void (*cb)(void *d))
{
    zynq_cams_t  *cams = &m_zynq_cams;
    zynq_cam_t   *cam = &cams->cam[cam_id];
    cam->cb = cb;
    cam->id = cam_id;

    //__I("BASE_CAMERA:%d \n", BASE_CAMERA);

    cams->nr_cams = std::max(cams->nr_cams, cam_id+1);
    task_t  *t = &cams->t;
    if(t->pth == 0) {
        task_run(t, zynq_cams_thread, (void*)cams);
        sleep(1);
    }

    if(cams->mipi == NULL) {
        return -1;
    }

    cams->mipi->vc_en |= 1 << cam_id;
    return 0;
}

int testcase_01(int cam_id)
{
    int ret = 0;
    int w = 1280;
    int h = 1080;
    int i,j,k;
    uint16_t a,b;
    uint16_t *raw = (typeof(raw))ptr_of_cam(cam_id);

    int sign;
    for (i = 1; i < 300; i++)
    {
        a = raw[i*w];
        if (a & 0x0800)
        {
            sign = -1;
        } else {
            sign = 1;
        }

        for (j = 0; j < w; j += 4)
        {
            if (j >= 12 && j <= 1280)
            {
                b = a + (j/4 * sign); // +/-: 0, 1, 2
                if (raw[i*w+j+0] != b+0 ||
                    raw[i*w+j+1] != b+1 ||
                    raw[i*w+j+2] != b+2 ||
                    raw[i*w+j+3] != b+3)
                {
                    printf("[%d,%d] = [%d,a:0x%X,b:0x%X]0x%X,%X,%X,%X \n", i,j,sign,a,b,
                            raw[i*w+j+0],
                            raw[i*w+j+1],
                            raw[i*w+j+2],
                            raw[i*w+j+3]
                        );
                    ret++;
                }
            }
        }
    }

    return ret; //err pixel cnt
}

#ifdef __cplusplus
//}
#endif

#endif

