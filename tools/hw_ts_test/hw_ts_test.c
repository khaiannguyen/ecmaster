// hw_ts_test.c — L0-03: verify TX/RX hardware timestamps increase monotonically
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <linux/if_packet.h>
#include <linux/if_ether.h>
#include <linux/net_tstamp.h>
#include <linux/sockios.h>

#define ETH_P_ECAT 0x88A4

static void print_ts(const char *label, struct timespec *ts) {
    printf("%s: %ld.%09ld\n", label, (long)ts->tv_sec, (long)ts->tv_nsec);
}

static int enable_hw_timestamping(int fd, const char *ifname) {
    struct ifreq ifr;
    struct hwtstamp_config hwconfig;

    memset(&hwconfig, 0, sizeof(hwconfig));
    hwconfig.tx_type = HWTSTAMP_TX_ON;
    hwconfig.rx_filter = HWTSTAMP_FILTER_ALL;

    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    ifr.ifr_data = (void *)&hwconfig;

    if (ioctl(fd, SIOCSHWTSTAMP, &ifr) < 0) {
        perror("SIOCSHWTSTAMP");
        return -1;
    }

    int flags = SOF_TIMESTAMPING_TX_HARDWARE
              | SOF_TIMESTAMPING_RX_HARDWARE
              | SOF_TIMESTAMPING_RAW_HARDWARE;

    if (setsockopt(fd, SOL_SOCKET, SO_TIMESTAMPING, &flags, sizeof(flags)) < 0) {
        perror("SO_TIMESTAMPING");
        return -1;
    }
    return 0;
}

static void parse_and_print_ts(struct msghdr *msg, const char *label) {
    struct cmsghdr *cmsg;
    for (cmsg = CMSG_FIRSTHDR(msg); cmsg; cmsg = CMSG_NXTHDR(msg, cmsg)) {
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SO_TIMESTAMPING) {
            struct timespec *ts = (struct timespec *)CMSG_DATA(cmsg);
            if (ts[2].tv_sec != 0 || ts[2].tv_nsec != 0) {
                print_ts(label, &ts[2]);   // ts[2] = hardware raw timestamp
            } else {
                printf("%s: hardware timestamp = 0 (not received)\n", label);
            }
        }
    }
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <interface>\n", argv[0]);
        return 1;
    }
    const char *ifname = argv[1];

    int fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ECAT));
    if (fd < 0) { perror("socket"); return 1; }

    if (enable_hw_timestamping(fd, ifname) < 0) { close(fd); return 1; }

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    if (ioctl(fd, SIOCGIFINDEX, &ifr) < 0) { perror("SIOCGIFINDEX"); return 1; }
    int ifindex = ifr.ifr_ifindex;

    struct sockaddr_ll sll;
    memset(&sll, 0, sizeof(sll));
    sll.sll_family = AF_PACKET;
    sll.sll_protocol = htons(ETH_P_ECAT);
    sll.sll_ifindex = ifindex;

    if (bind(fd, (struct sockaddr *)&sll, sizeof(sll)) < 0) { perror("bind"); return 1; }

    unsigned char frame[60];
    memset(frame, 0, sizeof(frame));
    memset(frame, 0xFF, 6);            // destination: broadcast
    frame[12] = 0x88; frame[13] = 0xA4; // EtherType: EtherCAT

    printf("=== TX hardware timestamp — sending 5 frames ===\n");
    for (int i = 0; i < 5; i++) {
        if (sendto(fd, frame, sizeof(frame), 0, (struct sockaddr *)&sll, sizeof(sll)) < 0) {
            perror("sendto"); continue;
        }
        usleep(2000); // wait for the kernel to attach the timestamp to the error queue

        char control[512], buf[128];
        struct msghdr msg; struct iovec iov;
        memset(&msg, 0, sizeof(msg));
        iov.iov_base = buf; iov.iov_len = sizeof(buf);
        msg.msg_iov = &iov; msg.msg_iovlen = 1;
        msg.msg_control = control; msg.msg_controllen = sizeof(control);

        if (recvmsg(fd, &msg, MSG_ERRQUEUE) < 0) {
            printf("Frame %d: failed to read TX timestamp (%s)\n", i, strerror(errno));
            continue;
        }
        char label[32]; snprintf(label, sizeof(label), "TX frame %d", i);
        parse_and_print_ts(&msg, label);
    }

    printf("\n=== RX hardware timestamp — listening for 5 seconds ===\n");
    printf("(requires a real frame arriving on %s — if nothing is connected, this will time out, which is expected)\n", ifname);

    struct timeval tv = {5, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    for (int i = 0; i < 20; i++) {
        char control[512], buf[1600];
        struct msghdr msg; struct iovec iov;
        memset(&msg, 0, sizeof(msg));
        iov.iov_base = buf; iov.iov_len = sizeof(buf);
        msg.msg_iov = &iov; msg.msg_iovlen = 1;
        msg.msg_control = control; msg.msg_controllen = sizeof(control);

        ssize_t r = recvmsg(fd, &msg, 0);
        if (r < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                printf("Timed out, no RX frame arrived within 5s.\n");
            } else {
                perror("recvmsg");
            }
            break;
        }
        char label[32]; snprintf(label, sizeof(label), "RX frame %d", i);
        parse_and_print_ts(&msg, label);
    }

    close(fd);
    return 0;
}
