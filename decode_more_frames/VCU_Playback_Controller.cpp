#include "VCU_Playback_Controller.h"
#include <Playback_Controller/Playback_Controller.h>
#include <Utils/Circular_Frame_Buffer_Controller.h>
#include <Utils/DDR_Info.h>
#include <Utils/in_range.h>
#include <Utils/is_multiple_of_.h>
#include <Sensors/Sensor.h>
#include <Utils/Kron_Exception.h>
#include <PL/PL_YUV2RGB.h>
#include <Video/should_disable_caching.h>
#include <Video/max_size_of_one_frame_in_bytes.h>
#include <filesystem>
#include <spdlog/spdlog.h>
#include <fmt/format.h>

namespace {

constexpr Playback_Layout DEFAULT_PLAYBACK_LAYOUT {
    Sensor::get_resolutionH_range()[1], Sensor::get_resolutionV_range()[1], 8, 1
};

}

VCU_Playback_Controller::VCU_Playback_Controller(
        Playback::Playback_Controller    & playback_controller_,
        Circular_Frame_Buffer_Controller & rgb_buffer_controller_,
        Kron::Infinite::PL_YUV2RGB       & pl_yuv2rgb_
)
    :   playback_controller   (playback_controller_)
    ,   rgb_buffer_controller (rgb_buffer_controller_)
    ,   pl_yuv2rgb            (pl_yuv2rgb_)
    ,   yuv_buffer_controller (
                                DDR_Info::get_udmabuf_saving_start_address(),
                                max_size_of_one_yuv_frame_in_bytes,
                                DDR_Info::get_udmabuf_saving_size() )
    ,   rgb_frame_buffer      ()
    ,   yuv_frame_buffer      ()
{}

Frame_Acquisition_Controller::last_rgb_frame_addr_t VCU_Playback_Controller::get_frame(
        std::chrono::time_point<std::chrono::steady_clock> time_point ) noexcept
{
    if ( is_error ) {
        return placeholder_frame_paddr;
    }

    auto const frame_index = playback_controller.get_next_frame_index( time_point );

    // Same frame as last time — return cached RGB address directly
    if ( frame_index == last_loaded_index ) {
        return last_loaded_address;
    }

    std::cout << "frame_index: " << frame_index << "\n";

    // Get the YUV address from cache (blocking on cache miss / seek)
    auto const start1 = std::chrono::high_resolution_clock::now();
    uint64_t const yuv_address = vcu_decode.decode_frame( frame_index );
    auto const end1 = std::chrono::high_resolution_clock::now();
    auto const duration1 = std::chrono::duration_cast<std::chrono::microseconds>( end1 - start1 );
    std::cout << "decode_frame took:  " << duration1.count() << " us\n";

    // Convert YUV -> RGB using FPGA block
    uint64_t const rgb_address = rgb_buffer_controller.get_the_next_address();

    auto const start2 = std::chrono::high_resolution_clock::now();
    pl_yuv2rgb.convert_to_rgb( yuv_address, rgb_address );
    auto const end2 = std::chrono::high_resolution_clock::now();
    auto const duration2 = std::chrono::duration_cast<std::chrono::microseconds>( end2 - start2 );
    std::cout << "convert_to_rgb took: " << duration2.count() << " us\n\n";

    last_loaded_address = rgb_address;
    last_loaded_index   = frame_index;

    return last_loaded_address;
}

void VCU_Playback_Controller::init(
        std::filesystem::path const & path )
{
    try {
        rgb_frame_buffer.init(
                DDR_Info::get_udmabuf_rgb_size(),
                DDR_Info::get_udmabuf_rgb_start_address(),
                DDR_Info::get_udmabuf_rgb_device_index(),
                false,
                true );

        last_loaded_address = 0;
        last_loaded_index   = -1;

        vcu_path.assign( path );
        if ( not vcu_path.empty() ) {
            read_vcu_layout_from_filesystem();
            playback_controller.configure_media_size( vcu_format.max_number_of_frames );
            pl_yuv2rgb.configure_yuv2rgb( vcu_format );

            yuv_frame_buffer.init(
                    DDR_Info::get_udmabuf_saving_size(),
                    DDR_Info::get_udmabuf_saving_start_address(),
                    DDR_Info::get_udmabuf_saving_device_index(),
                    true,
                    true );

            // Pass yuv buffer ownership to vcu_decode for prefetch management
            vcu_decode.decoder_init( vcu_path, yuv_frame_buffer, yuv_buffer_controller );

            is_error = false;
        }
        else {
            throw Kron::Kron_Exception(
                MSG2USR fmt::format( "Cannot play {}: No media file found.", vcu_path.string() ) );
        }
    }
    catch ( Kron::Kron_Exception &err ) {
        configure_error_state( err.what() );
    }
}

void VCU_Playback_Controller::deinit()
{
    placeholder_frame.deinit();
    rgb_frame_buffer.deinit();
    yuv_frame_buffer.deinit();
    vcu_decode.decoder_deinit();
}

Playback_Layout VCU_Playback_Controller::get_vcu_format() const noexcept
{
    return vcu_format;
}

void VCU_Playback_Controller::read_vcu_layout_from_filesystem()
{
    VideoMetaData meta = vcu_decode.get_video_info( vcu_path );

    if ( not in_range( meta.width,  Sensor::get_resolutionH_range() )
    or   not in_range( meta.height, Sensor::get_resolutionV_range() )
    or   not is_multiple_of_<8>( static_cast<unsigned int>( meta.width  ) )
    or   not is_multiple_of_<8>( static_cast<unsigned int>( meta.height ) ) ) {
        throw Kron::Kron_Exception(
            MSG2USR fmt::format( "Cannot play {}: Invalid format.", vcu_path.string() ) );
    }

    vcu_format = Playback_Layout {
        .width                = meta.width,
        .height               = meta.height,
        .bit_mode             = 8,
        .max_number_of_frames = meta.total_frames
    };
}

void VCU_Playback_Controller::prepare_placeholder_frame(
        std::string const & error_message )
{
    placeholder_frame_paddr = rgb_buffer_controller.get_the_next_address();
    auto placeholder_vaddr = const_cast<unsigned int*>(
            rgb_frame_buffer.get_virtual_address_from_physical_address( placeholder_frame_paddr ) );

    placeholder_frame.reset( reinterpret_cast<uint8_t*>( placeholder_vaddr ),
                             vcu_format.width,
                             vcu_format.height );
    placeholder_frame.set_text( error_message );
}

void VCU_Playback_Controller::configure_error_state(
        std::string const & error_message )
{
    vcu_format = DEFAULT_PLAYBACK_LAYOUT;
    playback_controller.configure_media_size( vcu_format.max_number_of_frames );
    prepare_placeholder_frame( error_message );
    is_error = true;
}
