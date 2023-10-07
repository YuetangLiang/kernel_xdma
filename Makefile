all:
	make -C xdma
	make -C tools

clean:
	make clean -C xdma
	make clean -C tools

install:
	make -C xdma install

.PHONY: all install
