
#include <iostream>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>

#include "DmaHeap.h"

bool DmaHeap::open(std::string device_name) {

    m_heap_device_fd = ::open(device_name.c_str(), O_RDWR, 0);
    if (m_heap_device_fd < 0) {
        std::cout << "Dma heap " << device_name << " can not be opened" << std::endl;
        return false;
    }

    return true;
}

int DmaHeap::allocate(size_t size) {
    struct dma_heap_allocation_data alloc = { 0 };

    alloc.len = size;
    alloc.fd_flags = O_CLOEXEC | O_RDWR;

    if(ioctl(m_heap_device_fd, DMA_HEAP_IOCTL_ALLOC, &alloc) < 0)
        return -1;

    /// @todo: check if dma heap name assignment is obligatory.
    //if(name)
    //    ioctl(alloc.fd, DMA_BUF_SET_NAME, name);

    return alloc.fd;
}

void DmaHeap::start_reading_cpu(int buf_fd) {
    struct dma_buf_sync sync = { 0 };
    sync.flags = (DMA_BUF_SYNC_START) | DMA_BUF_SYNC_READ;
    do
    {
        if(ioctl(buf_fd, DMA_BUF_IOCTL_SYNC, &sync) == 0)
            break;
    } while((errno == EINTR) || (errno == EAGAIN));
}

void DmaHeap::stop_reading_cpu(int buf_fd) {
    struct dma_buf_sync sync = { 0 };
    sync.flags = (DMA_BUF_SYNC_END) | DMA_BUF_SYNC_READ;
    do
    {
        if(ioctl(buf_fd, DMA_BUF_IOCTL_SYNC, &sync) == 0)
            break;
    } while((errno == EINTR) || (errno == EAGAIN));
}

DmaHeap::~DmaHeap() {
    ::close(m_heap_device_fd);
}