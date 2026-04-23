#include "syshead.h"
#include "utils.h"
#include "basic.h"

static int tun_fd;
static char* dev;

char *tapaddr = "10.0.0.5";
char *taproute = "10.0.0.0/24";

static int set_if_route(char *dev, char *cidr)
{
    return run_cmd("ip route add dev %s %s", dev, cidr);
}

static int set_if_address(char *dev, char *cidr)
{
    return run_cmd("ip address add dev %s local %s", dev, cidr);
}

static int set_if_up(char *dev)
{
    return run_cmd("ip link set dev %s up", dev);
}

/*
 * Taken from Kernel Documentation/networking/tuntap.txt
 */
static int tun_alloc(char *dev)
{
    struct ifreq ifr;
    int fd, err;

    /* Use the standard Linux TUN/TAP clone device path first. */
    fd = open("/dev/net/tun", O_RDWR);

    /* Fallback for legacy/custom setups that still expose /dev/net/tap. */
    if (fd < 0) {
        fd = open("/dev/net/tap", O_RDWR);
    }

    if (fd < 0) {
        perror("Cannot open TUN/TAP dev\n"
               "Expected /dev/net/tun (or legacy /dev/net/tap).\n"
               "Try: sudo modprobe tun\n"
               "Then, if needed:\n"
               "  sudo mkdir -p /dev/net\n"
               "  sudo mknod /dev/net/tun c 10 200");
        exit(1);
    }

    CLEAR(ifr);

    /* Flags: IFF_TUN   - TUN device (no Ethernet headers)
     *        IFF_TAP   - TAP device
     *
     *        IFF_NO_PI - Do not provide packet information
     */
    ifr.ifr_flags = IFF_TAP | IFF_NO_PI;
    if( *dev ) {
        strncpy(ifr.ifr_name, dev, IFNAMSIZ);
    }

    if( (err = ioctl(fd, TUNSETIFF, (void *) &ifr)) < 0 ){
        perror("ERR: Could not ioctl tun");
        close(fd);
        return err;
    }

    strcpy(dev, ifr.ifr_name);
    return fd;
}

int tun_read(char *buf, int len)
{
    return read(tun_fd, buf, len);
}

int tun_write(char *buf, int len)
{
    return write(tun_fd, buf, len);
}

void tun_init()
{
    dev = calloc(10, 1);

    /* Tests and host setup expect the stack to use tap0 explicitly. */
    strncpy(dev, "tap0", 9);
    tun_fd = tun_alloc(dev);

    if (set_if_up(dev) != 0) {
        print_err("ERROR when setting up if\n");
    }

    if (set_if_route(dev, taproute) != 0) {
        print_err("ERROR when setting route for if\n");
    }

    if (set_if_address(dev, tapaddr) != 0) {
        print_err("ERROR when setting addr for if\n");
    }
}

void free_tun()
{
    free(dev);
}
