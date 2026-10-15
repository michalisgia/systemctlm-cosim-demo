#
# verilog-pcie example design guest DMA tests.
#
# In the guest (/opt/vpcie-tests): make
# On the host, static binaries that run on any x86_64 guest:
#   make -C pcie/verilog-pcie/scripts/guest LDFLAGS=-static
#
CC ?= gcc
CFLAGS ?= -O2 -g -Wall -Wextra

PROGS = vpcie-phys-dma-test vpcie-vfio-dma-test

all: $(PROGS)

%: %.c vpcie.c vpcie.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< vpcie.c

clean:
	rm -f $(PROGS)

.PHONY: all clean
