/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <AK/ConditionVariable.h>
#include <AK/HashMap.h>
#include <AK/Mutex.h>
#include <AK/NeverDestroyed.h>
#include <AK/ThreadID.h>
#include <AK/ThreadSafeWeakable.h>
#include <AK/Vector.h>
#include <LibCore/EventLoop.h>
#include <LibIPC/ConnectionToServer.h>
#include <LibMedia/RemoteVideoDecoder.h>
#include <LibMediaCodecs/CodedFrame.h>
#include <LibMediaCodecs/VideoDecoderClientEndpoint.h>
#include <LibMediaCodecs/VideoDecoderServerEndpoint.h>
#include <LibMediaCodecs/VideoFrame.h>
#include <LibMediaCodecs/VideoFrameHandle.h>
#include <LibMediaCodecs/VideoFramePool.h>
#include <LibThreading/Thread.h>

namespace Media {

static constexpr size_t MINIMUM_RING_SIZE = 4 * MiB;
static constexpr size_t MAXIMUM_RING_SIZE = 256 * MiB;

namespace {

class VideoDecoderClientConnection;

struct EventLoopThread {
    Core::EventLoop* event_loop { nullptr };
    AK::ThreadID thread_id;
};

}

// Shared by a decoder with the connection that routes its server's answers to it.
struct RemoteVideoDecoder::State : public AtomicRefCounted<State> {
    struct Output {
        NonnullRefPtr<PooledVideoFrameSlot> slot;
        VideoFrameHandle handle;
    };

    struct RingEntry {
        u64 ring_serial { 0 };
        u64 offset { 0 };
        u64 size { 0 };
    };

    void forget_server_while_locked()
    {
        connection_was_lost = true;
        connection = {};
        outputs.clear();
        relay_pools.clear();
        ring_entries.clear();
        frames_in_flight = 0;
        failure.clear();
        server_decoder_state_changed.broadcast();
        drained = false;
    }

    Mutex mutex;
    ConditionVariable server_decoder_state_changed { mutex };
    ThreadSafeWeakRef<VideoDecoderClientConnection> connection;
    u64 decoder_id { 0 };
    u32 generation { 0 };
    bool connection_was_lost { false };

    Vector<Output> outputs;
    HashMap<VideoFramePoolID, NonnullRefPtr<VideoFrameRelayPool>> relay_pools;
    Vector<RingEntry> ring_entries;
    size_t frames_in_flight { 0 };
    Optional<DecoderError> failure;
    bool drained { false };

    // Called under the mutex when the server reports something the decoder's caller is not already waiting for, so
    // that destroying the decoder, which clears it, ends its calls.
    Function<void()> output_arrived_callback;
    bool caller_is_waiting_for_output { false };

