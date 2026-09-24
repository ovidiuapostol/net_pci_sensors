CC=gcc
CFLAGS=-Wall -o2

all: net_pci_sensors

net_pci_sensors: net_pci_sensors.c
	$(CC) $(CFLAGS) -o net_pci_sensors net_pci_sensors.c

clean:
	rm -f net_pci_sensors