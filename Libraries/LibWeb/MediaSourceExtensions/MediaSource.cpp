/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "AK/Utf16String.h"
#include <AK/FixedArray.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/MediaSourcePrototype.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/HTML/HTMLMediaElement.h>
#include <LibWeb/MediaSourceExtensions/EventNames.h>
#include <LibWeb/MediaSourceExtensions/MediaSource.h>
#include <LibWeb/MediaSourceExtensions/SourceBuffer.h>
#include <LibWeb/MediaSourceExtensions/SourceBufferList.h>
#include <LibWeb/MimeSniff/MimeType.h>

namespace Web::MediaSourceExtensions {

using Bindings::ReadyState;

GC_DEFINE_ALLOCATOR(MediaSource);

WebIDL::ExceptionOr<GC::Ref<MediaSource>> MediaSource::construct_impl(JS::Realm& realm)
{
    return realm.create<MediaSource>(realm);
}

MediaSource::MediaSource(JS::Realm& realm)
    : DOM::EventTarget(realm)
    , m_source_buffers(realm.create<SourceBufferList>(realm))
    , m_active_source_buffers(realm.create<SourceBufferList>(realm))
{
}

MediaSource::~MediaSource() = default;

void MediaSource::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(MediaSource);
    Base::initialize(realm);
}

void MediaSource::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_source_buffers);
    visitor.visit(m_active_source_buffers);
}

ReadyState MediaSource::ready_state() const
{
    dbgln("is open? {}", m_ready_state == ReadyState::Open);
    return m_ready_state;
}

bool MediaSource::ready_state_is_closed() const
{
    return m_ready_state == ReadyState::Closed;
}

void MediaSource::set_has_ever_been_attached()
{
    m_has_ever_been_attached = true;
}

void MediaSource::set_ready_state_to_open()
{
    dbgln("set_ready_state_to_open");
    m_ready_state = ReadyState::Open;
}

double MediaSource::duration() const
{
    // 1. If the readyState attribute is "closed" then return NaN and abort these steps.
    if (ready_state_is_closed())
        return AK::NaN<double>;

    // 2. Return the current value of the attribute.
    return m_duration;
}

WebIDL::ExceptionOr<void> MediaSource::duration_change_algorithm(double new_duration)
{
    // 1. If the current value of duration is equal to new duration, then return.
    if (m_duration == new_duration)
        return {};

    // FIXME: 2. If new duration is less than the highest presentation timestamp of any buffered coded frames for all SourceBuffer objects in sourceBuffers, then throw an InvalidStateError exception and abort these steps.

    // FIXME: 3. Let highest end time be the largest track buffer ranges end time across all the track buffers across all SourceBuffer objects in sourceBuffers.
    // FIXME: 4. If new duration is less than highest end time, then
    {
        // FIXME: 1. Update new duration to equal highest end time.
    }
    // 5. Update duration to new duration.
    m_duration = new_duration;

    // 6. Use the mirror if necessary algorithm to run the following steps in Window to update the media element's duration:
    if (media_element_assigned_to())
    {
        // 1. Update the media element's duration to new duration.
        // 2. Run the HTMLMediaElement duration change algorithm.
        media_element_assigned_to()->set_duration(new_duration);
    } else {
        // FIXME: Mirror to the remote media element.
        return WebIDL::InvalidStateError::create(realm(), "Not implemented"_utf16);
    }

    return {};
}

WebIDL::ExceptionOr<void> MediaSource::set_duration(double duration)
{
    // 1. If the value being set is negative or NaN then throw a TypeError exception and abort these steps.
    if (duration < 0 || isnan(duration))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Value is negative or NaN"_string };

    // 2. If the readyState attribute is not "open" then throw an InvalidStateError exception and abort these steps.
    if (ready_state() != ReadyState::Open)
        return WebIDL::InvalidStateError::create(realm(), "MediaSource is not open"_utf16);

    // 3. If the updating attribute equals true on any SourceBuffer in sourceBuffers, then throw an InvalidStateError exception and abort these steps.
    if (source_buffers()->is_any_buffer_updating())
        return WebIDL::InvalidStateError::create(realm(), "SourceBuffer is updating"_utf16);

    // 4. Run the duration change algorithm with new duration set to the value being assigned to this attribute. 
    return duration_change_algorithm(duration);
}

