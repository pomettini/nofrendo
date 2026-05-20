#include <sys/stat.h>
#include <errno.h>

/* Newlib bare-metal syscall stubs. All file I/O goes through pd->file->*, not stdio. */

int _close(int fd)                              { return -1; }
int _fstat(int fd, struct stat *st)             { st->st_mode = S_IFCHR; return 0; }
int _isatty(int fd)                             { return 1; }
int _lseek(int fd, int ptr, int dir)            { return 0; }
int _open(const char *name, int flags, int mode){ errno = ENOENT; return -1; }
int _read(int fd, char *ptr, int len)           { return 0; }
int _write(int fd, char *ptr, int len)          { return len; }
