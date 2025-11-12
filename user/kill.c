#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int main(int argc, char** argv) {
    int i;
    int signum = SIGKILL;
    int start = 1;

    if (argc < 2) {
        fprintf(2, "usage: kill [-s signum] pid...\n");
        exit(1);
    }

    if (argc > 2 && strcmp(argv[1], "-s") == 0) {
        signum = atoi(argv[2]);
        start = 3;
        if (start >= argc) {
            fprintf(2, "usage: kill [-s signum] pid...\n");
            exit(1);
        }
    }

    for (i = start; i < argc; i++)
        kill(atoi(argv[i]), signum);
    exit(0);
}
