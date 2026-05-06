#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <errno.h>
#include <string.h>
#include <sys/ioctl.h>

struct dataDef {
    int pot_val;
} conf;


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

    for(;;) {
        char *c_bytes = (char*) &conf;

        for (int i = 0; i < sizeof(conf); i++) {
            if (read(sfd, &c_bytes[i],1) == -1) break;
        }

        printf("%d\n", conf.pot_val);
    }

    close(sfd);

    return 0;
}