void MediaSource::fire_sourceopen_event()
{
    dbgln("fire sourceopen");
    auto event = DOM::Event::create(realm(), EventNames::sourceopen);
    dispatch_event(event);
}

void MediaSource::queue_a_task(GC::Ref<GC::Function<void()>> task)
{
    // FIXME: The MSE spec does not say what task source to use for its tasks. Should this use the media element's
    //        task source? We may not have access to it if we're in a worker.
    GC::Ptr<DOM::Document> document = nullptr;
    if (media_element_assigned_to() != nullptr)
        document = media_element_assigned_to()->document();

    HTML::queue_a_task(HTML::Task::Source::Unspecified, HTML::main_thread_event_loop(), document, task);
}

GC::Ref<SourceBufferList> MediaSource::source_buffers()
{
    return m_source_buffers;
}

GC::Ref<SourceBufferList> MediaSource::active_source_buffers()
{
    return m_active_source_buffers;
}

Utf16String MediaSource::next_track_id()
{
    if (m_next_track_id_counter_buffer.is_empty()) {
        m_next_track_id_counter_buffer.resize(1);
        m_next_track_id_counter_buffer[0] = '1';
        return "1"_utf16;
    }

    auto length = m_next_track_id_counter_buffer.size();

    // Resize the buffer for one more leading character if a carry may occur at the first index.
    auto shifted = false;
    if (m_next_track_id_counter_buffer[0] == '9') {
        m_next_track_id_counter_buffer.resize(length + 1);
        shifted = true;
    }

    bool carry = true;
    for (size_t i = length; i-- > 0;) {
        auto digit = m_next_track_id_counter_buffer[i];
        VERIFY(digit >= '0' && digit <= '9');

        auto& destination = m_next_track_id_counter_buffer[i + (shifted ? 1 : 0)];
        if (!carry) {
            destination = digit;
        } else if (digit == '9') {
            destination = '0';
        } else {
            destination = static_cast<char>(digit + 1);
            carry = false;
        }
    }
    if (shifted)
        m_next_track_id_counter_buffer[0] = carry ? '1' : '0';
    if (m_next_track_id_counter_buffer[0] == '0')
        return Utf16String::from_ascii_without_validation(m_next_track_id_counter_buffer.bytes().slice(1));
    return Utf16String::from_ascii_without_validation(m_next_track_id_counter_buffer);
}

void MediaSource::set_assigned_to_media_element(Badge<HTML::HTMLMediaElement>, HTML::HTMLMediaElement& media_element)
{
    m_media_element_assigned_to = media_element;
}

void MediaSource::unassign_from_media_element(Badge<HTML::HTMLMediaElement>)
{
    m_media_element_assigned_to = nullptr;
}

// https://w3c.github.io/media-source/#dom-mediasource-onsourceopen
void MediaSource::set_onsourceopen(GC::Ptr<WebIDL::CallbackType> event_handler)
{
    set_event_handler_attribute(EventNames::sourceopen, event_handler);
}

// https://w3c.github.io/media-source/#dom-mediasource-onsourceopen
GC::Ptr<WebIDL::CallbackType> MediaSource::onsourceopen()
{
    return event_handler_attribute(EventNames::sourceopen);
}

// https://w3c.github.io/media-source/#dom-mediasource-onsourceended
void MediaSource::set_onsourceended(GC::Ptr<WebIDL::CallbackType> event_handler)
{
    set_event_handler_attribute(EventNames::sourceended, event_handler);
}

// https://w3c.github.io/media-source/#dom-mediasource-onsourceended
GC::Ptr<WebIDL::CallbackType> MediaSource::onsourceended()
{
    return event_handler_attribute(EventNames::sourceended);
}

// https://w3c.github.io/media-source/#dom-mediasource-onsourceclose
void MediaSource::set_onsourceclose(GC::Ptr<WebIDL::CallbackType> event_handler)
{
    set_event_handler_attribute(EventNames::sourceclose, event_handler);
}

