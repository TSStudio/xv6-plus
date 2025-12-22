#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fcntl.h"

int main(void)
{
    int fd = open("a", O_CREATE | O_WRONLY);
    if (fd < 0)
    {
        printf("hardlinktest: open a failed\n");
        exit(1);
    }

    if (write(fd, "x", 1) != 1)
    {
        printf("hardlinktest: write x failed\n");
        exit(1);
    }

    if (link("a", "b") < 0)
    {
        printf("hardlinktest: link failed\n");
        exit(1);
    }
    if (unlink("a") < 0)
    {
        printf("hardlinktest: unlink a failed\n");
        exit(1);
    }

    if (write(fd, "y", 1) != 1)
    {
        printf("hardlinktest: write y failed\n");
        exit(1);
    }
    close(fd);

    fd = open("b", O_RDONLY);
    if (fd < 0)
    {
        printf("hardlinktest: open b failed\n");
        exit(1);
    }

    char buf[3] = {0};
    if (read(fd, buf, 2) != 2)
    {
        printf("hardlinktest: read failed\n");
        exit(1);
    }
    close(fd);

    if (strcmp(buf, "xy") != 0)
    {
        printf("hardlinktest: expect xy got %s\n", buf);
        exit(1);
    }

    if (unlink("b") < 0)
    {
        printf("hardlinktest: unlink b failed\n");
        exit(1);
    }

    printf("hardlinktest: ok\n");
    exit(0);
}
