#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int x;

static void
sighandler(int signum) {
    printf("sigtest: caught signal %d\n", signum);
    x = signum;
    sigreturn();
}

int main(int argc, char* argv[]) {
    x = 1;
    int pid;

    printf("sigtest: starting...\n");

    pid = fork();
    if (pid < 0) {
        printf("sigtest: fork failed\n");
        exit(1);
    }

    if (pid == 0) {
        if (signal(SIGINT, sighandler) == (sighandler_t)-1) {
            printf("sigtest: register handler failed\n");
            exit(1);
        }
        printf("sigtest (child %d): waiting for signal\n", getpid());
        for (;;) {
            pause(10);
        }
    } else {
        pause(50);
        printf("sigtest (parent %d): sending signal %d to child %d\n",
               getpid(), SIGINT, pid);
        kill(pid, SIGINT);
        pause(50);
        printf("sigtest (parent %d): sending SIGKILL to child %d\n",
               getpid(), pid);
        kill(pid, SIGKILL);
        wait(0);
        printf("sigtest (parent): child finished\n");
    }

    exit(0);
}
