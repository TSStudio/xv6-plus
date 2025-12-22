#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fs.h"
#include "kernel/fcntl.h"

char *
fmtname(char *path)
{
  static char buf[DIRSIZ + 1];
  char *p;

  // Find first character after last slash.
  for (p = path + strlen(path); p >= path && *p != '/'; p--)
    ;
  p++;

  // Return blank-padded name.
  if (strlen(p) >= DIRSIZ)
    return p;
  memmove(buf, p, strlen(p));
  memset(buf + strlen(p), ' ', DIRSIZ - strlen(p));
  return buf;
}

void ls(char *path, int longfmt)
{
  char buf[512], *p;
  int fd;
  struct dirent de;
  struct stat st;
  char longname[MAXFNAME];
  int longlen = 0;

  if ((fd = open(path, O_RDONLY)) < 0)
  {
    fprintf(2, "ls: cannot open %s\n", path);
    return;
  }

  if (fstat(fd, &st) < 0)
  {
    fprintf(2, "ls: cannot stat %s\n", path);
    close(fd);
    return;
  }

  switch (st.type)
  {
  case T_DEVICE:
  case T_FILE:
    if (longfmt)
      printf("%s %d %d %d %d\n", fmtname(path), st.type, st.nlink, st.ino, (int)st.size);
    else
      printf("%s %d %d %d\n", fmtname(path), st.type, st.ino, (int)st.size);
    break;

  case T_DIR:
    if (strlen(path) + 1 + MAXFNAME + 1 > sizeof buf)
    {
      printf("ls: path too long\n");
      break;
    }
    strcpy(buf, path);
    p = buf + strlen(buf);
    *p++ = '/';
    while (read(fd, &de, sizeof(de)) == sizeof(de))
    {
      if (de.inum == 0)
      {
        longlen = 0;
        longname[0] = 0;
        continue;
      }

      if (de.inum == DIRENT_CONT)
      {
        for (int i = 0; i < DIRSIZ && longlen < MAXFNAME - 1; i++)
        {
          longname[longlen++] = de.name[i];
          if (de.name[i] == 0)
            break;
        }
        if (longlen >= MAXFNAME)
          longname[MAXFNAME - 1] = 0;
        continue;
      }

      char *ename;
      if (longlen > 0)
      {
        longname[longlen < MAXFNAME ? longlen : MAXFNAME - 1] = 0;
        ename = longname;
        memmove(p, ename, strlen(ename) + 1);
      }
      else
      {
        memmove(p, de.name, DIRSIZ);
        p[DIRSIZ] = 0;
        ename = p;
      }
      if (stat(buf, &st) < 0)
      {
        printf("ls: cannot stat %s\n", buf);
        continue;
      }
      if (longfmt)
        printf("%s %d %d %d %d\n", ename, st.type, st.nlink, st.ino, (int)st.size);
      else
        printf("%s %d %d %d\n", fmtname(buf), st.type, st.ino, (int)st.size);
      longlen = 0;
      longname[0] = 0;
    }
    break;
  }
  close(fd);
}

int main(int argc, char *argv[])
{
  int i;
  int longfmt = 0;

  if (argc > 1 && strcmp(argv[1], "-l") == 0)
  {
    longfmt = 1;
    argv++;
    argc--;
  }

  if (argc < 2)
  {
    ls(".", longfmt);
    exit(0);
  }
  for (i = 1; i < argc; i++)
    ls(argv[i], longfmt);
  exit(0);
}
