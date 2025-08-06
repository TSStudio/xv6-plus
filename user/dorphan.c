#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "user/user.h"

// Stress xv6 logging system by having several processes writing
// concurrently to their own file (e.g., logstress f1 f2 f3 f4)

#define BUFSZ 500

char buf[BUFSZ];

int
main(int argc, char **argv)
{
  char *s = argv[0];

  if(mkdir("dd") != 0){
    printf("%s: mkdir dd failed\n", s);
    exit(1);
  }

  if(chdir("dd") != 0){
    printf("%s: chdir dd failed\n", s);
    exit(1);
  }

  if (unlink("../dd") < 0) {
    printf("%s: unlink failed\n", s);
    exit(1);
  }
  printf("wait for kill and reclaim\n");
  // sit around until killed
  for(;;) sleep(1000);
}
