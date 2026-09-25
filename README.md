**net_pci_sensors** is a user-space application that demonstrates how to integrate a Linux character device driver with multiple communication interfaces using an event-driven architecture based on *epoll()*.

The application reads sensor data from the companion kernel module pci_sensors, then forwards the same data to:

    a SocketCAN interface (vcan0)
    a TCP connection (127.0.0.1:9000)

At the same time, it monitors incoming CAN and TCP traffic using a single epoll() event loop.


Architecture
```
+----------------------+
|  pci_sensors driver  |
|----------------------|
| Virtual PCI device   | 
| Kernel thread        |
| Temperature sensor   |
| Pressure sensor      |
+----------+-----------+
           |
           | read()
           v
+----------------------+
| net_pci_sensors      |
|----------------------|
| epoll() event loop   |
| Multiplexer          |
+-----+-----------+----+
      |           |
      |           |
      v           v
+---------+ +---------+
| vcan0   | | TCP/IP  |
|SocketCAN| |127.0.0.1|
+---------+ +---------+
```
**Features**

    Reads sensor data from /dev/pci_sensors
    Uses epoll() for efficient event multiplexing
    Sends sensor values as CAN frames
    Sends sensor values as TCP messages
    Receives incoming CAN traffic
    Receives incoming TCP traffic
    Uses non-blocking sockets
    Demonstrates integration between kernel-space and user-space components

**SocketCAN utilities:**

    sudo apt install can-utils

**Building**

***Compile the application:***

    make

The **generated executable** is: *net_pci_sensors.o*


**Companion Kernel Driver**

This application requires the companion kernel module: *pci_sensors*

GitHub: https://github.com/ovidiuapostol/pci_sensors


The driver provides: /dev/pci_sensors
 

which is consumed by this application.

**Preparing the Test Environment**
1. *Load the kernel module*

        sudo insmod pci_sensors.ko

    *Verify:*

        dmesg | grep pci_sensors

2. *Create a virtual CAN interface* 

        sudo modprobe vcan
        sudo ip link add dev vcan0 type vcan
        sudo ip link set up vcan0

    *Verify:*


        ip link show vcan0

3. *Start a TCP listener*

        In a separate terminal: nc -l 9000

4. *Start a CAN sniffer*

        In another terminal: candump vcan0

**Running**

Start the multiplexer:  ./net_pci_sensors.o

**Example output:**

    epoll CAN+TCP multiplexer running... 
    Sensor: temp=101 pressure=202 status=0
    Sensor: temp=102 pressure=204 status=0
    Sensor: temp=103 pressure=206 status=0

**Data Flow**

When new sensor data becomes available:

    Linux kernel driver updates virtual sensor registers
    Kernel thread wakes waiting readers
    epoll() wakes up
    Application reads sensor values
    Sensor values are encapsulated into a CAN frame
    Sensor values are formatted as a TCP message
    Data is transmitted through both interfaces

**Example TCP data:**

    T=101 P=202 S=0
 
**Example CAN frame:**

    vcan0 123 [8] 65 00 00 00 CA 00 00 00
