#include "pty.h"

#include <fcntl.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

int pty_open(int *master_out, int *slave_out)
{
    int master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (master < 0)
        return -1;
    if (grantpt(master) != 0 || unlockpt(master) != 0) {
        close(master);
        return -1;
    }
    const char *name = ptsname(master);
    if (!name) {
        close(master);
        return -1;
    }
    int slave = open(name, O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (slave < 0) {
        close(master);
        return -1;
    }
    int fl = fcntl(master, F_GETFL, 0);
    if (fl < 0 || fcntl(master, F_SETFL, fl | O_NONBLOCK) < 0) {
        close(master);
        close(slave);
        return -1;
    }
    *master_out = master;
    *slave_out = slave;
    return 0;
}

int pty_set_size(int fd, uint16_t rows, uint16_t cols)
{
    struct winsize ws;
    ws.ws_row = rows;
    ws.ws_col = cols;
    ws.ws_xpixel = 0;
    ws.ws_ypixel = 0;
    return ioctl(fd, TIOCSWINSZ, &ws);
}