// https://w3c.github.io/media-source/#dom-mediasource-onsourceclose
GC::Ptr<WebIDL::CallbackType> MediaSource::onsourceclose()
{
    return event_handler_attribute(EventNames::sourceclose);
}

// https://w3c.github.io/media-source/#addsourcebuffer-method
WebIDL::ExceptionOr<GC::Ref<SourceBuffer>> MediaSource::add_source_buffer(String const& type)
{
    // 1. If type is an empty string then throw a TypeError exception and abort these steps.
    if (type.is_empty()) {
        return WebIDL::SimpleException {
            WebIDL::SimpleExceptionType::TypeError,
            "SourceBuffer type must not be empty"sv
        };
    }

    // 2. If type contains a MIME type that is not supported or contains a MIME type that is not
    //    supported with the types specified for the other SourceBuffer objects in sourceBuffers,
    //    then throw a NotSupportedError exception and abort these steps.
    if (!is_type_supported(type)) {
        return WebIDL::NotSupportedError::create(realm(), "Unsupported MIME type"_utf16);
    }

    // FIXME: 3. If the user agent can't handle any more SourceBuffer objects or if creating a SourceBuffer
    //           based on type would result in an unsupported SourceBuffer configuration, then throw a
    //           QuotaExceededError exception and abort these steps.

    // 4. If the readyState attribute is not in the "open" state then throw an InvalidStateError exception and abort these steps.
    if (ready_state() != ReadyState::Open)
        return WebIDL::InvalidStateError::create(realm(), "MediaSource is not open"_utf16);

    // 5. Let buffer be a new instance of a ManagedSourceBuffer if this is a ManagedMediaSource, or
    //    a SourceBuffer otherwise, with their respective associated resources.
    [[maybe_unused]] auto buffer = make_source_buffer();

    // FIXME: 6. Set buffer's [[generate timestamps flag]] to the value in the "Generate Timestamps Flag"
    //           column of the Media Source Extensions™ Byte Stream Format Registry entry that is
    //           associated with type.
    // FIXME: 7. If buffer's [[generate timestamps flag]] is true, set buffer's mode to "sequence".
    //           Otherwise, set buffer's mode to "segments".
    // 8. Append buffer to this's sourceBuffers.
    // 9. Queue a task to fire an event named addsourcebuffer at this's sourceBuffers.
    m_source_buffers->append(buffer);

    // 10. Return buffer.
    return buffer;
}

GC::Ref<SourceBuffer> MediaSource::make_source_buffer()
{
    return realm().create<SourceBuffer>(realm(), GC::Ref(*this));
}

// https://w3c.github.io/media-source/#dom-mediasource-istypesupported
bool MediaSource::is_type_supported(String const& type)
{
    // 1. If type is an empty string, then return false.
    if (type.is_empty())
        return false;

    // 2. If type does not contain a valid MIME type string, then return false.
    auto mime_type = MimeSniff::MimeType::parse(type);
    if (!mime_type.has_value())
        return false;

    // FIXME: Ask LibMedia about what it supports instead of hardcoding this.

    // 3. If type contains a media type or media subtype that the MediaSource does not support, then
    //    return false.
    if (mime_type->type() != "video" || mime_type->subtype() != "webm")
        return false;

    // 4. If type contains a codec that the MediaSource does not support, then return false.
    // 5. If the MediaSource does not support the specified combination of media type, media
    //    subtype, and codecs then return false.
    auto codecs_iter = mime_type->parameters().find("codecs"sv);
    if (codecs_iter == mime_type->parameters().end())
        return false;
    auto codecs = codecs_iter->value.bytes_as_string_view();
    auto had_unsupported_codec = false;
    codecs.for_each_split_view(',', SplitBehavior::Nothing, [&](auto const& codec) {
        if (!codec.starts_with("vp9"sv) && !codec.starts_with("opus"sv)) {
            had_unsupported_codec = true;
            return IterationDecision::Break;
        }
        return IterationDecision::Continue;
    });
    if (had_unsupported_codec)
        return false;

    // 6. Return true.
    return true;
}

}
