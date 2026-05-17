#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <errno.h>
#include <string.h>
#include <sys/ioctl.h>

#define PACKET_START 0xAA
#define PACKET_END 0x55

typedef struct __attribute__((packed)) {
  uint8_t start;
  int32_t potentiometer;
  int32_t joystick_x;
  int32_t joystick_y;
  int32_t ultrasonic;
  uint8_t end;
} Packet;

Packet g_conf;

int read_packet(int sfd, Packet *out) {
    uint8_t byte;

    for (;;) {
        if (read(sfd, &byte, 1) != 1) {
            return 0;
        }

        if (byte == PACKET_START) {
            break;
        }
    }

    uint8_t *bytes = (uint8_t *)out;
    bytes[0] = PACKET_START;

    // read remaining bytes
    size_t received = 1;

    while (received < sizeof(Packet)) {
        ssize_t n = read(sfd, bytes + received, sizeof(Packet) - received);

        if (n <= 0) {
            return 0;
        }

        received += n;
    }

    if (out->end != PACKET_END) {
        return 0;
    }

    return 1;
}


int main() {
    int sfd = open("/dev/cu.usbmodem1101", O_RDWR | O_NOCTTY);
    if (sfd == -1) {
        printf("Error opening serial port: %s\n",strerror(errno));
        return -1;
    }

    struct termios options;
    if (tcgetattr(sfd,&options) < 0) {
        printf("Error getting attributes: %s\n",strerror(errno));
        return -1;
    }
    cfmakeraw(&options);
    cfsetspeed(&options, 9600);
    options.c_cflag &= ~CSTOPB;
    options.c_cflag |= CLOCAL;
    options.c_cflag |= CREAD;
    options.c_cflag |= CRTSCTS;
    options.c_cc[VTIME] = 0;
    options.c_cc[VMIN] = 1;
    if (tcsetattr(sfd,TCSANOW,&options) < 0) {
        printf("Error setting attributes: %s\n",strerror(errno));
        return -1;
    }

    tcflush(sfd,TCIFLUSH);

    char c;

    for (;;) {
        Packet packet;

        if (!read_packet(sfd, &packet)) {
            continue;
        }

        g_conf = packet;

        printf("pot=%d x=%d y=%d\n", g_conf.potentiometer, g_conf.joystick_x, g_conf.joystick_y);
    }

    close(sfd);

    return 0;
}
