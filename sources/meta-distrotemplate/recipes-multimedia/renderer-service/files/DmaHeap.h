#pragma once
#include <string>

class DmaHeap {

public:

    bool open(std::string device_name);

    int allocate(size_t size);

    static void start_reading_cpu(int buf_fd);
    static void stop_reading_cpu(int buf_fd);

    virtual ~DmaHeap();

private:
    int m_heap_device_fd{-1};
};