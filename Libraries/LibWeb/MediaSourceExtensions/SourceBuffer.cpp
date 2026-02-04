/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2026, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "LibMedia/DecoderError.H"
#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/MediaSourcePrototype.h>
#include <LibWeb/Bindings/SourceBufferPrototype.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/HTML/HTMLMediaElement.h>
#include <LibWeb/MediaSourceExtensions/ByteStreamParser.h>
#include <LibWeb/MediaSourceExtensions/EventNames.h>
#include <LibWeb/MediaSourceExtensions/MediaSource.h>
#include <LibWeb/MediaSourceExtensions/SourceBuffer.h>
#include <LibWeb/MediaSourceExtensions/SourceBufferList.h>
#include <LibWeb/MediaSourceExtensions/TrackBuffer.h>
#include <LibWeb/WebIDL/Buffers.h>
#include <LibWeb/WebIDL/QuotaExceededError.h>

namespace Web::MediaSourceExtensions {

GC_DEFINE_ALLOCATOR(SourceBuffer);

SourceBuffer::SourceBuffer(JS::Realm& realm, MediaSource& media_source)
    : DOM::EventTarget(realm)
    , m_media_source(media_source)
{
}

SourceBuffer::~SourceBuffer() = default;

void SourceBuffer::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SourceBuffer);
    Base::initialize(realm);
}

void SourceBuffer::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_media_source);
}

// https://w3c.github.io/media-source/#dom-sourcebuffer-onupdatestart
void SourceBuffer::set_onupdatestart(GC::Ptr<WebIDL::CallbackType> event_handler)
{
    set_event_handler_attribute(EventNames::updatestart, event_handler);
}

// https://w3c.github.io/media-source/#dom-sourcebuffer-onupdatestart
GC::Ptr<WebIDL::CallbackType> SourceBuffer::onupdatestart()
{
    return event_handler_attribute(EventNames::updatestart);
}

// https://w3c.github.io/media-source/#dom-sourcebuffer-onupdate
void SourceBuffer::set_onupdate(GC::Ptr<WebIDL::CallbackType> event_handler)
{
    set_event_handler_attribute(EventNames::update, event_handler);
}

// https://w3c.github.io/media-source/#dom-sourcebuffer-onupdate
GC::Ptr<WebIDL::CallbackType> SourceBuffer::onupdate()
{
    return event_handler_attribute(EventNames::update);
}

// https://w3c.github.io/media-source/#dom-sourcebuffer-onupdateend
void SourceBuffer::set_onupdateend(GC::Ptr<WebIDL::CallbackType> event_handler)
{
    set_event_handler_attribute(EventNames::updateend, event_handler);
}

// https://w3c.github.io/media-source/#dom-sourcebuffer-onupdateend
GC::Ptr<WebIDL::CallbackType> SourceBuffer::onupdateend()
{
    return event_handler_attribute(EventNames::updateend);
}

// https://w3c.github.io/media-source/#dom-sourcebuffer-onerror
void SourceBuffer::set_onerror(GC::Ptr<WebIDL::CallbackType> event_handler)
{
    set_event_handler_attribute(EventNames::error, event_handler);
}

// https://w3c.github.io/media-source/#dom-sourcebuffer-onerror
GC::Ptr<WebIDL::CallbackType> SourceBuffer::onerror()
{
    return event_handler_attribute(EventNames::error);
}

// https://w3c.github.io/media-source/#dom-sourcebuffer-onabort
void SourceBuffer::set_onabort(GC::Ptr<WebIDL::CallbackType> event_handler)
{
    set_event_handler_attribute(EventNames::abort, event_handler);
}

// https://w3c.github.io/media-source/#dom-sourcebuffer-onabort
GC::Ptr<WebIDL::CallbackType> SourceBuffer::onabort()
{
    return event_handler_attribute(EventNames::abort);
}