    void tell_caller_of_new_state_while_locked()
    {
        if (!caller_is_waiting_for_output && output_arrived_callback)
            output_arrived_callback();
    }
};

namespace {

NeverDestroyed<RemoteVideoDecoder::TransportRequester> s_transport_requester;
NeverDestroyed<RemoteVideoDecoder::TransportAnswerWaiter> s_transport_answer_waiter;
Core::EventLoop* s_transport_requester_event_loop { nullptr };
AK::ThreadID s_transport_requester_thread_id;

// A request for a transport is in flight until its answer has been turned into a connection, or found to be none. One
// asked for by another thread is queued to the requester's thread, which sends it itself if it needs it first.
enum class TransportRequestState : u8 {
    None,
    Queued,
    Sent,
};
NeverDestroyed<Mutex> s_transport_request_mutex;
NeverDestroyed<ConditionVariable> s_transport_request_condition { *s_transport_request_mutex };
TransportRequestState s_transport_request_state { TransportRequestState::None };
u64 s_transport_requests_answered { 0 };

NeverDestroyed<Mutex> s_capabilities_mutex;
NeverDestroyed<HashMap<ParsedCodec, Optional<DecoderCapabilities>>> s_capabilities_by_format;

Atomic<u64> s_next_decoder_id { 1 };

template<typename T>
T run_on_thread(EventLoopThread const& thread, Function<T()> function)
{
    if (thread.thread_id.is_current_thread())
        return function();

    IGNORE_USE_IN_ESCAPING_LAMBDA Mutex mutex;
    IGNORE_USE_IN_ESCAPING_LAMBDA ConditionVariable condition { mutex };
    IGNORE_USE_IN_ESCAPING_LAMBDA Optional<T> result;
    thread.event_loop->deferred_invoke([&, function = move(function)] {
        auto function_result = function();
        MutexLocker locker { mutex };
        result = move(function_result);
        condition.broadcast();
    });

    MutexLocker locker { mutex };
    while (!result.has_value())
        condition.wait();
    return result.release_value();
}

// The connection's answers are handled here rather than on a busy event loop, since each one can hold up a decode.
// This thread never waits on another.
EventLoopThread const& connection_thread()
{
    static EventLoopThread const connection_thread = [] {
        IGNORE_USE_IN_ESCAPING_LAMBDA Mutex mutex;
        IGNORE_USE_IN_ESCAPING_LAMBDA ConditionVariable condition { mutex };
        IGNORE_USE_IN_ESCAPING_LAMBDA Optional<EventLoopThread> started_thread;
        auto thread = MUST(Threading::Thread::try_create("VideoDecoderIPC"sv, [&]() -> intptr_t {
            Core::EventLoop event_loop;
            {
                MutexLocker locker { mutex };
                started_thread = EventLoopThread { &event_loop, AK::ThreadID::current() };
                condition.broadcast();
            }
            return event_loop.exec();
        }));
        thread->start();
        thread->detach();

        MutexLocker locker { mutex };
        while (!started_thread.has_value())
            condition.wait();
        return *started_thread;
    }();
    return connection_thread;
}

template<typename T>
T run_on_connection_thread(Function<T()> function)
{
    return run_on_thread(connection_thread(), move(function));
}

// The connection to the VideoDecoderServer, used only on the connection thread except through weak references, whose
// targets are reached under the reference's lock.
class VideoDecoderClientConnection final
    : public IPC::ConnectionToServer<VideoDecoderClientEndpoint, VideoDecoderServerEndpoint>
    , public VideoDecoderClientEndpoint
    , public ThreadSafeWeakable<VideoDecoderClientConnection> {
    C_OBJECT_ABSTRACT(VideoDecoderClientConnection);

public:
    static ErrorOr<ThreadSafeWeakRef<VideoDecoderClientConnection>> acquire()
    {
        VERIFY(!connection_thread().thread_id.is_current_thread());
        if (auto connection = current(); connection.has_value())
            return connection.release_value();

        if (!*s_transport_requester)
            return Error::from_string_literal("No video decoder server transport requester is installed");

        MutexLocker locker { *s_transport_request_mutex };
        // A request answered since the check above has already connected.
        if (auto connection = current(); connection.has_value())
            return connection.release_value();
        auto requests_answered = s_transport_requests_answered;
        if (!s_transport_requester_thread_id.is_current_thread()) {
            if (s_transport_request_state == TransportRequestState::None) {
                s_transport_request_state = TransportRequestState::Queued;
                s_transport_requester_event_loop->deferred_invoke([] {
                    MutexLocker locker { *s_transport_request_mutex };
                    send_transport_request_if_queued(locker);
                });
            }
            while (s_transport_requests_answered == requests_answered)
                s_transport_request_condition->wait();
        } else {
            if (s_transport_request_state == TransportRequestState::None)
                s_transport_request_state = TransportRequestState::Queued;
            send_transport_request_if_queued(locker);
            if (s_transport_requests_answered == requests_answered) {
                locker.unlock();
                (*s_transport_answer_waiter)();
                locker.lock();
            }
        }

        if (auto connection = current(); connection.has_value())
            return connection.release_value();
        return Error::from_string_literal("Could not connect to a video decoder server");
    }

    static void send_transport_request_if_queued(MutexLocker<Mutex>& locker)
    {
        VERIFY(s_transport_requester_thread_id.is_current_thread());
        if (s_transport_request_state != TransportRequestState::Queued)
            return;
        s_transport_request_state = TransportRequestState::Sent;
        // The requester may answer at once.
        locker.unlock();
        (*s_transport_requester)(transport_answered);
        locker.lock();
    }

    static void transport_answered(ErrorOr<NonnullOwnPtr<IPC::Transport>> transport)
    {
        VERIFY(s_transport_requester_thread_id.is_current_thread());
        if (transport.is_error()) {
            dbgln("Could not get a transport to a video decoder server: {}", transport.error());
        } else {
            run_on_connection_thread<ThreadSafeWeakRef<VideoDecoderClientConnection>>([transport = transport.release_value()]() mutable {
                *s_connection = adopt_ref(*new VideoDecoderClientConnection(move(transport)));
                auto connection = (*s_connection)->make_weak_ref();
                MutexLocker locker { *s_current_connection_mutex };
                *s_current_connection = connection;
                return connection;
            });
        }

        MutexLocker locker { *s_transport_request_mutex };
        s_transport_request_state = TransportRequestState::None;
        s_transport_requests_answered++;
        s_transport_request_condition->broadcast();
    }

    static Optional<Optional<DecoderCapabilities>> query_capabilities(ParsedCodec const& codec)
    {
        auto connection = acquire();
        if (connection.is_error()) {
            dbgln("Could not reach the video decoder server: {}", connection.error());
            return {};
        }
        return run_on_connection_thread<Optional<Optional<DecoderCapabilities>>>([codec]() -> Optional<Optional<DecoderCapabilities>> {
            if (s_connection->is_null())
                return {};
            auto response = (*s_connection)->send_sync_but_allow_failure<Messages::VideoDecoderServer::DecoderCapabilities>(codec);
            if (!response)
                return {};
            return response->capabilities();
        });
    }

    virtual ~VideoDecoderClientConnection() override
    {
        revoke_weak_refs();
    }

    void register_decoder(u64 decoder_id, NonnullRefPtr<RemoteVideoDecoder::State> state)
    {
        MutexLocker locker { m_decoders_mutex };
        m_decoders.set(decoder_id, move(state));
    }

    void unregister_decoder(u64 decoder_id)
    {
        MutexLocker locker { m_decoders_mutex };
        m_decoders.remove(decoder_id);
    }

private:
    explicit VideoDecoderClientConnection(NonnullOwnPtr<IPC::Transport> transport)
        : IPC::ConnectionToServer<VideoDecoderClientEndpoint, VideoDecoderServerEndpoint>(*this, move(transport))
    {
    }

    static Optional<ThreadSafeWeakRef<VideoDecoderClientConnection>> current()
    {
        MutexLocker locker { *s_current_connection_mutex };
        return *s_current_connection;
    }

    // Only the connection thread uses this.
    static NeverDestroyed<RefPtr<VideoDecoderClientConnection>> s_connection;
    // The connection that is open, for other threads to reach.
    static NeverDestroyed<Mutex> s_current_connection_mutex;
    static NeverDestroyed<Optional<ThreadSafeWeakRef<VideoDecoderClientConnection>>> s_current_connection;

    virtual void die() override
    {
        // NB: Forgetting this connection below can drop the last reference to it.
        NonnullRefPtr keep_alive_until_death_is_handled = *this;
        revoke_weak_refs();
        if (*s_connection == this) {
            *s_connection = nullptr;
            MutexLocker locker { *s_current_connection_mutex };
            *s_current_connection = {};
        }

        HashMap<u64, NonnullRefPtr<RemoteVideoDecoder::State>> decoders;
        {
            MutexLocker locker { m_decoders_mutex };
            decoders = move(m_decoders);
        }
        for (auto& [decoder_id, state] : decoders) {
            MutexLocker locker { state->mutex };
            if (state->decoder_id != decoder_id)
                continue;
            state->forget_server_while_locked();
            state->tell_caller_of_new_state_while_locked();
        }
    }

    RefPtr<RemoteVideoDecoder::State> find_decoder(u64 decoder_id)
    {
        MutexLocker locker { m_decoders_mutex };
        if (auto state = m_decoders.get(decoder_id); state.has_value())
            return *state;
        return nullptr;
    }

    // The callback returns whether it left something new for the decoder's caller to take.
    template<typename Callback>
    void with_decoder(u64 decoder_id, Callback callback)
    {
        auto state = find_decoder(decoder_id);
        if (!state)
            return;
        MutexLocker locker { state->mutex };
        if (state->decoder_id != decoder_id)
            return;
        auto caller_has_something_to_take = callback(*state);
        state->server_decoder_state_changed.broadcast();
        if (caller_has_something_to_take)
            state->tell_caller_of_new_state_while_locked();
    }

    // A server that breaks the protocol is treated as lost, so that its decoders start over with a new one.
    void disconnect_misbehaving_server(StringView reason, Error const& error)
    {
        dbgln("The video decoder server misbehaved ({}: {}), disconnecting.", reason, error);
        shutdown();
    }

    virtual void video_frame_slot_announced(u64 decoder_id, VideoFramePoolID pool_id, u32 slot_index, Core::AnonymousBuffer slot_buffer, RefPtr<VideoSurface> surface) override
    {
        Optional<Error> misbehavior;
        with_decoder(decoder_id, [&](RemoteVideoDecoder::State& state) {
            RefPtr<VideoFrameRelayPool> relay_pool;
            if (auto existing_relay_pool = state.relay_pools.get(pool_id); existing_relay_pool.has_value())
                relay_pool = *existing_relay_pool;
            if (!relay_pool) {
                auto relay_pool_or_error = VideoFrameRelayPool::create([weak_connection = make_weak_ref(), pool_id](u32 released_slot_index) {
                    weak_connection.with_target([&](auto& connection) {
                        connection.async_release_frame(pool_id, released_slot_index);
                    });
                });
                if (relay_pool_or_error.is_error()) {
                    state.failure = DecoderError::with_description(DecoderErrorCategory::Memory, "Could not create a pool to relay decoded video frames"sv);
                    return true;
                }
                relay_pool = relay_pool_or_error.release_value();
                state.relay_pools.set(pool_id, *relay_pool);
            }
            if (auto result = relay_pool->set_slot_storage(slot_index, move(slot_buffer), move(surface)); result.is_error())
                misbehavior = result.release_error();
            return false;
        });
        if (misbehavior.has_value())
            disconnect_misbehaving_server("unusable slot"sv, *misbehavior);
    }

    virtual void frame_decoded(u64 decoder_id, u32 generation, VideoFrameHandle handle) override
    {
        auto frame_was_taken = false;
        Optional<Error> misbehavior;
        with_decoder(decoder_id, [&](RemoteVideoDecoder::State& state) {
            if (generation != state.generation)
                return false;
            auto relay_pool = state.relay_pools.get(handle.pool_id);
            if (!relay_pool.has_value()) {
                misbehavior = Error::from_string_literal("The frame is in a pool that was never announced");
                return false;
            }
            auto slot_or_error = (*relay_pool)->try_adopt_published_slot(handle.slot_index, handle.slot_acquisition_id);
            if (slot_or_error.is_error()) {
                misbehavior = slot_or_error.release_error();
                return false;
            }
            frame_was_taken = true;
            state.outputs.append({ slot_or_error.release_value(), handle });
            return true;
        });
        if (misbehavior.has_value()) {
            disconnect_misbehaving_server("unusable frame"sv, *misbehavior);
            return;
        }
        // A frame that nobody wants anymore is returned to the server right away.
        if (!frame_was_taken)
            async_release_frame(handle.pool_id, handle.slot_index);
    }

    virtual void coded_frame_consumed(u64 decoder_id) override
    {
        with_decoder(decoder_id, [&](RemoteVideoDecoder::State& state) {
            if (state.frames_in_flight > 0)
                state.frames_in_flight--;
            if (!state.ring_entries.is_empty())
                state.ring_entries.remove(0);
            return false;
        });
    }

    virtual void decoder_failed(u64 decoder_id, u32 generation, DecoderError error) override
    {
        with_decoder(decoder_id, [&](RemoteVideoDecoder::State& state) {
            if (generation != state.generation || state.failure.has_value())
                return false;
            state.failure = move(error);
            return true;
        });
    }

    virtual void decoder_drained(u64 decoder_id, u32 generation) override
    {
        with_decoder(decoder_id, [&](RemoteVideoDecoder::State& state) {
            if (generation != state.generation)
                return false;
            state.drained = true;
            return true;
        });
    }

    Mutex m_decoders_mutex;
    HashMap<u64, NonnullRefPtr<RemoteVideoDecoder::State>> m_decoders;
};

NeverDestroyed<RefPtr<VideoDecoderClientConnection>> VideoDecoderClientConnection::s_connection;
NeverDestroyed<Mutex> VideoDecoderClientConnection::s_current_connection_mutex;
NeverDestroyed<Optional<ThreadSafeWeakRef<VideoDecoderClientConnection>>> VideoDecoderClientConnection::s_current_connection;

}

void RemoteVideoDecoder::set_transport_requester(TransportRequester requester, TransportAnswerWaiter answer_waiter)
{
    *s_transport_requester = move(requester);
    *s_transport_answer_waiter = move(answer_waiter);
    s_transport_requester_event_loop = &Core::EventLoop::current();
    s_transport_requester_thread_id = AK::ThreadID::current();
}

Optional<DecoderCapabilities> RemoteVideoDecoder::capabilities(ParsedCodec const& codec)
{
    if (track_type_from_codec_id(codec.codec_id()) != TrackType::Video || !*s_transport_requester)
        return {};

    auto support_key = normalize_parsed_codec_for_support_keying(codec);
    {
        MutexLocker locker { *s_capabilities_mutex };
        if (auto cached_capabilities = s_capabilities_by_format->get(support_key); cached_capabilities.has_value())
            return *cached_capabilities;
    }

    auto capabilities = VideoDecoderClientConnection::query_capabilities(support_key);
    // An unanswered query is asked again next time, in case the server can be reached by then.
    if (!capabilities.has_value())
        return {};

    MutexLocker locker { *s_capabilities_mutex };
    s_capabilities_by_format->set(support_key, *capabilities);
    return *capabilities;
}

DecoderErrorOr<NonnullOwnPtr<RemoteVideoDecoder>> RemoteVideoDecoder::try_create(CodecID codec_id, ReadonlyBytes codec_initialization_data)
{
    auto codec_configuration = DECODER_TRY_ALLOC(ByteBuffer::copy(codec_initialization_data));
    auto state = DECODER_TRY_ALLOC(try_make_ref_counted<State>());
    auto decoder = DECODER_TRY_ALLOC(adopt_nonnull_own_or_enomem(new (nothrow) RemoteVideoDecoder(codec_id, move(codec_configuration), move(state))));
    TRY(decoder->start_server_decoder());
    return decoder;
}

RemoteVideoDecoder::RemoteVideoDecoder(CodecID codec_id, ByteBuffer codec_configuration, NonnullRefPtr<State> state)
    : m_codec_id(codec_id)
    , m_codec_configuration(move(codec_configuration))
    , m_state(move(state))
{
}

RemoteVideoDecoder::~RemoteVideoDecoder()
{
    ThreadSafeWeakRef<VideoDecoderClientConnection> connection;
    u64 decoder_id = 0;
    Vector<State::Output> outputs;
    {
        MutexLocker locker { m_state->mutex };
        connection = m_state->connection;
        decoder_id = m_state->decoder_id;
        outputs = move(m_state->outputs);
        m_state->relay_pools.clear();
        m_state->output_arrived_callback = nullptr;
    }
    connection.with_target([&](auto& connection) {
        connection.async_destroy_decoder(decoder_id);
        connection.unregister_decoder(decoder_id);
    });
}

DecoderErrorOr<void> RemoteVideoDecoder::start_server_decoder()
{
    if (!*s_transport_requester)
        return DecoderError::with_description(DecoderErrorCategory::NotImplemented, "This process cannot reach a video decoder server"sv);
    if (m_server_loss_count > MAXIMUM_SERVER_LOSSES)
        return DecoderError::with_description(DecoderErrorCategory::NotImplemented, "The video decoder server was lost too many times"sv);

    auto connection_or_error = VideoDecoderClientConnection::acquire();
    if (connection_or_error.is_error())
        return DecoderError::format(DecoderErrorCategory::NotImplemented, "Could not reach the video decoder server: {}", connection_or_error.error());
    auto connection = connection_or_error.release_value();

    auto decoder_id = s_next_decoder_id.fetch_add(1, AK::MemoryOrder::memory_order_relaxed);
    u32 generation = 0;
    {
        MutexLocker locker { m_state->mutex };
        m_state->forget_server_while_locked();
        m_state->connection_was_lost = false;
        m_state->connection = connection;
        m_state->decoder_id = decoder_id;
        generation = m_state->generation;
    }
    m_ring = {};
    m_server_loss_was_reported = false;

    auto connection_is_open = false;
    connection.with_target([&](auto& connection) {
        connection.register_decoder(decoder_id, m_state);
        connection.async_create_decoder(decoder_id, generation, m_codec_id, m_codec_configuration);
        connection_is_open = true;
    });
    if (!connection_is_open) {
        MutexLocker locker { m_state->mutex };
        m_state->forget_server_while_locked();
    }
    return {};
}

void RemoteVideoDecoder::set_output_arrived_callback(Function<void()> callback)
{
    MutexLocker locker { m_state->mutex };
    m_state->output_arrived_callback = move(callback);
}

DecoderError RemoteVideoDecoder::report_server_loss()
{
    m_server_loss_was_reported = true;
    m_server_loss_count++;
    return DecoderError::with_description(DecoderErrorCategory::NeedsResubmission, "The video decoder server was lost"sv);
}

DecoderErrorOr<u64> RemoteVideoDecoder::write_to_ring(ReadonlyBytes data)
{
    auto range_is_free = [&](u64 offset) {
        if (offset + data.size() > m_ring.size())
            return false;
        for (auto const& entry : m_state->ring_entries) {
            if (entry.ring_serial != m_ring_serial)
                continue;
            if (offset < entry.offset + entry.size && entry.offset < offset + data.size())
                return false;
        }
        return true;
    };

    Optional<u64> offset;
    {
        MutexLocker locker { m_state->mutex };
        if (m_ring.is_valid()) {
            if (range_is_free(m_ring_write_offset))
                offset = m_ring_write_offset;
            else if (range_is_free(0))
                offset = 0;
        }
    }

    if (!offset.has_value()) {
        // The frames still in the old ring keep it alive on the server until it has consumed them.
        auto ring_size = max(m_ring.size() * 2, MINIMUM_RING_SIZE);
        while (ring_size < data.size() * 4 && ring_size < MAXIMUM_RING_SIZE)
            ring_size *= 2;
        if (data.size() > ring_size)
            return DecoderError::with_description(DecoderErrorCategory::Memory, "A coded video frame is too large to submit"sv);
        m_ring = DECODER_TRY_ALLOC(Core::AnonymousBuffer::create_with_size(ring_size));
        m_ring_serial++;
        offset = 0;

        MutexLocker locker { m_state->mutex };
        m_state->connection.with_target([&](auto& connection) {
            connection.async_replace_coded_data_ring(m_state->decoder_id, m_ring);
        });
    }

    data.copy_to(Bytes { m_ring.data<u8>() + *offset, data.size() });
    m_ring_write_offset = *offset + data.size();
    return *offset;
}

DecoderErrorOr<void> RemoteVideoDecoder::receive_coded_data(CodedFrame const& coded_frame, DecodeIntent intent)
{
    if (m_reached_end_of_stream)
        return DecoderError::with_description(DecoderErrorCategory::EndOfStream, "The decoder has been drained"sv);

    {
        MutexLocker locker { m_state->mutex };
        if (m_state->connection_was_lost && !m_server_loss_was_reported)
            return report_server_loss();
    }
    if (m_server_loss_was_reported)
        TRY(start_server_decoder());

    if (auto new_codec_configuration = coded_frame.new_codec_configuration(); new_codec_configuration.has_value())
        m_codec_configuration = DECODER_TRY_ALLOC(ByteBuffer::copy(*new_codec_configuration));

    auto ring_offset = TRY(write_to_ring(coded_frame.data()));

    Optional<ByteBuffer> new_codec_configuration;
    if (coded_frame.new_codec_configuration().has_value())
        new_codec_configuration = m_codec_configuration;

    MutexLocker locker { m_state->mutex };
    m_state->frames_in_flight++;
    m_state->ring_entries.append({ m_ring_serial, ring_offset, coded_frame.data().size() });
    m_state->connection.with_target([&](auto& connection) {
        connection.async_submit_coded_frame(m_state->decoder_id, m_state->generation, ring_offset, coded_frame.data().size(), coded_frame.presentation_timestamp(), coded_frame.decode_timestamp(), coded_frame.duration(), coded_frame.flags(), move(new_codec_configuration), intent, m_output_target);
    });
    return {};
}

void RemoteVideoDecoder::signal_end_of_stream()
{
    m_reached_end_of_stream = true;
    MutexLocker locker { m_state->mutex };
    m_state->drained = false;
    m_state->connection.with_target([&](auto& connection) {
        connection.async_signal_end_of_stream(m_state->decoder_id, m_state->generation);
    });
}

DecoderErrorOr<NonnullRefPtr<VideoFrame>> RemoteVideoDecoder::take_next_output(CodingIndependentCodePoints const& container_cicp, Optional<AK::Duration> target)
{
    m_output_target = target;

    Optional<State::Output> output;
    {
        MutexLocker locker { m_state->mutex };
        while (true) {
            if (m_state->connection_was_lost) {
                if (!m_server_loss_was_reported)
                    return report_server_loss();
                // Nothing is decoding until the frames are submitted again.
                if (m_reached_end_of_stream)
                    return DecoderError::with_description(DecoderErrorCategory::EndOfStream, "The decoder has been drained"sv);
                return DecoderError::with_description(DecoderErrorCategory::NeedsMoreInput, "The decoder needs its frames submitted again"sv);
            }

            if (!m_state->outputs.is_empty()) {
                output = m_state->outputs.take_first();
                break;
            }
            if (m_state->failure.has_value())
                return m_state->failure.release_value();
            if (m_reached_end_of_stream) {
                if (m_state->drained)
                    return DecoderError::with_description(DecoderErrorCategory::EndOfStream, "The decoder has been drained"sv);
            } else if (m_state->frames_in_flight < MAXIMUM_FRAMES_IN_FLIGHT) {
                // A decoder may hold frames until it receives more, so only frames the server has yet to answer
                // are waited on.
                return DecoderError::with_description(DecoderErrorCategory::NeedsMoreInput, "The video decoder server has no frame ready"sv);
            }
            m_state->caller_is_waiting_for_output = true;
            m_state->server_decoder_state_changed.wait();
            m_state->caller_is_waiting_for_output = false;
        }
    }

    auto const& handle = output->handle;
    auto cicp = container_cicp;
    cicp.adopt_specified_values(handle.cicp);
    return DECODER_TRY_ALLOC(try_make_ref_counted<VideoFrame>(
        handle.timestamp, handle.duration, handle.size.to_type<u32>(),
        handle.bit_depth, handle.subsampling, cicp, move(output->slot)));
}

void RemoteVideoDecoder::flush()
{
    m_reached_end_of_stream = false;
    m_output_target = {};

    Vector<State::Output> outputs;
    MutexLocker locker { m_state->mutex };
    m_state->generation++;
    outputs = move(m_state->outputs);
    m_state->failure.clear();
    m_state->drained = false;
    m_state->connection.with_target([&](auto& connection) {
        connection.async_flush(m_state->decoder_id, m_state->generation);
    });
}

}
