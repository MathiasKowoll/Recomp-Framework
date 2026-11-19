// The plain names of the functions the runtime has under their $UNIX2003 names.
//
// An i386 program built against the 10.4 SDK imports the conforming variants as _open$UNIX2003 and the like; one
// built later, or with other settings (tcmalloc inside a 10.9-era game), imports plain _open for the same thing.
// What separates the two on a real Mac is error reporting in corners no game depends on, so each plain name is
// the same shim.
#include "rt.h"
#define PLAIN(name) void I_##name##_UNIX2003(CPU *c); void I_##name(CPU *c) { I_##name##_UNIX2003(c); }
PLAIN(_accept)
PLAIN(_bind)
PLAIN(_chmod)
PLAIN(_clock)
PLAIN(_close)
PLAIN(_closedir)
PLAIN(_connect)
PLAIN(_fcntl)
PLAIN(_fputs)
PLAIN(_fwrite)
PLAIN(_getpeername)
PLAIN(_getsockname)
PLAIN(_listen)
PLAIN(_mktime)
PLAIN(_mmap)
PLAIN(_msync)
PLAIN(_nanosleep)
PLAIN(_open)
PLAIN(_opendir)
PLAIN(_pthread_cond_timedwait)
PLAIN(_pthread_cond_wait)
PLAIN(_pthread_join)
PLAIN(_read)
PLAIN(_recv)
PLAIN(_recvfrom)
PLAIN(_select)
PLAIN(_sem_wait)
PLAIN(_send)
PLAIN(_sendto)
PLAIN(_strerror)
PLAIN(_strftime)
PLAIN(_strtod)
PLAIN(_system)
PLAIN(_write)
