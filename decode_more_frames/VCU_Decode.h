#pragma once

#include <filesystem>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <array>
#include <cstdint>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#pragma GCC diagnostic pop
#include <Video/Save_File_Format.h>
#include <Utils/Circular_Frame_Buffer_Controller.h>
#include <Utils/UDMABuf.h>

namespace Kron_VCU_Decode {

struct VideoMetaData {
    unsigned int     width         = 0;
    unsigned int     height        = 0;
    unsigned int     fps_n         = 0;
    unsigned int     fps_d         = 0;
    int              total_frames  = 0;
    Save_File_Format codec         = Save_File_Format::h264;
};

}

class VCU_Decode
{
public:

    static constexpr int PREFETCH_CACHE_SIZE = 8;

    struct CachedFrame {
        unsigned int frame_index      = 0;
        uint64_t     yuv_slot_address = 0;
        bool         is_valid         = false;
    };

    VCU_Decode() = default;
   ~VCU_Decode() = default;

    VCU_Decode( VCU_Decode const & ) = delete;
    VCU_Decode& operator=( VCU_Decode const & ) = delete;

    ///
    /// Initialise the GStreamer pipeline.
    /// yuv_frame_buffer and yuv_buffer_controller are owned externally
    /// but used by the prefetch thread for slot management.
    ///
    void decoder_init(
            std::filesystem::path          const & file,
            UDMABuf                              & yuv_frame_buffer_,
            Circular_Frame_Buffer_Controller     & yuv_buffer_controller_ );

    void decoder_deinit();

    ///
    /// Returns the physical YUV address for the requested frame.
    /// Cache hit  -> returns immediately.
    /// Cache miss -> seeks, decodes blocking, restarts prefetch, then returns.
    ///
    uint64_t decode_frame( unsigned int frame_index );

    Kron_VCU_Decode::VideoMetaData get_video_info(
            std::filesystem::path const & file );

private:

    // -------------------------------------------------------------------------
    // GStreamer pipeline elements
    // -------------------------------------------------------------------------
    using GstElementPtr  = std::unique_ptr<GstElement,  decltype(&gst_object_unref)>;
    using GstPipelinePtr = std::unique_ptr<GstElement,  decltype(&gst_object_unref)>;

    GstPipelinePtr decoding_pipeline { nullptr, gst_object_unref };
    GstElementPtr  file_source       { nullptr, gst_object_unref };
    GstElementPtr  demuxer           { nullptr, gst_object_unref };
    GstElementPtr  parser            { nullptr, gst_object_unref };
    GstElementPtr  caps_filter       { nullptr, gst_object_unref };
    GstElementPtr  decoder           { nullptr, gst_object_unref };
    GstElementPtr  raw_caps_filter   { nullptr, gst_object_unref };
    GstElementPtr  app_sink          { nullptr, gst_object_unref };

    GMainLoop*  m_loop        = nullptr;
    std::thread m_loop_thread;

    // -------------------------------------------------------------------------
    // GStreamer decode synchronisation (used by decode_into())
    // -------------------------------------------------------------------------
    std::mutex              m_mutex;
    std::condition_variable m_cv;
    bool                    got_target       = false;
    bool                    gop_multiple_skip = false;

    uint8_t*     virt_addr    = nullptr;
    unsigned int target_frame = 0;
    GstClockTime target_pts   = 0;
    unsigned int last_gst_frame = std::numeric_limits<unsigned int>::max();

    // -------------------------------------------------------------------------
    // Prefetch cache
    // -------------------------------------------------------------------------
    std::array<CachedFrame, PREFETCH_CACHE_SIZE> frame_cache {};

    std::mutex              cache_mutex;
    std::condition_variable cache_consumed_cv;

    std::thread          prefetch_thread;
    std::atomic<bool>    prefetch_stop_flag { true };
    unsigned int         prefetch_next_frame = 0;
    int                  next_cache_slot     = 0;
    unsigned int         last_displayed_frame = std::numeric_limits<unsigned int>::max();

    // -------------------------------------------------------------------------
    // YUV buffer — owned externally, referenced here for prefetch thread use
    // -------------------------------------------------------------------------
    UDMABuf*                        yuv_frame_buffer    = nullptr;
    Circular_Frame_Buffer_Controller* yuv_buffer_controller = nullptr;

    // -------------------------------------------------------------------------
    // Video metadata
    // -------------------------------------------------------------------------
    Kron_VCU_Decode::VideoMetaData meta;

    // -------------------------------------------------------------------------
    // Private methods
    // -------------------------------------------------------------------------
    void     decode_into             ( unsigned int frame_index, uint8_t* destination );
    uint64_t decode_frame_blocking   ( unsigned int frame_index );

    void     start_prefetch_thread   ( unsigned int start_frame );
    void     stop_prefetch_thread    ();
    void     flush_cache             ();
    void     prefetch_loop           ();

    void     create_pipeline         ( Save_File_Format format );
    void     teardown_pipeline       ();
    void     configure_file_source   ( std::filesystem::path const & file );
    void     configure_demuxer       ();
    void     configure_parser        ();
    void     configure_caps_filter   ( Save_File_Format format );
    void     configure_decoder       ();
    void     configure_raw_caps_filter();
    void     configure_app_sink      ();

    static GstFlowReturn on_new_sample  ( GstAppSink* sink, gpointer user_data );
    static void          on_pad_added   ( GstElement* src, GstPad* pad, gpointer user_data );
    static gboolean      on_bus_callback( GstBus* bus, GstMessage* msg, gpointer user_data );
};