// https://w3c.github.io/media-source/#dom-sourcebuffer-timestampoffset
WebIDL::ExceptionOr<void> SourceBuffer::set_timestamp_offset(double timestamp_offset)
{
    // 1. If this object has been removed from the sourceBuffers attribute of the parent media source
    //    then throw an InvalidStateError exception and abort these steps.
    if (!m_media_source->source_buffers()->contains(*this))
        return WebIDL::InvalidStateError::create(realm(), "SourceBuffer has been removed"_utf16);

    // 2. If the updating attribute equals true, then throw an InvalidStateError exception and abort these steps.
    if (m_updating)
        return WebIDL::InvalidStateError::create(realm(), "SourceBuffer is updating"_utf16);

    // 3. If the readyState attribute of the parent media source is in the "ended" state then run the following steps:
    if (m_media_source->ready_state() == Bindings::ReadyState::Ended) {
        // 3.1. Set the readyState attribute of the parent media source to "open"
        m_media_source->set_ready_state_to_open();
        // 3.2. Queue a task to fire an event named sourceopen at the parent media source.
        MediaSource::queue_a_media_source_task(GC::create_function(heap(), [media_source = m_media_source] {
            media_source->fire_sourceopen_event();
        }));
    }

    // 4. If the [[append state]] equals PARSING_MEDIA_SEGMENT, then throw an InvalidStateError exception
    //    and abort these steps.
    if (m_append_state == AppendState::ParsingMediaSegment)
        return WebIDL::InvalidStateError::create(realm(), "Cannot set timestampOffset while parsing a media segment"_utf16);

    // 5. If the mode attribute equals "sequence", then set the [[group start timestamp]] to the new value.
    if (m_mode == Bindings::AppendMode::Sequence)
        m_group_start_timestamp = timestamp_offset;

    // 6. Update the attribute to the new value.
    m_timestamp_offset = timestamp_offset;

    return {};
}

// https://w3c.github.io/media-source/#dom-sourcebuffer-appendwindowstart
WebIDL::ExceptionOr<void> SourceBuffer::set_append_window_start(double append_window_start)
{
    // 1. If this object has been removed from the sourceBuffers attribute of the parent media source
    //    then throw an InvalidStateError exception and abort these steps.
    if (!m_media_source->source_buffers()->contains(*this))
        return WebIDL::InvalidStateError::create(realm(), "SourceBuffer has been removed"_utf16);

    // 2. If the updating attribute equals true, then throw an InvalidStateError exception and abort these steps.
    if (m_updating)
        return WebIDL::InvalidStateError::create(realm(), "SourceBuffer is updating"_utf16);

    // 3. If the new value is less than 0 or greater than or equal to appendWindowEnd then throw a TypeError
    //    exception and abort these steps.
    if (append_window_start < 0 || append_window_start >= m_append_window_end)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "appendWindowStart must be >= 0 and < appendWindowEnd"sv };

    // 4. Update the attribute to the new value.
    m_append_window_start = append_window_start;

    return {};
}

// https://w3c.github.io/media-source/#dom-sourcebuffer-appendwindowend
WebIDL::ExceptionOr<void> SourceBuffer::set_append_window_end(double append_window_end)
{
    // 1. If this object has been removed from the sourceBuffers attribute of the parent media source
    //    then throw an InvalidStateError exception and abort these steps.
    if (!m_media_source->source_buffers()->contains(*this))
        return WebIDL::InvalidStateError::create(realm(), "SourceBuffer has been removed"_utf16);

    // 2. If the updating attribute equals true, then throw an InvalidStateError exception and abort these steps.
    if (m_updating)
        return WebIDL::InvalidStateError::create(realm(), "SourceBuffer is updating"_utf16);

    // 3. If the new value equals NaN, then throw a TypeError exception and abort these steps.
    if (isnan(append_window_end))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "appendWindowEnd cannot be NaN"sv };

    // 4. If the new value is less than or equal to appendWindowStart then throw a TypeError exception
    //    and abort these steps.
    if (append_window_end <= m_append_window_start)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "appendWindowEnd must be > appendWindowStart"sv };

    // 5. Update the attribute to the new value.
    m_append_window_end = append_window_end;

    return {};
}

