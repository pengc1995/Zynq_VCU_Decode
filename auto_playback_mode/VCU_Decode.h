#pragma once
#ifndef USE_HAL_MOCK

#include <Video/gst/GSTElement.h>
#include <Video/Enums/Save_File_Format.h>
#include <Utils/Circular_Frame_Buffer_Controller.h>
#include <Utils/UDMABuf.h>
#include <filesystem>
#include <condition_variable>
#include <mutex>
#include <atomic>
#include <thread>

struct _GstAppSink;
typedef struct _GstAppSink GstAppSink;

struct VideoMetaData {
    int              width        = 0;
    int              height       = 0;
    int              fps_n        = 0;
    int              fps_d        = 0;
    int              total_frames = 0;
    Save_File_Format codec;
    bool             has_b_frames = false;
    unsigned int     gop_size     = 1;
};

namespace Kron_VCU_Decode {

class VCU_Decode {
private:
    // -------------------------------------------------------------------------
    // GStreamer pipeline elements
    // -------------------------------------------------------------------------
    GSTElement              decoding_pipeline;
    GSTElement              file_source;
    GSTElement              demuxer;
    GSTElement              parser;
    GSTElement              caps_filter;
    GSTElement              decoder;
    GSTElement              raw_caps_filter;
    GSTElement              app_sink;

    GMainLoop*              m_loop       = nullptr;
    std::thread             m_loop_thread;

    // -------------------------------------------------------------------------
    // decode_frame() — seek/position mode state
    // -------------------------------------------------------------------------
    uint8_t*                virt_addr         = nullptr;
    GstClockTime            target_pts        = 0;
    std::atomic<bool>       got_target        = false;
    std::atomic<bool>       gop_multiple_skip = false;
    unsigned int            last_frame        = 0;
    std::mutex              m_mutex;
    std::condition_variable m_cv;

    // -------------------------------------------------------------------------
    // continuous playback mode state
    // -------------------------------------------------------------------------
    std::atomic<bool>       is_continuous_playback  = false;

    ///< Front buffer — fully written frame ready for get_frame() to consume
    std::atomic<uint64_t>   front_address           = 0;

    ///< Back buffer — being written by on_new_sample
    uint64_t                back_address            = 0;

    ///< References to YUV buffer — set when continuous playback starts
    UDMABuf*                         yuv_frame_buffer     = nullptr;
    Circular_Frame_Buffer_Controller* yuv_buffer_controller = nullptr;

    // -------------------------------------------------------------------------
    // Video metadata
    // -------------------------------------------------------------------------
    VideoMetaData           meta;

private:
    void create_pipeline        ( Save_File_Format const format );
    void teardown_pipeline      ();

    void configure_file_source  ( std::filesystem::path const & file );
    void configure_demuxer      ();
    void configure_parser       ();
    void configure_caps_filter  ( Save_File_Format const format );
    void configure_decoder      ();
    void configure_raw_caps_filter();
    void configure_app_sink     ();

    static GstFlowReturn on_new_sample  ( GstAppSink* sink, gpointer user_data );
    static void          on_pad_added   ( GstElement* src, GstPad* pad, gpointer user_data );
    static gboolean      on_bus_callback( GstBus* bus, GstMessage* msg, gpointer user_data );

public:
    VideoMetaData get_video_info( std::filesystem::path const & file );

    void decoder_init  ( std::filesystem::path const & file );
    void decoder_deinit();

    ///
    /// Seek/position mode — decodes exact frame, blocking.
    /// Used for manual position changes and paused state.
    ///
    void decode_frame(
            unsigned int  const frame_index,
            uint8_t*      const destination );

    ///
    /// Continuous playback mode — lets GStreamer run freely at the given rate.
    /// front_address is initialised with last_yuv_address from the previous
    /// decode_frame() call so get_latest_yuv_address() is always valid.
    ///
    void start_continuous_playback(
            double    const rate,
            uint64_t  const initial_yuv_address,
            UDMABuf                          & yuv_frame_buffer_,
            Circular_Frame_Buffer_Controller & yuv_buffer_controller_ );

    void stop_continuous_playback();

    ///
    /// Changes GStreamer playback rate while pipeline is PLAYING.
    /// Only called when rate actually changes.
    ///
    void set_continuous_playback_rate( double rate );

    ///
    /// Returns the latest fully decoded YUV frame address.
    /// Safe to call from get_frame() concurrently with on_new_sample().
    ///
    uint64_t get_latest_yuv_address() const noexcept;
};

}

#endif
