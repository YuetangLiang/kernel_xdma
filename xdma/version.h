#ifndef __XDMA_VERSION_H__
#define __XDMA_VERSION_H__

#define DRV_MOD_MAJOR		2017
#define DRV_MOD_MINOR		1
#define DRV_MOD_PATCHLEVEL	47

#define DRV_MODULE_VERSION      \
	__stringify(DRV_MOD_MAJOR) "." \
	__stringify(DRV_MOD_MINOR) "." \
	__stringify(DRV_MOD_PATCHLEVEL)

#define DRV_MOD_VERSION_NUMBER  \
	((DRV_MOD_MAJOR)*1000 + (DRV_MOD_MINOR)*100 + DRV_MOD_PATCHLEVEL)

#endif /* ifndef __XDMA_VERSION_H__ */