// https://w3c.github.io/media-source/#dom-sourcebuffer-mode
WebIDL::ExceptionOr<void> SourceBuffer::set_mode(Bindings::AppendMode mode)
{
    // 1. If this object has been removed from the sourceBuffers attribute of the parent media source
    //    then throw an InvalidStateError exception and abort these steps.
    if (!m_media_source->source_buffers()->contains(*this))
        return WebIDL::InvalidStateError::create(realm(), "SourceBuffer has been removed"_utf16);

    // 2. If the updating attribute equals true, then throw an InvalidStateError exception and abort these steps.
    if (m_updating)
        return WebIDL::InvalidStateError::create(realm(), "SourceBuffer is updating"_utf16);

    // 3. If the [[generate timestamps flag]] equals true and the new value equals "segments",
    //    then throw a TypeError exception and abort these steps.
    if (m_generate_timestamps_flag && mode == Bindings::AppendMode::Segments)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Cannot set mode to 'segments' when generate timestamps flag is true"sv };

    // 4. If the readyState attribute of the parent media source is in the "ended" state then run the following steps:
    if (m_media_source->ready_state() == Bindings::ReadyState::Ended) {
        // 4.1. Set the readyState attribute of the parent media source to "open"
        m_media_source->set_ready_state_to_open();
        // 4.2. Queue a task to fire an event named sourceopen at the parent media source.
        MediaSource::queue_a_media_source_task(GC::create_function(heap(), [media_source = m_media_source] {
            media_source->fire_sourceopen_event();
        }));
    }

    // 5. If the [[append state]] equals PARSING_MEDIA_SEGMENT, then throw an InvalidStateError exception
    //    and abort these steps.
    if (m_append_state == AppendState::ParsingMediaSegment)
        return WebIDL::InvalidStateError::create(realm(), "Cannot change mode while parsing a media segment"_utf16);

    // 6. If the new value equals "sequence", then set the [[group start timestamp]] to the [[group end timestamp]].
    if (mode == Bindings::AppendMode::Sequence)
        m_group_start_timestamp = m_group_end_timestamp;

    // 7. Update the attribute to the new value.
    m_mode = mode;

    return {};
}

// https://w3c.github.io/media-source/#sourcebuffer-prepare-append
WebIDL::ExceptionOr<void> SourceBuffer::prepare_append()
{
    // 1. If the SourceBuffer has been removed from the sourceBuffers attribute of the parent media source then throw an
    //    InvalidStateError exception and abort these steps.
    if (!m_media_source->source_buffers()->contains(*this))
        return WebIDL::InvalidStateError::create(realm(), "SourceBuffer has been removed"_utf16);

    // 2. If the updating attribute equals true, then throw an InvalidStateError exception and abort these steps.
    if (m_updating)
        return WebIDL::InvalidStateError::create(realm(), "SourceBuffer is already updating"_utf16);

    // 3. Let recent element error be determined as follows:
    auto recent_element_error = [&] {
        // If the MediaSource was constructed in a Window
        if (m_media_source->media_element_assigned_to() != nullptr) {
            // Let recent element error be true if the HTMLMediaElement's error attribute is not null.
            // If that attribute is null, then let recent element error be false.
            return m_media_source->media_element_assigned_to()->error() != nullptr;
        }
        // Otherwise
        // FIXME: Let recent element error be the value resulting from the steps for the Window case,
        //        but run on the Window HTMLMediaElement on any change to its error attribute and
        //        communicated by using [[port to worker]] implicit messages.
        //        If such a message has not yet been received, then let recent element error be false.
        return false;
    }();

    // 4. If recent element error is true, then throw an InvalidStateError exception and abort these steps.
    if (recent_element_error)
        return WebIDL::InvalidStateError::create(realm(), "Element has a recent error"_utf16);

    // 5. If the readyState attribute of the parent media source is in the "ended" state then run the following steps:
    if (m_media_source->ready_state() == Bindings::ReadyState::Ended) {
        // 5.1. Set the readyState attribute of the parent media source to "open"
        m_media_source->set_ready_state_to_open();
        // 5.2. Queue a task to fire an event named sourceopen at the parent media source.
        MediaSource::queue_a_media_source_task(GC::create_function(heap(), [media_source = m_media_source] {
            media_source->fire_sourceopen_event();
        }));
    }

    // 6. Run the coded frame eviction algorithm.
    run_coded_frame_eviction_algorithm();

    // 7. If the [[buffer full flag]] equals true, then throw a QuotaExceededError exception and abort these steps.
    if (m_buffer_full_flag)
        return WebIDL::QuotaExceededError::create(realm(), "Buffer is full"_utf16);

    return {};
}

