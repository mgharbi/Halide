#include "runtime_internal.h"

extern "C" {

// NaCl can run with either glibc or newlib; under newlib, stderr and friends
// are offsets into a thread-context pointer, not a single global that we
// can declare extern. To work reasonably in both environments, we use
// vsnprintf() to write to a local buffer, then write() to fd=2.
extern size_t write(int fd, const void *buf, size_t count);
const int STDERR_FILENO = 2;
}

namespace Halide { namespace Runtime { namespace Internal {

WEAK void halide_print_impl(void *user_context, const char *str) {
    size_t len = 0;
    while (str[len] != 0) ++len;
    write(STDERR_FILENO, str, len);
}

}}}


