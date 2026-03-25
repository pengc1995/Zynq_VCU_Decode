#pragma once

#include "Frame_Acquisition_Controller.h"
#include "vcu/VCU_Decode.h"
#include <Video/Display/Placeholder_Frame_Painter.h>
#include <Utils/Playback_Layout.h>
#include <Utils/Circular_Frame_Buffer_Controller.h>
#ifndef USE_HAL_MOCK
#include <Utils/PS_Frame_Buffer.h>
#else
#include <Utils/Main_Memory_Frame_Buffer.h>
#endif
#include <PL/PL_YUV2RGB_fwd.h>
#include <filesystem>

namespace Playback {
    class Playback_Controller;
}

using namespace Kron::Infinite;
#ifndef USE_HAL_MOCK
using namespace Kron_VCU_Decode;
#endif

class VCU_Playback_Controller : public Frame_Acquisition_Controller
{
private:
    Playback::Playback_Controller    & playback_controller;
    Circular_Frame_Buffer_Controller & rgb_buffer_controller;
    Kron::Infinite::PL_YUV2RGB       & pl_yuv2rgb;

    /// YUV buffer controller — owned exclusively by the prefetch thread
    /// via m_get_yuv_slot lambda. get_frame() never calls it directly.
    Circular_Frame_Buffer_Controller   yuv_buffer_controller;

#ifndef USE_HAL_MOCK
    PS_Frame_Buffer                    rgb_frame_buffer;
    PS_Frame_Buffer                    yuv_frame_buffer;
#else
    Main_Memory_Frame_Buffer           rgb_frame_buffer;
    Main_Memory_Frame_buffer           yuv_frame_buffer;
#endif

    std::filesystem::path              vcu_path;
    Playback_Layout                    vcu_format;
    VCU_Decode                         vcu_decode;

    /// next_address_to_fill is removed — the prefetch thread owns YUV slots entirely.
    /// next_address_to_play is still managed by get_frame() for the RGB buffer.
    last_rgb_frame_addr_t              next_address_to_play;
    last_rgb_frame_addr_t              last_loaded_address;
    long int                           last_loaded_index;

    last_rgb_frame_addr_t              placeholder_frame_paddr;
    Placeholder_Frame_Painter          placeholder_frame;
    bool                               is_error;

public:
    VCU_Playback_Controller(
            Playback::Playback_Controller      & playback_controller,
            Circular_Frame_Buffer_Controller   & rgb_buffer_controller,
            Kron::Infinite::PL_YUV2RGB         & pl_yuv2rgb );

public:
    void init(
            std::filesystem::path const & path );

public:
    void deinit();

public:
    Playback_Layout get_vcu_format() const noexcept;

private:
    void read_vcu_layout_from_filesystem();

private:
    void prepare_placeholder_frame(
            std::string const & error_message );

private:
    void configure_error_state(
            std::string const & error_message );

private:
    [[nodiscard]]
    last_rgb_frame_addr_t
    get_frame(
            std::chrono::time_point<std::chrono::steady_clock> time_point ) noexcept override;
};