// https://w3c.github.io/media-source/#dom-sourcebuffer-appendbuffer
WebIDL::ExceptionOr<void> SourceBuffer::append_buffer(GC::Root<WebIDL::BufferSource> const& data)
{
    // 1. Run the prepare append algorithm.
    TRY(prepare_append());

    // 2. Add data to the end of the [[input buffer]].
    if (auto array_buffer = data->viewed_array_buffer(); array_buffer && !array_buffer->is_detached()) {
        auto bytes = array_buffer->buffer();
        m_input_buffer.append(bytes.data(), bytes.size());
    }

    // 3. Set the updating attribute to true.
    m_updating = true;

    // 4. Queue a task to fire an event named updatestart at this SourceBuffer object.
    MediaSource::queue_a_media_source_task(GC::create_function(heap(), [this] {
        dispatch_event(DOM::Event::create(realm(), EventNames::updatestart));
    }));

    // 5. Asynchronously run the buffer append algorithm.
    // FIXME: Actually run this asynchronously
    run_buffer_append_algorithm();

    return {};
}

// https://w3c.github.io/media-source/#dom-sourcebuffer-abort
WebIDL::ExceptionOr<void> SourceBuffer::abort()
{
    // 1. If this object has been removed from the sourceBuffers attribute of the parent media source
    //    then throw an InvalidStateError exception and abort these steps.
    if (!m_media_source->source_buffers()->contains(*this))
        return WebIDL::InvalidStateError::create(realm(), "SourceBuffer has been removed"_utf16);

    // 2. If the readyState attribute of the parent media source is not in the "open" state
    //    then throw an InvalidStateError exception and abort these steps.
    if (m_media_source->ready_state() != Bindings::ReadyState::Open)
        return WebIDL::InvalidStateError::create(realm(), "MediaSource is not open"_utf16);

    // 3. If the range removal algorithm is running, then throw an InvalidStateError exception and abort these steps.
    // FIXME: Track whether range removal is running

    // 4. If the updating attribute equals true, then run the following steps:
    if (m_updating) {
        // 4.1. Abort the buffer append algorithm if it is running.
        // FIXME: Actually abort the async algorithm

        // 4.2. Set the updating attribute to false.
        m_updating = false;

        // 4.3. Queue a task to fire an event named abort at this SourceBuffer object.
        MediaSource::queue_a_media_source_task(GC::create_function(heap(), [this] {
            dispatch_event(DOM::Event::create(realm(), EventNames::abort));
        }));

        // 4.4. Queue a task to fire an event named updateend at this SourceBuffer object.
        MediaSource::queue_a_media_source_task(GC::create_function(heap(), [this] {
            dispatch_event(DOM::Event::create(realm(), EventNames::updateend));
        }));
    }

    // 5. Run the reset parser state algorithm.
    reset_parser_state();

    // 6. Set appendWindowStart to the presentation start time.
    m_append_window_start = 0;

    // 7. Set appendWindowEnd to positive Infinity.
    m_append_window_end = AK::Infinity<double>;

    return {};
}

