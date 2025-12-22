#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fcntl.h"

int main(void)
{
    char name[] = "this_is_a_very_very_long_filename_for_xv6_test_0123456789";
    int fd = open(name, O_CREATE | O_WRONLY);
    if (fd < 0)
    {
        printf("longnametest: open failed\n");
        exit(1);
    }
    if (write(fd, "ok", 2) != 2)
    {
        printf("longnametest: write failed\n");
        close(fd);
        exit(1);
    }
    close(fd);

    fd = open(name, O_RDONLY);
    if (fd < 0)
    {
        printf("longnametest: reopen failed\n");
        exit(1);
    }
    char buf[3] = {0};
    if (read(fd, buf, 2) != 2)
    {
        printf("longnametest: read failed\n");
        close(fd);
        exit(1);
    }
    close(fd);

    if (strcmp(buf, "ok") != 0)
    {
        printf("longnametest: content mismatch %s\n", buf);
        exit(1);
    }

    if (unlink(name) < 0)
    {
        printf("longnametest: unlink failed\n");
        exit(1);
    }

    printf("longnametest: ok\n");
    exit(0);
}
