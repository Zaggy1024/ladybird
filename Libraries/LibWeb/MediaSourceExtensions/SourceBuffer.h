/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2026, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/HashMap.h>
#include <AK/Math.h>
#include <AK/Optional.h>
#include <LibWeb/Bindings/SourceBufferPrototype.h>
#include <LibWeb/DOM/EventTarget.h>

namespace Web::MediaSourceExtensions {

class ByteStreamParser;
class TrackBuffer;

// https://w3c.github.io/media-source/#sourcebuffer-append-state
enum class AppendState : u8 {
    WaitingForSegment,
    ParsingInitSegment,
    ParsingMediaSegment,
};

// https://w3c.github.io/media-source/#dom-sourcebuffer
class SourceBuffer : public DOM::EventTarget {
    WEB_PLATFORM_OBJECT(SourceBuffer, DOM::EventTarget);
    GC_DECLARE_ALLOCATOR(SourceBuffer);

public:
    void set_onupdatestart(GC::Ptr<WebIDL::CallbackType>);
    GC::Ptr<WebIDL::CallbackType> onupdatestart();

    void set_onupdate(GC::Ptr<WebIDL::CallbackType>);
    GC::Ptr<WebIDL::CallbackType> onupdate();

    void set_onupdateend(GC::Ptr<WebIDL::CallbackType>);
    GC::Ptr<WebIDL::CallbackType> onupdateend();

    void set_onerror(GC::Ptr<WebIDL::CallbackType>);
    GC::Ptr<WebIDL::CallbackType> onerror();

    void set_onabort(GC::Ptr<WebIDL::CallbackType>);
    GC::Ptr<WebIDL::CallbackType> onabort();

    // https://w3c.github.io/media-source/#dom-sourcebuffer-updating
    bool updating() const { return m_updating; }

    // https://w3c.github.io/media-source/#dom-sourcebuffer-timestampoffset
    double timestamp_offset() const { return m_timestamp_offset; }
    WebIDL::ExceptionOr<void> set_timestamp_offset(double);

    // https://w3c.github.io/media-source/#dom-sourcebuffer-appendwindowstart
    double append_window_start() const { return m_append_window_start; }
    WebIDL::ExceptionOr<void> set_append_window_start(double);

    // https://w3c.github.io/media-source/#dom-sourcebuffer-appendwindowend
    double append_window_end() const { return m_append_window_end; }
    WebIDL::ExceptionOr<void> set_append_window_end(double);

    // https://w3c.github.io/media-source/#dom-sourcebuffer-mode
    Bindings::AppendMode mode() const { return m_mode; }
    WebIDL::ExceptionOr<void> set_mode(Bindings::AppendMode);

    // IDL methods
    WebIDL::ExceptionOr<void> append_buffer(GC::Root<WebIDL::BufferSource> const&);
    WebIDL::ExceptionOr<void> abort();
    WebIDL::ExceptionOr<void> change_type(String const& type);
    WebIDL::ExceptionOr<void> remove(double start, double end);

protected:
    SourceBuffer(JS::Realm&, MediaSource&);

    virtual ~SourceBuffer() override;

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

private:
    // https://w3c.github.io/media-source/#sourcebuffer-prepare-append
    WebIDL::ExceptionOr<void> prepare_append();

    // https://w3c.github.io/media-source/#sourcebuffer-buffer-append
    // Run asynchronously when appendBuffer() is called.
    void run_buffer_append_algorithm();

    void remove_bytes_from_input_buffer(size_t);

    // https://w3c.github.io/media-source/#sourcebuffer-segment-parser-loop
    bool run_segment_parser_loop();

    // https://w3c.github.io/media-source/#sourcebuffer-reset-parser-state
    void reset_parser_state();

    // https://w3c.github.io/media-source/#sourcebuffer-append-error
    void run_append_error_algorithm();

    // https://w3c.github.io/media-source/#sourcebuffer-init-segment-received
    void run_initialization_segment_received_algorithm();

    // https://w3c.github.io/media-source/#sourcebuffer-coded-frame-processing
    void run_coded_frame_processing_algorithm();

    // https://w3c.github.io/media-source/#sourcebuffer-coded-frame-removal
    void run_coded_frame_removal_algorithm(double start, double end);

    // https://w3c.github.io/media-source/#sourcebuffer-coded-frame-eviction
    void run_coded_frame_eviction_algorithm();

    // https://w3c.github.io/media-source/#sourcebuffer-range-removal
    void run_range_removal_algorithm(double start, double end);

    // Helper to unset timestamps on all track buffers
    void unset_all_track_buffer_timestamps();

    // Helper to set need random access point flag on all track buffers
    void set_need_random_access_point_flag_on_all_track_buffers(bool flag);

    GC::Ref<MediaSource> m_media_source;

    // https://w3c.github.io/media-source/#dom-sourcebuffer-updating
    bool m_updating { false };

    // https://w3c.github.io/media-source/#dom-sourcebuffer-mode
    Bindings::AppendMode m_mode { Bindings::AppendMode::Segments };

    // https://w3c.github.io/media-source/#dom-sourcebuffer-timestampoffset
    double m_timestamp_offset { 0 };

    // https://w3c.github.io/media-source/#dom-sourcebuffer-appendwindowstart
    double m_append_window_start { 0 };

    // https://w3c.github.io/media-source/#dom-sourcebuffer-appendwindowend
    double m_append_window_end { AK::Infinity<double> };

    // https://w3c.github.io/media-source/#dfn-input-buffer
    ByteBuffer m_input_buffer;

    // https://w3c.github.io/media-source/#dfn-buffer-full-flag
    bool m_buffer_full_flag { false };

    // https://w3c.github.io/media-source/#sourcebuffer-append-state
    AppendState m_append_state { AppendState::WaitingForSegment };

    // https://w3c.github.io/media-source/#dfn-group-start-timestamp
    Optional<double> m_group_start_timestamp;

    // https://w3c.github.io/media-source/#dfn-group-end-timestamp
    double m_group_end_timestamp { 0 };

    // https://w3c.github.io/media-source/#dfn-generate-timestamps-flag
    bool m_generate_timestamps_flag { false };

    // https://w3c.github.io/media-source/#dfn-first-initialization-segment-received-flag
    bool m_first_initialization_segment_received_flag { false };

    // https://w3c.github.io/media-source/#dfn-pending-initialization-segment-for-changetype-flag
    bool m_pending_initialization_segment_for_change_type_flag { false };

    OwnPtr<ByteStreamParser> m_parser;
    HashMap<u64, OwnPtr<TrackBuffer>> m_track_buffers;
};

}