// https://w3c.github.io/media-source/#dom-sourcebuffer-changetype
WebIDL::ExceptionOr<void> SourceBuffer::change_type(String const& type)
{
    // 1. If type is an empty string then throw a TypeError exception and abort these steps.
    if (type.is_empty())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Type cannot be empty"sv };

    // 2. If this object has been removed from the sourceBuffers attribute of the parent media source,
    //    then throw an InvalidStateError exception and abort these steps.
    if (!m_media_source->source_buffers()->contains(*this))
        return WebIDL::InvalidStateError::create(realm(), "SourceBuffer has been removed"_utf16);

    // 3. If the updating attribute equals true, then throw an InvalidStateError exception and abort these steps.
    if (m_updating)
        return WebIDL::InvalidStateError::create(realm(), "SourceBuffer is updating"_utf16);

    // 4. If type contains a MIME type that is not supported or contains a MIME type that is not supported
    //    with the types specified (currently or previously) of SourceBuffer objects in the sourceBuffers
    //    attribute of the parent media source, then throw a NotSupportedError exception and abort these steps.
    if (!MediaSource::is_type_supported(type))
        return WebIDL::NotSupportedError::create(realm(), "Type is not supported"_utf16);

    // 5. If the readyState attribute of the parent media source is in the "ended" state then run the following steps:
    if (m_media_source->ready_state() == Bindings::ReadyState::Ended) {
        // 5.1. Set the readyState attribute of the parent media source to "open"
        m_media_source->set_ready_state_to_open();
        // 5.2. Queue a task to fire an event named sourceopen at the parent media source.
        MediaSource::queue_a_media_source_task(GC::create_function(heap(), [media_source = m_media_source] {
            media_source->fire_sourceopen_event();
        }));
    }

    // 6. Run the reset parser state algorithm.
    reset_parser_state();

    // 7. Update the [[generate timestamps flag]] on this SourceBuffer object to the value in the
    //    "Generate Timestamps Flag" column of the byte stream format registry entry that is associated with type.
    // FIXME: Look up the generate timestamps flag from the registry
    // For now, assume false for most formats
    m_generate_timestamps_flag = false;

    // 8. If the [[generate timestamps flag]] equals true:
    //       Set the mode attribute on this SourceBuffer object to "sequence", including running the
    //       associated steps for that attribute being set.
    //    Otherwise:
    //       Keep the previous value of the mode attribute on this SourceBuffer object, without running
    //       any associated steps for that attribute being set.
    if (m_generate_timestamps_flag)
        TRY(set_mode(Bindings::AppendMode::Sequence));

    // 9. Set the [[pending initialization segment for changeType flag]] on this SourceBuffer object to true.
    m_pending_initialization_segment_for_change_type_flag = true;

    return {};
}

// https://w3c.github.io/media-source/#dom-sourcebuffer-remove
WebIDL::ExceptionOr<void> SourceBuffer::remove(double start, double end)
{
    // 1. If this object has been removed from the sourceBuffers attribute of the parent media source
    //    then throw an InvalidStateError exception and abort these steps.
    if (!m_media_source->source_buffers()->contains(*this))
        return WebIDL::InvalidStateError::create(realm(), "SourceBuffer has been removed"_utf16);

    // 2. If the updating attribute equals true, then throw an InvalidStateError exception and abort these steps.
    if (m_updating)
        return WebIDL::InvalidStateError::create(realm(), "SourceBuffer is updating"_utf16);

    // 3. If duration equals NaN, then throw a TypeError exception and abort these steps.
    // FIXME: Get duration from MediaSource

    // 4. If start is negative or greater than duration, then throw a TypeError exception and abort these steps.
    if (start < 0)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "start must be >= 0"sv };

    // 5. If end is less than or equal to start or end equals NaN, then throw a TypeError exception and abort these steps.
    if (end <= start || isnan(end))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "end must be > start and not NaN"sv };

    // 6. If the readyState attribute of the parent media source is in the "ended" state then run the following steps:
    if (m_media_source->ready_state() == Bindings::ReadyState::Ended) {
        // 6.1. Set the readyState attribute of the parent media source to "open"
        m_media_source->set_ready_state_to_open();
        // 6.2. Queue a task to fire an event named sourceopen at the parent media source.
        MediaSource::queue_a_media_source_task(GC::create_function(heap(), [media_source = m_media_source] {
            media_source->fire_sourceopen_event();
        }));
    }

    // 7. Run the range removal algorithm with start and end as the start and end of the removal range.
    run_range_removal_algorithm(start, end);

    return {};
}

