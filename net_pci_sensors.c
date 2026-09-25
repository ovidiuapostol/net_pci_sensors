/***************************************************************
 *  net_pci_sensors.c
 *
 *  Purpose:
 *      Demonstrates how to multiplex three different data sources
 *      using epoll():
 *
 *          1. A custom char device  (/dev/pci_sensors)
 *          2. A CAN RAW socket      (vcan0)
 *          3. A TCP client socket   (127.0.0.1:9000)
 *
 *      Whenever the sensor driver produces new data, the program:
 *          - reads the sensor struct
 *          - sends it as a CAN frame
 *          - sends it as a TCP message
 *
 *      The program also listens for incoming CAN and TCP data.
 *
 *  Key concepts:
 *      - epoll for event-driven multiplexing
 *      - non-blocking sockets
 *      - CAN RAW sockets (SocketCAN)
 *      - TCP client sockets
 *      - char device read() integration
 *
 ***************************************************************/

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>        // needed for inet_addr()
#include <linux/can.h>
#include <linux/can/raw.h>
#include <linux/if.h>
#include <sys/ioctl.h>

/***************************************************************
 *  Sensor data structure
 *  Must match what the kernel driver writes.
 ***************************************************************/
struct sensor_regs {
    int temp;
    int pressure;
    int status;
};

/***************************************************************
 *  open_sensor_dev()
 *
 *  Opens your char device /dev/pci_sensors in NON-BLOCKING mode.
 *
 *  Important:
 *      Even though O_NONBLOCK is used, your driver *blocks*
 *      inside wait_event_interruptible(). This is OK because
 *      epoll wakes up only when data is ready.
 ***************************************************************/
static int open_sensor_dev(void)
{
    int fd = open("/dev/pci_sensors", O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        perror("open /dev/pci_sensors");
        exit(1);
    }
    return fd;
}

/***************************************************************
 *  open_can_socket()
 *
 *  Creates a CAN RAW socket and binds it to a specific CAN
 *  interface (e.g., "vcan0").
 *
 *  Why bind() is required:
 *      CAN has no routing, no IP, no ports.
 *      The kernel MUST know which CAN controller to use.
 ***************************************************************/
static int open_can_socket(const char *ifname)
{
    int s;
    struct ifreq ifr;
    struct sockaddr_can addr;

    // Create CAN RAW socket
    s = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (s < 0) {
        perror("socket CAN");
        exit(1);
    }

    // Copy interface name (e.g., "vcan0")
    strcpy(ifr.ifr_name, ifname);

    // Query kernel for interface index
    if (ioctl(s, SIOCGIFINDEX, &ifr) < 0) {
        perror("SIOCGIFINDEX");
        exit(1);
    }

    // Prepare CAN address structure
    addr.can_family  = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;

    // Bind CAN socket to that interface
    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind CAN");
        exit(1);
    }

    return s;
}

/***************************************************************
 *  open_tcp_socket()
 *
 *  Creates a TCP CLIENT socket and connects to a server.
 *
 *  Why no bind()?
 *      TCP clients do NOT bind. The kernel chooses:
 *          - local IP
 *          - local port
 *          - routing path
 *
 *      Only TCP servers call bind() + listen() + accept().
 ***************************************************************/
static int open_tcp_socket(const char *ip, int port)
{
    int s = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (s < 0) {
        perror("socket TCP");
        exit(1);
    }

    // Prepare server address
    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(port),
        .sin_addr.s_addr = inet_addr(ip)
    };

    // Non-blocking connect
    connect(s, (struct sockaddr *)&addr, sizeof(addr));

    return s;
}

/***************************************************************
 *  main()
 *
 *  Sets up epoll, registers all FDs, and enters the event loop.
 *
 *  epoll_wait():
 *      - blocks until ANY FD becomes readable
 *      - returns number of ready FDs
 *      - fills events[] array with details
 *
 ***************************************************************/
int main(void)
{
    int fd_sensor = open_sensor_dev();
    int fd_can    = open_can_socket("vcan0");
    int fd_tcp    = open_tcp_socket("127.0.0.1", 9000);

    // Create epoll instance
    int ep = epoll_create1(0);
    if (ep < 0) {
        perror("epoll_create1");
        exit(1);
    }

    // We want EPOLLIN events (readable FDs)
    struct epoll_event ev = { .events = EPOLLIN };

    // Register sensor FD
    ev.data.fd = fd_sensor;
    epoll_ctl(ep, EPOLL_CTL_ADD, fd_sensor, &ev);

    // Register CAN FD
    ev.data.fd = fd_can;
    epoll_ctl(ep, EPOLL_CTL_ADD, fd_can, &ev);

    // Register TCP FD
    ev.data.fd = fd_tcp;
    epoll_ctl(ep, EPOLL_CTL_ADD, fd_tcp, &ev);

    printf("epoll CAN+TCP multiplexer running...\n");

    /***********************************************************
     *  Main event loop
     *
     *  epoll_wait():
     *      - blocks until at least one FD is readable
     *      - returns number of ready FDs (n)
     *      - fills events[] with details
     ***********************************************************/
    while (1) {
        struct epoll_event events[8];   // up to 8 events at once
        int n = epoll_wait(ep, events, 8, -1);

        // Process all ready FDs
        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;

            /***************************************************
             *  Sensor FD ready
             ***************************************************/
            if (fd == fd_sensor) {
                struct sensor_regs regs;
                int r = read(fd_sensor, &regs, sizeof(regs));

                if (r == sizeof(regs)) {
                    printf("Sensor: temp=%d pressure=%d status=%d\n",
                           regs.temp, regs.pressure, regs.status);

                    // Build CAN frame
                    struct can_frame frame = {
                        .can_id  = 0x123,
                        .can_dlc = 8
                    };

                    // Copy sensor struct into CAN payload (max 8 bytes)
                    memcpy(frame.data, &regs,
                           sizeof(regs) > 8 ? 8 : sizeof(regs));

                    // Send CAN frame
                    write(fd_can, &frame, sizeof(frame));

                    // Send TCP message
                    char msg[64];
                    int len = snprintf(msg, sizeof(msg),
                                       "T=%d P=%d S=%d\n",
                                       regs.temp, regs.pressure, regs.status);
                    write(fd_tcp, msg, len);
                }
            }

            /***************************************************
             *  CAN FD ready (incoming CAN frame)
             ***************************************************/
            if (fd == fd_can) {
                struct can_frame frame;
                int r = read(fd_can, &frame, sizeof(frame));

                if (r > 0) {
                    printf("CAN RX id=%03X dlc=%d\n",
                           frame.can_id, frame.can_dlc);
                }
            }

            /***************************************************
             *  TCP FD ready (incoming TCP data)
             ***************************************************/
            if (fd == fd_tcp) {
                char buf[128];
                int r = read(fd_tcp, buf, sizeof(buf));

                if (r > 0) {
                    buf[r] = 0;   // null-terminate
                    printf("TCP RX: %s\n", buf);
                }
            }
        }
    }

    return 0;
}

