/**
 * [h2c] 0x0{0123}00
 * [c2h] 0x1{0123}00
 * reg_rw /dev/xdma0_control 0x${h2c/c2h}00 w | grep "Read.*:" | sed 's/Read.*: 0x\([a-z0-9]*\)/\1/'`
 *  ${statusRegVal:0:3} = "1fc":ChannelEnable
 *  ${statusRegVal:4:1} = "8":streamEnable(NOT memory mapped)
 *
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <byteswap.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <ctype.h>
#include <termios.h>

#include <sys/types.h>
#include <sys/mman.h>

/* ltoh: little to host */
/* htol: little to host */
#if __BYTE_ORDER == __LITTLE_ENDIAN
#define ltohl(x)       (x)
#define ltohs(x)       (x)
#define htoll(x)       (x)
#define htols(x)       (x)
#elif __BYTE_ORDER == __BIG_ENDIAN
#define ltohl(x)     __bswap_32(x)
#define ltohs(x)     __bswap_16(x)
#define htoll(x)     __bswap_32(x)
#define htols(x)     __bswap_16(x)
#endif

#define FATAL do { fprintf(stderr, "Error at line %d, file %s (%d) [%s]\n", __LINE__, __FILE__, errno, strerror(errno)); exit(1); } while(0)


char g_devname[50];

#define MAP_SIZE (32*1024UL)
#define MAP_MASK (MAP_SIZE - 1)
typedef struct {
    int fd;
    char *map_base;
} zynq_cfg_reg_t;

static zynq_cfg_reg_t m_zynq_cfg_reg;
void zynq_cfg_reg_exit()
{
    zynq_cfg_reg_t *reg = &m_zynq_cfg_reg;

    if(reg->map_base) {
        munmap(reg->map_base, MAP_SIZE);
        reg->map_base = NULL;
    }
    if(reg->fd) {
        close(reg->fd);
        reg->fd = 0;
    }
}
int zynq_cfg_reg_init()
{
    zynq_cfg_reg_t *reg = &m_zynq_cfg_reg;

    if ((reg->fd = open(g_devname, O_RDWR | O_SYNC)) == -1) {
        FATAL;
        return -1;
    }

    reg->map_base = mmap(0, MAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED,
                         reg->fd, 0);
	if (reg->map_base == (void *)-1) {
        goto err0;
    }



    return 0;

  err0:
    close(reg->fd);
    reg->fd = 0;
    FATAL;
    return -1;
}

int copy_from_zynq(void *to, off_t from, size_t n)
{
    zynq_cfg_reg_t *reg = &m_zynq_cfg_reg;
    if(reg->fd == 0) {
        if(zynq_cfg_reg_init() == -1) {
            return -1;
        }
    }

    char *src_addr = reg->map_base + from;
    memcpy(to, (void*)src_addr, n);

    // munmap(map_base, MAP_SIZE)
    return 0;
}

int copy_to_zynq(off_t to, void *from, size_t n)
{
    zynq_cfg_reg_t *reg = &m_zynq_cfg_reg;
    if(reg->fd == 0) {
        if(zynq_cfg_reg_init() == -1) {
            return -1;
        }
    }

    char *dst_addr = reg->map_base + to;
    memcpy((void*)dst_addr, from, n);

    // munmap(map_base, MAP_SIZE)
    return 0;
}

int main(int argc, char **argv)
{
	int fd;
	void *map_base, *virt_addr;
	uint32_t read_result, writeval;
	off_t target;
	/* access width */
	int access_width = 'w';
	char *device;

	/* not enough arguments given? */
	if (argc < 3) {
		fprintf(stderr,
			"\nUsage:\t%s <device> <address> [[type] data]\n"
			"\tdevice  : character device to access\n"
			"\taddress : memory address to access\n"
			"\ttype    : access operation type : [b]yte, [h]alfword, [w]ord\n"
			"\tdata    : data to be written for a write\n\n",
			argv[0]);
		exit(1);
	}

	printf("argc = %d\n", argc);

	device = strdup(argv[1]);
	printf("device: %s\n", device);
	target = strtoul(argv[2], 0, 0);
	printf("address: 0x%08x\n", (unsigned int)target);

	printf("access type: %s\n", argc >= 4 ? "write" : "read");

	/* data given? */
	if (argc >= 4) {
		printf("access width given.\n");
		access_width = tolower(argv[3][0]);
	}
	printf("access width: ");
	if (access_width == 'b')
		printf("byte (8-bits)\n");
	else if (access_width == 'h')
		printf("half word (16-bits)\n");
	else if (access_width == 'w')
		printf("word (32-bits)\n");
	else {
		printf("word (32-bits)\n");
		access_width = 'w';
	}

    sprintf(g_devname, "%s", argv[1]);

	/* read only */
	if (argc <= 4) {
		//printf("Read from address %p.\n", virt_addr); 
		switch (access_width) {
		case 'b':
			//read_result = *((uint8_t *) virt_addr);
            copy_from_zynq(&read_result, target, 1);
			printf
			    ("Read 8-bits value at address 0x%08x: 0x%02x\n",
			     (unsigned int)target,
			     (unsigned int)read_result);
			break;
		case 'h':
			//read_result = *((uint16_t *) virt_addr);
            copy_from_zynq(&read_result, target, 2);
			/* swap 16-bit endianess if host is not little-endian */
			read_result = ltohs(read_result);
			printf
			    ("Read 16-bit value at address 0x%08x: 0x%04x\n",
			     (unsigned int)target,
			     (unsigned int)read_result);
			break;
		case 'w':
			//read_result = *((uint32_t *) virt_addr);
            copy_from_zynq(&read_result, target, 4);
			/* swap 32-bit endianess if host is not little-endian */
			read_result = ltohl(read_result);
			printf
			    ("Read 32-bit value at address 0x%08x: 0x%08x\n",
			     (unsigned int)target,
			     (unsigned int)read_result);
			return (int)read_result;
			break;
		default:
			fprintf(stderr, "Illegal data type '%c'.\n",
				access_width);
			exit(2);
		}
		fflush(stdout);
	}
	/* data value given, i.e. writing? */
	if (argc >= 5) {
		writeval = strtoul(argv[4], 0, 0);
		switch (access_width) {
		case 'b':
			printf("Write 8-bits value 0x%02x to 0x%08x\n",
			       (unsigned int)writeval, (unsigned int)target);
			//*((uint8_t *) virt_addr) = writeval;
            copy_to_zynq(target, &writeval, 1);
			break;
		case 'h':
			printf("Write 16-bits value 0x%04x to 0x%08x\n",
			       (unsigned int)writeval, (unsigned int)target);
			/* swap 16-bit endianess if host is not little-endian */
			writeval = htols(writeval);
			//*((uint16_t *) virt_addr) = writeval;
            copy_to_zynq(target, &writeval, 2);
			break;
		case 'w':
			printf("Write 32-bits value 0x%08x to 0x%08x\n",
			       (unsigned int)writeval, (unsigned int)target);
			/* swap 32-bit endianess if host is not little-endian */
			writeval = htoll(writeval);
			//*((uint32_t *) virt_addr) = writeval;
            copy_to_zynq(target, &writeval, 4);
			break;
		}
		fflush(stdout);
	}

    zynq_cfg_reg_exit();

	return 0;
}