// https://w3c.github.io/media-source/#sourcebuffer-buffer-append
void SourceBuffer::run_buffer_append_algorithm()
{
    // 1. Run the segment parser loop algorithm.
    bool aborted = !run_segment_parser_loop();

    // 2. If the segment parser loop algorithm in the previous step was aborted, then abort this algorithm.
    if (aborted)
        return;

    // 3. Set the updating attribute to false.
    m_updating = false;

    // 4. Queue a task to fire an event named update at this SourceBuffer object.
    MediaSource::queue_a_media_source_task(GC::create_function(heap(), [this] {
        dispatch_event(DOM::Event::create(realm(), EventNames::update));
    }));

    // 5. Queue a task to fire an event named updateend at this SourceBuffer object.
    MediaSource::queue_a_media_source_task(GC::create_function(heap(), [this] {
        dispatch_event(DOM::Event::create(realm(), EventNames::updateend));
    }));
}

void SourceBuffer::remove_bytes_from_input_buffer(size_t bytes)
{
    auto remaining_bytes = m_input_buffer.bytes().slice(bytes);
    AK::TypedTransfer<u8>::move(m_input_buffer.data(), remaining_bytes.data(), remaining_bytes.size());
    m_input_buffer.resize(remaining_bytes.size());
}

// https://w3c.github.io/media-source/#sourcebuffer-segment-parser-loop
bool SourceBuffer::run_segment_parser_loop()
{
    VERIFY(m_parser);
    size_t current_input_buffer_position = 0;
    auto input_buffer = [&]() {
        return m_input_buffer.bytes().slice(current_input_buffer_position);
    };

    while (true) {
        // 1. Loop Top: If the [[input buffer]] is empty, then jump to the need more data step below.
        if (m_input_buffer.is_empty())
            goto need_more_data;

        // 2. If the [[input buffer]] contains bytes that violate the SourceBuffer byte stream format specification,
        //    then run the append error algorithm and abort this algorithm.
        // NB: We'll react to this below when actually parsing the segments.

        // 3. Remove any bytes that the byte stream format specifications say MUST be ignored from the start of
        //    the [[input buffer]].
        current_input_buffer_position += m_parser->check_for_bytes_to_skip(input_buffer());

        // 4. If the [[append state]] equals WAITING_FOR_SEGMENT, then run the following steps:
        if (m_append_state == AppendState::WaitingForSegment) {
            auto input_buffer_type = m_parser->sniff_segment_type(input_buffer());
            // 1. If the beginning of the [[input buffer]] indicates the start of an initialization segment, set the [[append state]] to PARSING_INIT_SEGMENT.
            if (input_buffer_type == SegmentType::InitializationSegment) {
                m_append_state = AppendState::ParsingInitSegment;
                // 2. If the beginning of the [[input buffer]] indicates the start of a media segment, set [[append state]] to PARSING_MEDIA_SEGMENT.
            } else if (input_buffer_type == SegmentType::MediaSegment) {
                m_append_state = AppendState::ParsingMediaSegment;
            } else if (input_buffer_type == SegmentType::Incomplete) {
                // NB: If we cannot determine the type due to an incomplete segment, this is equivalent to if we were
                //     parsing an initialization segment and didn't have enough data, which would result in jumping to
                //     the need more data step.
                goto need_more_data;
            } else {
                VERIFY(input_buffer_type == SegmentType::Unknown);
                run_append_error_algorithm();
                break;
            }

            // 3. Jump to the loop top step above.
            continue;
        }

        // 5. If the [[append state]] equals PARSING_INIT_SEGMENT, then run the following steps:
        if (m_append_state == AppendState::ParsingInitSegment) {
            // 1. If the [[input buffer]] does not contain a complete initialization segment yet, then jump to the need more data step below.
            auto parse_init_segment_result = m_parser->parse_initialization_segment(input_buffer());
            if (parse_init_segment_result.is_error()) {
                if (parse_init_segment_result.error().category() == Media::DecoderErrorCategory::EndOfStream)
                    goto need_more_data;
                run_append_error_algorithm();
                break;
            }
            // 2. Run the initialization segment received algorithm.
            

            // 3. Remove the initialization segment bytes from the beginning of the [[input buffer]].
            // 4. Set [[append state]] to WAITING_FOR_SEGMENT.
            // 5. Jump to the loop top step above.
        }

        // 6. If the [[append state]] equals PARSING_MEDIA_SEGMENT, then run the following steps:

        VERIFY_NOT_REACHED();

        // 7. Need more data: Return control to the calling algorithm.
    need_more_data:
        remove_bytes_from_input_buffer(current_input_buffer_position);
        return true;
    }

    remove_bytes_from_input_buffer(current_input_buffer_position);
    return false;
}

