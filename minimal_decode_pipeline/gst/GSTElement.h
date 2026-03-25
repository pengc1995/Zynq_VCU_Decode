#pragma once

#ifndef USE_HAL_MOCK

#include "Gst_object_deleter.h"
#include <gst/gstpipeline.h>
#include <gst/gstelement.h>
#include "GSTObject.h"
// #include <Utils/Kron_Exception.h>
// #include <Utils/annotation_tags.h>
// #include <fmt/format.h>


using GSTElement = GSTObject<GstElement,decltype(gst_object_deleter)>;   ///< RAII wrapper for GstElement


template<
    typename...Args>
inline
auto
make_gst_element(
        Args&& ...args )
{
    return make_gst_object<

        GSTElement,
        decltype(
            []( char const* type, char const* name )
            {
                return gst_element_factory_make( type, name );
            }),
        decltype(
            []( char const* type, char const* name )
            {
                throw "GST error";
                    // Kron::Kron_Exception{
                    //     fmt::format(
                    //         MSG2USR "GST error: element '{1}' of type '{0}' could not be created.",
                    //         type,
                    //         name ) };
            })>

        (std::forward<Args>(args)...);
}

template<
    typename...Args>
inline
auto
make_gst_pipeline(
        Args&& ...args )
{
    return make_gst_object<

        GSTElement,
        decltype(
            []( char const* name )
            {
                return gst_pipeline_new( name );
            }),
        decltype(
            []( char const* name)
            {
                throw "GST error";
                    // Kron::Kron_Exception{
                    //     fmt::format(
                    //         MSG2USR "GST error: pipeline '{0}' could not be created.",
                    //         name ) };

            })>

        (std::forward<Args>(args)...);
}

#endif
