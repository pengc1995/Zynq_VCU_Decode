#pragma once

#include <filesystem>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <atomic>
#include <chrono>
#include <cstdint>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#pragma GCC diagnostic pop

#include <Utils/Circular_Frame_Buffer_Controller.h>

namespace Kron_VCU_Decode {
    enum class Save_File_Format { h264, h265 };
}

struct VideoMetaData
{
    unsigned int           width         { 0 };
    unsigned int           height        { 0 };
    unsigned int           fps_n         { 0 };
    unsigned int           fps_d         { 0 };
    int                    total_frames  { 0 };
    Kron_VCU_Decode::Save_File_Format codec {};
};

// Wraps a GstElement with automatic unref
struct GstElementDeleter {
    void operator()( GstElement* e ) const { if (e) gst_object_unref(e); }
};
using GstElementPtr = std::unique_ptr<GstElement, GstElementDeleter>;

GstElementPtr make_gst_element( const char* factory, const char* name );
GstElementPtr make_gst_pipeline( const char* name );


class VCU_Decode
{
public:

    // -----------------------------------------------------------------------
    // Public API
    // -----------------------------------------------------------------------

    VideoMetaData get_video_info( std::filesystem::path const& file );

    void decoder_init( std::filesystem::path const& file );
    void decoder_deinit();

    /// Request a specific frame. Blocks until the frame has been copied into
    /// destination. For sequential access this should return almost instantly
    /// because the prefetch thread already decoded it. For seeks it will take
    /// longer while the pipeline repositions.
    void decode_frame( unsigned int frame_index, uint8_t* destination );

private:

    // -----------------------------------------------------------------------
    // Decoded-frame queue entry
    // -----------------------------------------------------------------------

    struct Decoded_Frame
    {
        unsigned int  frame_index { 0 };
        uint8_t*      yuv_address { nullptr };  ///< virtual address in udmabuf
    };

    // -----------------------------------------------------------------------
    // Pipeline helpers
    // -----------------------------------------------------------------------

    void create_pipeline        ( Kron_VCU_Decode::Save_File_Format format );
    void teardown_pipeline      ();
    void configure_file_source  ( std::filesystem::path const& file );
    void configure_demuxer      ();
    void configure_parser       ();
    void configure_caps_filter  ( Kron_VCU_Decode::Save_File_Format format );
    void configure_decoder      ();
    void configure_raw_caps_filter();
    void configure_app_sink     ();

    // -----------------------------------------------------------------------
    // Prefetch thread
    // -----------------------------------------------------------------------

    void prefetch_loop();

    /// Called by the prefetch loop to decode the next sequential frame.
    /// Blocks until on_new_sample copies one frame into the slot.
    /// Returns false if EOS was reached.
    bool decode_next_frame_into_slot( unsigned int frame_index, uint8_t* slot_address );

    /// Issue a seek to bring the pipeline close to target_frame.
    void seek_to( unsigned int frame_index );

    // -----------------------------------------------------------------------
    // GStreamer callbacks (static)
    // -----------------------------------------------------------------------

    static GstFlowReturn on_new_sample ( GstAppSink* sink, gpointer user_data );
    static void          on_pad_added  ( GstElement* src,  GstPad* pad, gpointer user_data );
    static gboolean      on_bus_callback( GstBus* bus, GstMessage* msg, gpointer user_data );

    // -----------------------------------------------------------------------
    // GStreamer pipeline elements
    // -----------------------------------------------------------------------

    GstElementPtr decoding_pipeline;
    GstElementPtr file_source;
    GstElementPtr demuxer;
    GstElementPtr parser;
    GstElementPtr caps_filter;
    GstElementPtr decoder;
    GstElementPtr raw_caps_filter;
    GstElementPtr app_sink;

    GMainLoop*    m_loop        { nullptr };
    std::thread   m_loop_thread;

    // -----------------------------------------------------------------------
    // Prefetch thread state
    // -----------------------------------------------------------------------

    std::thread   m_prefetch_thread;
    std::atomic<bool> m_prefetch_running { false };

    /// Shared between prefetch thread and decode_frame() caller.
    std::mutex              m_queue_mutex;
    std::condition_variable m_queue_cv;
    std::deque<Decoded_Frame> m_decoded_queue;

    static constexpr unsigned int MAX_QUEUE_SIZE { 8u };  ///< max prefetched frames ahead

    // -----------------------------------------------------------------------
    // Seek request (caller → prefetch thread)
    // -----------------------------------------------------------------------

    std::mutex              m_seek_mutex;
    std::condition_variable m_seek_cv;
    std::atomic<bool>       m_seek_requested  { false };
    std::atomic<int>        m_seek_target     { -1 };     ///< -1 = no pending seek

    // -----------------------------------------------------------------------
    // on_new_sample → prefetch thread signalling
    // -----------------------------------------------------------------------

    std::mutex              m_sample_mutex;
    std::condition_variable m_sample_cv;
    bool                    m_sample_ready    { false };
    bool                    m_eos_reached     { false };

    /// Written by prefetch thread before PLAYING; read by on_new_sample.
    uint8_t*                m_current_slot    { nullptr };
    GstClockTime            m_current_target_pts { GST_CLOCK_TIME_NONE };
    unsigned int            m_current_target_frame { 0 };
    bool                    m_gop_multiple_skip { false };

    // -----------------------------------------------------------------------
    // Misc state
    // -----------------------------------------------------------------------

    VideoMetaData           meta;
    int                     m_last_decoded_frame { -1 };  ///< last frame decoded by prefetch thread
    bool                    m_pipeline_playing   { false };
};