// https://w3c.github.io/media-source/#sourcebuffer-reset-parser-state
void SourceBuffer::reset_parser_state()
{
    // 1. If the [[append state]] equals PARSING_MEDIA_SEGMENT and the [[input buffer]] contains some
    //    complete coded frames, then run the coded frame processing algorithm until all of these
    //    complete coded frames have been processed.
    if (m_append_state == AppendState::ParsingMediaSegment) {
        // FIXME: Process any complete coded frames
    }

    // 2. Unset the last decode timestamp on all track buffers.
    // 3. Unset the last frame duration on all track buffers.
    // 4. Unset the highest end timestamp on all track buffers.
    unset_all_track_buffer_timestamps();

    // 5. Set the need random access point flag on all track buffers to true.
    set_need_random_access_point_flag_on_all_track_buffers(true);

    // 6. If the mode attribute equals "sequence", then set the [[group start timestamp]]
    //    to the [[group end timestamp]]
    if (m_mode == Bindings::AppendMode::Sequence)
        m_group_start_timestamp = m_group_end_timestamp;

    // 7. Remove all bytes from the [[input buffer]].
    m_input_buffer.clear();

    // 8. Set [[append state]] to WAITING_FOR_SEGMENT.
    m_append_state = AppendState::WaitingForSegment;
}

// https://w3c.github.io/media-source/#sourcebuffer-append-error
void SourceBuffer::run_append_error_algorithm()
{
    // 1. Run the reset parser state algorithm.
    reset_parser_state();

    // 2. Set the updating attribute to false.
    m_updating = false;

    // 3. Queue a task to fire an event named error at this SourceBuffer object.
    MediaSource::queue_a_media_source_task(GC::create_function(heap(), [this] {
        dispatch_event(DOM::Event::create(realm(), EventNames::error));
    }));

    // 4. Queue a task to fire an event named updateend at this SourceBuffer object.
    MediaSource::queue_a_media_source_task(GC::create_function(heap(), [this] {
        dispatch_event(DOM::Event::create(realm(), EventNames::updateend));
    }));

    // 5. Run the end of stream algorithm with the error parameter set to "decode".
    // FIXME: Call MediaSource::end_of_stream with decode error
}

// https://w3c.github.io/media-source/#sourcebuffer-init-segment-received
void SourceBuffer::run_initialization_segment_received_algorithm()
{
    // FIXME: Implement the full initialization segment received algorithm.
    // This involves:
    // 1. Update the duration attribute if it currently equals NaN
    // 2. If the initialization segment has no audio, video, or text tracks, then run the append error algorithm
    // 3. If the [[first initialization segment received flag]] is true, verify track properties match
    // 4. Let active track flag equal false
    // 5. If the [[first initialization segment received flag]] is false, create track buffers and tracks
    // 6. If active track flag equals true, run potential events steps
    // 7. Set [[first initialization segment received flag]] to true
    // 8. Set [[pending initialization segment for changeType flag]] to false

    m_first_initialization_segment_received_flag = true;
    m_pending_initialization_segment_for_change_type_flag = false;
}

// https://w3c.github.io/media-source/#sourcebuffer-coded-frame-processing
void SourceBuffer::run_coded_frame_processing_algorithm()
{
    // FIXME: Implement the full coded frame processing algorithm.
    // This is a complex algorithm that handles:
    // 1. For each coded frame in the media segment:
    //    - Determine presentation/decode timestamps
    //    - Handle generate timestamps flag
    //    - Handle group start/end timestamps
    //    - Handle discontinuity detection
    //    - Handle append window filtering
    //    - Handle need random access point flag
    //    - Handle frame overlap/splice operations
    //    - Add coded frames to track buffers
    //    - Update track buffer timestamps
    // 2. Update HTMLMediaElement readyState as appropriate
    // 3. Update duration if needed
}

