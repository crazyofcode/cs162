#include <stdio.h>
#include <sys/resource.h>

int main() {
    struct rlimit lim;
    getrlimit(RLIMIT_STACK, &lim);
    printf("stack size: %lx\n", lim.rlim_cur);
    getrlimit(RLIMIT_NPROC, &lim);
    printf("process limit: %lx\n", lim.rlim_max);
    getrlimit(RLIMIT_NOFILE, &lim);
    printf("max file descriptors: %lx\n", lim.rlim_cur);
    return 0;
}
