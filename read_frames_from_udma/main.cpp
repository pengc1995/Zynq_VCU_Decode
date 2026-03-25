#include <fcntl.h>     /* open */
#include <unistd.h>
#include <sys/mman.h>
#include <cstdint>
#include <vector>
#include <cstring>
#include <fstream>

int main()
{
    int const fd_yuv = open( "/dev/udmabuf0", O_RDONLY | O_SYNC );
    int const fd_rgb = open( "/dev/udmabuf1", O_RDONLY | O_SYNC );

    if ( 0 > fd_yuv || 0> fd_rgb)
    {
        return 13;
    }
    
    unsigned int const map_size_yuv =  536870912u;
    unsigned int const map_size_rgb =  268435456u;

    unsigned int * addr_yuv = (unsigned int *)mmap(
                                        NULL,
                                        map_size_yuv,
                                        PROT_READ,
                                        MAP_SHARED,
                                        fd_yuv,
                                        0x0u );
    unsigned int * addr_rgb = (unsigned int *)mmap(
                                        NULL,
                                        map_size_rgb,
                                        PROT_READ,
                                        MAP_SHARED,
                                        fd_rgb,
                                        0x0u );

    close( fd_yuv );
    close( fd_rgb );

    if (   (NULL       == addr_yuv)
        || (MAP_FAILED == addr_yuv)
        || (NULL       == addr_rgb)
        || (MAP_FAILED == addr_rgb) )
    {
        return 14;
    }

    unsigned int constexpr frame_size_in_uint32_yuv{ 1920*1080*3u/2u };
    unsigned int constexpr frame_size_in_uint32_rgb{ 1920*1080*4u };
    
    std::vector<uint8_t> frame_yuv( frame_size_in_uint32_yuv, 0u );
    std::vector<uint8_t> frame_rgb( frame_size_in_uint32_rgb, 0u );

    memcpy(
        /* dest */ frame_yuv.data(),
        /* src  */ addr_yuv,
        /* size */ frame_size_in_uint32_yuv
    );
    std::ofstream of_yuv{ "frame_yuv.data", std::ios::binary };
    of_yuv.write( (char*)( frame_yuv.data() ), frame_size_in_uint32_yuv );
    munmap( (void*)addr_yuv, map_size_yuv );

    memcpy(
        /* dest */ frame_rgb.data(),
        /* src  */ addr_rgb,
        /* size */ frame_size_in_uint32_rgb
    );
    std::ofstream of_rgb{ "frame_rgb.data", std::ios::binary };
    of_rgb.write( (char*)( frame_rgb.data() ), frame_size_in_uint32_rgb );
    munmap( (void*)addr_rgb, map_size_rgb );

    return 0;
}
