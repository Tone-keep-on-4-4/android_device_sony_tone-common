#include <sys/types.h>

int ion_open();
int ion_close(int fd);
int ion_alloc_fd(int fd, size_t len, size_t align, unsigned int heap_mask,
              unsigned int flags, int *handle_fd);