// https://w3c.github.io/media-source/#sourcebuffer-coded-frame-removal
void SourceBuffer::run_coded_frame_removal_algorithm(double start, double end)
{
    (void)start;
    // FIXME:
    // 1. Let start be the starting presentation timestamp for the removal range.
    // 2. Let end be the end presentation timestamp for the removal range.
    // 3. For each track buffer in this SourceBuffer, run the following steps:
    for (auto& [track_id, track_buffer] : m_track_buffers) {
        // 1. Let remove end timestamp be the current value of duration
        // FIXME: Get duration from MediaSource

        // 2. If this track buffer has a random access point timestamp that is greater than or equal to end,
        //      then update remove end timestamp to that random access point timestamp.
        auto rap_timestamp = track_buffer->next_random_access_point_timestamp_after(end);
        (void)rap_timestamp;

        // 3. Remove all media data, from this track buffer, that contain starting timestamps
        //      greater than or equal to start and less than the remove end timestamp.
        // FIXME: Actually remove frames and handle the nested steps.

        // 4. Remove all possible decoding dependencies on the coded frames removed in the previous step by removing
        //    all coded frames from this track buffer between those frames removed in the previous step and the next
        //    random access point after those removed frames.

        // 5. If this object is in activeSourceBuffers, the current playback position is greater than or equal to start
        //    and less than the remove end timestamp, and HTMLMediaElement's readyState is greater than HAVE_METADATA,
        //    then set the HTMLMediaElement's readyState attribute to HAVE_METADATA and stall playback.
    }
}

// https://w3c.github.io/media-source/#sourcebuffer-coded-frame-eviction
void SourceBuffer::run_coded_frame_eviction_algorithm()
{
    // FIXME:
    // 1. Let new data equal the data that is about to be appended to this SourceBuffer.
    // 2. If the [[buffer full flag]] equals false, then abort these steps.
    // 3. Let removal ranges equal a list of presentation time ranges that can be evicted from the presentation to make
    //    room for the new data.
    // 4. For each range in removal ranges, run the coded frame removal algorithm with start and end equal to the removal
    //    range start and end timestamp respectively.
    dbgln("FIXME: Evict some data from the SourceBuffer.");
}

// https://w3c.github.io/media-source/#sourcebuffer-range-removal
void SourceBuffer::run_range_removal_algorithm(double start, double end)
{
    // 1. Let start equal the starting presentation timestamp for the removal range, in seconds
    //    measured from presentation start time.
    // 2. Let end equal the end presentation timestamp for the removal range, in seconds
    //    measured from presentation start time.
    // 3. Set the updating attribute to true.
    m_updating = true;

    // 4. Queue a task to fire an event named updatestart at this SourceBuffer object.
    MediaSource::queue_a_media_source_task(GC::create_function(heap(), [this] {
        dispatch_event(DOM::Event::create(realm(), EventNames::updatestart));
    }));

    // 5. Return control to the caller and run the rest of the steps asynchronously.
    // FIXME: Create a thread to process tasks and run these steps asynchronously.

    // 6. Run the coded frame removal algorithm with start and end as the start and end of the removal range.
    run_coded_frame_removal_algorithm(start, end);

    // 7. Set the updating attribute to false.
    m_updating = false;

    // 8. Queue a task to fire an event named update at this SourceBuffer object.
    MediaSource::queue_a_media_source_task(GC::create_function(heap(), [this] {
        dispatch_event(DOM::Event::create(realm(), EventNames::update));
    }));

    // 9. Queue a task to fire an event named updateend at this SourceBuffer object.
    MediaSource::queue_a_media_source_task(GC::create_function(heap(), [this] {
        dispatch_event(DOM::Event::create(realm(), EventNames::updateend));
    }));
}

void SourceBuffer::unset_all_track_buffer_timestamps()
{
    for (auto& [track_id, track_buffer] : m_track_buffers) {
        track_buffer->unset_last_decode_timestamp();
        track_buffer->unset_last_frame_duration();
        track_buffer->unset_highest_end_timestamp();
    }
}

void SourceBuffer::set_need_random_access_point_flag_on_all_track_buffers(bool flag)
{
    for (auto& [track_id, track_buffer] : m_track_buffers) {
        track_buffer->set_need_random_access_point_flag(flag);
    }
}

}
