#pragma once

#ifndef USE_HAL_MOCK

#include <utility>
#include <gstreamer-1.0/gst/gstobject.h>

///
/// RAII wrapper around GST objects
///
template<
    typename T,
    typename Deleter>
class GSTObject
{
    /// rule of five
public:
    GSTObject() = default;
public:
    explicit
    GSTObject(
            T* const el
    ) noexcept
        :   element (el)
    {}
public:
    GSTObject( GSTObject const & other ) = delete;
public:
    GSTObject(
            GSTObject&& other
    ) noexcept
        :   element (other.element)
    {
        other.element = NULL;
    }
public:
    GSTObject& operator=( GSTObject const & other ) = delete;
public:
    GSTObject& operator=( GSTObject&& other )
    {
        Deleter()( element );
        element = other.element;
        other.element = NULL;
        return *this;
    }
public:
    ~GSTObject()
    {
        if ( element and (GST_OBJECT_REFCOUNT_VALUE(element)) )
        {
            Deleter()( element );
        }
    }

    /// getter
public:
    auto
    get() const
    {
        return element;
    }

private:
    T* element = NULL;
};

template<
    typename Container,
    typename GST_Create_Fun_Policy,
    typename Error_Handling_Fun_Policy,
    typename...Args>
inline
auto
make_gst_object(
        Args&& ...args )
{
    Container element{
        GST_Create_Fun_Policy()( std::forward<Args>(args)... )
    };

    if ( not element.get() )
    {
        Error_Handling_Fun_Policy()( std::forward<Args>(args)... );
    }

    return element;
}

#endif
