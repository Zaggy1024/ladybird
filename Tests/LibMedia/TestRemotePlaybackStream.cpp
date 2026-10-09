/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "FakeDeviceStream.h"
#include <AK/Atomic.h>
#include <AK/Time.h>
#include <LibCore/EventLoop.h>
#include <LibIPC/Transport.h>
#include <LibMedia/Audio/AudioDevices.h>
#include <LibMedia/Audio/CaptureDevices.h>
#include <LibMedia/Audio/ClientConnection.h>
#include <LibMedia/Audio/PlaybackStreamMixer.h>
#include <LibMedia/Audio/RemotePlaybackStream.h>
#include <LibMedia/Audio/ServerConnection.h>
#include <LibMedia/Sinks/AudioPlaybackSink.h>
#include <LibTest/TestCase.h>

static constexpr u32 CHANNEL_COUNT = FakeDeviceStream::CHANNEL_COUNT;

// What the data request callback hands out. Declared before the fixture, since the pump may still be in the callback
// when a test drops its stream.
struct Source {
    Atomic<bool> starved { false };
    Atomic<u64> requests { 0 };
    Atomic<u64> frames_supplied { 0 };
    // How far ahead of the request the latest buffer was said to start playing.
    Atomic<i64> latest_lead_nanoseconds { 0 };
    float value { 1.0f };
    // Runs on the pump thread inside a starved callback, before it returns empty.
    Function<void()> on_starved_request;

    Audio::PlaybackStream::AudioDataRequestCallback callback()
    {
        return [this](Span<float> buffer, MonotonicTime buffer_starts_playing_at) -> ReadonlySpan<float> {
            requests++;
            latest_lead_nanoseconds = (buffer_starts_playing_at - MonotonicTime::now()).to_nanoseconds();
            if (starved.load()) {
                if (on_starved_request)
                    on_starved_request();
                return {};
            }
            buffer.fill(value);
            frames_supplied += buffer.size() / CHANNEL_COUNT;
            return buffer;
        };
    }
};

// A mixer on a fake device, a server connection serving it, and a client connection on the other end of a paired
// transport, all on one event loop.
struct RemoteFixture {
    static constexpr u32 TARGET_LATENCY_MS = 100;
    static constexpr u32 DEVICE_LATENCY_MS = 40;
    // What the client keeps in its ring: the budget less the device's share.
    static constexpr u32 RING_LATENCY_MS = TARGET_LATENCY_MS - DEVICE_LATENCY_MS;

    Core::EventLoop loop;
    RefPtr<FakeDeviceStream> device;
    RefPtr<Audio::PlaybackStreamMixer> mixer;
    RefPtr<Audio::ServerConnection> server;
    RefPtr<Audio::ClientConnection> client;
    bool server_died { false };

    RemoteFixture()
    {
        mixer = Audio::PlaybackStreamMixer::create(loop, DEVICE_LATENCY_MS, [this](Audio::OutputState state, u32, Audio::PlaybackStream::AudioDataRequestCallback callback) {
            device = make_ref_counted<FakeDeviceStream>(state, move(callback));
            auto promise = Audio::PlaybackStream::CreatePromise::construct();
            promise->resolve(*device);
            return promise;
        });
        auto paired = MUST(IPC::Transport::create_paired());
        server = Audio::ServerConnection::construct(move(paired.local), 1, *mixer, Audio::ServerConnection::DeviceEnumeration::Platform, Audio::CaptureDevices::create([](Audio::SampleSpecification const&, u32, StringView, Audio::RecordStream::RecordCallback) { return Audio::RecordStream::CreatePromise::rejected(Error::from_string_literal("No capture in this test")); }));
        server->on_death = [this] { server_died = true; };
        client = adopt_ref(*new Audio::ClientConnection(MUST(paired.remote_handle.create_transport())));
    }

    // The client goes first, and the loop runs until the server has seen it go: a connection's shutdown is deferred
    // on the thread's event queue with a reference to it, so it must not outlive the fixture.
    ~RemoteFixture()
    {
        client = nullptr;
        if (server)
            EXPECT(pump_until([&] { return server_died; }));
        pump_until([] { return false; }, AK::Duration::from_milliseconds(10));
    }

    template<typename Condition>
    bool pump_until(Condition condition, AK::Duration timeout = AK::Duration::from_seconds(5))
    {
        auto deadline = MonotonicTime::now() + timeout;
        while (!condition()) {
            if (MonotonicTime::now() > deadline)
                return false;
            loop.pump(Core::EventLoop::WaitMode::PollForEvents);
        }
        return true;
    }

    RefPtr<Audio::PlaybackStream> create_stream(Audio::OutputState initial_state, Source& source)
    {
        RefPtr<Audio::PlaybackStream> stream;
        bool rejected = false;
        auto promise = client->create_stream(initial_state, TARGET_LATENCY_MS, source.callback());
        promise->when_resolved([&](NonnullRefPtr<Audio::PlaybackStream>& created) { stream = created; });
        promise->when_rejected([&](Error&) { rejected = true; });
        EXPECT(pump_until([&] { return stream || rejected; }));
        EXPECT(!rejected);
        return stream;
    }

    // Keeps the fake device rendering while waiting, the way a real device would.
    template<typename Condition>
    bool render_until(Condition condition, size_t frames_per_render = 480)
    {
        return pump_until([&] {
            if (device->is_playing())
                (void)device->render(frames_per_render);
            return condition();
        });
    }

    // Renders until the pump has filled a whole buffer with the expected value; the pump pushes into the ring some
    // time after the data request callback returns.
    bool render_until_full_buffer_of(float expected, size_t frames_per_render = 480)
    {
        return pump_until([&] {
            if (!device->is_playing())
                return false;
            auto output = device->render(frames_per_render);
            return all_of(output, [&](float sample) { return sample == expected; });
        });
    }
};

TEST_CASE(stream_is_created_with_the_device_format)
{
    Source source;
    RemoteFixture fixture;
    auto stream = fixture.create_stream(Audio::OutputState::Suspended, source);
    EXPECT(stream);
    EXPECT_EQ(stream->sample_specification().sample_rate(), FakeDeviceStream::SAMPLE_RATE);
    EXPECT_EQ(stream->sample_specification().channel_count(), CHANNEL_COUNT);
    // Nothing is attached to the mixer until the stream plays.
    EXPECT(!fixture.mixer->has_clients());
    EXPECT_EQ(source.requests.load(), 0u);
}

TEST_CASE(resume_attaches_a_ring_that_the_pump_fills)
{
    Source source;
    RemoteFixture fixture;
    auto stream = fixture.create_stream(Audio::OutputState::Suspended, source);

    bool resumed = false;
    stream->resume()->when_resolved([&] { resumed = true; }).when_rejected([](Error&&) {});
    EXPECT(fixture.pump_until([&] { return resumed && fixture.mixer->has_clients() && fixture.device->is_playing(); }));
    EXPECT(fixture.render_until_full_buffer_of(1.0f));
}

TEST_CASE(pump_dates_each_buffer_by_what_the_ring_still_holds)
{
    Source source;
    RemoteFixture fixture;
    auto stream = fixture.create_stream(Audio::OutputState::Suspended, source);
    (void)stream->resume();

    // The first buffer fills the empty ring up to its share of the budget, so it plays at once.
    auto target_frames = AK::Duration::from_milliseconds(RemoteFixture::RING_LATENCY_MS).to_time_units(1, FakeDeviceStream::SAMPLE_RATE);
    EXPECT(fixture.pump_until([&] { return source.frames_supplied.load() >= static_cast<u64>(target_frames); }));
    EXPECT(source.latest_lead_nanoseconds.load() < AK::Duration::from_milliseconds(30).to_nanoseconds());
    auto requests_before_consuming = source.requests.load();

    // Once the device has taken one buffer, its replacement plays behind what the ring still holds: most of the
    // ring's share, and never more than the whole of it.
    EXPECT(fixture.render_until_full_buffer_of(1.0f));
    EXPECT(fixture.pump_until([&] { return source.requests.load() > requests_before_consuming; }));
    auto lead = AK::Duration::from_nanoseconds(source.latest_lead_nanoseconds.load());
    EXPECT(lead >= AK::Duration::from_milliseconds(30));
    EXPECT(lead <= AK::Duration::from_milliseconds(RemoteFixture::RING_LATENCY_MS + 10));
}

TEST_CASE(resume_after_a_drain_plays_again)
{
    Source source;
    RemoteFixture fixture;
    auto stream = fixture.create_stream(Audio::OutputState::Suspended, source);
    (void)stream->resume();
    EXPECT(fixture.render_until_full_buffer_of(1.0f));
    bool drained = false;
    stream->drain_buffer_and_suspend()->when_resolved([&] { drained = true; }).when_rejected([](Error&&) {});
    EXPECT(fixture.render_until([&] { return drained; }));

    bool resumed = false;
    stream->resume()->when_resolved([&] { resumed = true; }).when_rejected([](Error&&) {});
    EXPECT(fixture.render_until_full_buffer_of(1.0f));
    EXPECT(fixture.pump_until([&] { return resumed; }));
}

TEST_CASE(stream_created_playing_starts_at_once)
{
    Source source;
    RemoteFixture fixture;
    auto stream = fixture.create_stream(Audio::OutputState::Playing, source);
    EXPECT(fixture.pump_until([&] { return fixture.device->is_playing() && source.frames_supplied.load() > 0; }));
}

TEST_CASE(drain_completes_once_the_device_has_played_the_ring_out)
{
    Source source;
    RemoteFixture fixture;
    auto stream = fixture.create_stream(Audio::OutputState::Suspended, source);
    (void)stream->resume();
    EXPECT(fixture.pump_until([&] { return source.frames_supplied.load() >= 480; }));

    // Stop supplying, then drain: the request completes only after the fake has rendered everything.
    source.starved = true;
    bool drained = false;
    stream->drain_buffer_and_suspend()->when_resolved([&] { drained = true; }).when_rejected([](Error&&) {});
    fixture.loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    EXPECT(!drained);
    EXPECT(fixture.render_until([&] { return drained; }));
    EXPECT(fixture.pump_until([&] { return !fixture.device->is_playing(); }));
}

TEST_CASE(discard_drops_the_ring_and_suspends)
{
    Source source;
    RemoteFixture fixture;
    auto stream = fixture.create_stream(Audio::OutputState::Suspended, source);
    (void)stream->resume();
    EXPECT(fixture.pump_until([&] { return source.frames_supplied.load() >= 480; }));

    bool discarded = false;
    stream->discard_buffer_and_suspend()->when_resolved([&] { discarded = true; }).when_rejected([](Error&&) {});
    EXPECT(fixture.pump_until([&] { return discarded && !fixture.device->is_playing(); }));
    auto requests_after_discard = source.requests.load();

    // Nothing plays and the pump has stopped asking.
    auto output = fixture.device->render(480);
    for (auto sample : output)
        EXPECT_EQ(sample, 0.0f);
    fixture.pump_until([&] { return false; }, AK::Duration::from_milliseconds(100));
    EXPECT_EQ(source.requests.load(), requests_after_discard);
}

TEST_CASE(a_starved_stream_waits_for_notify_data_available)
{
    Source source;
    RemoteFixture fixture;
    source.starved = true;
    auto stream = fixture.create_stream(Audio::OutputState::Suspended, source);
    (void)stream->resume();
    EXPECT(fixture.pump_until([&] { return source.requests.load() >= 1; }));
    auto requests_while_starved = source.requests.load();

    // The pump parks after the empty return: no further requests over several periods.
    fixture.pump_until([&] { return false; }, AK::Duration::from_milliseconds(150));
    EXPECT_EQ(source.requests.load(), requests_while_starved);

    source.starved = false;
    stream->notify_data_available();
    EXPECT(fixture.pump_until([&] { return source.frames_supplied.load() > 0; }));
}

TEST_CASE(a_notification_during_a_starved_request_is_not_lost)
{
    Source source;
    source.starved = true;
    RemoteFixture fixture;
    auto stream = fixture.create_stream(Audio::OutputState::Suspended, source);
    // Data arrives while the pump is still inside the request that finds nothing: the pump must ask again.
    source.on_starved_request = [&] {
        source.starved = false;
        stream->notify_data_available();
    };
    (void)stream->resume();
    EXPECT(fixture.pump_until([&] { return source.frames_supplied.load() > 0; }));
}

TEST_CASE(volume_reaches_the_mix)
{
    Source source;
    RemoteFixture fixture;
    auto stream = fixture.create_stream(Audio::OutputState::Suspended, source);
    (void)stream->set_volume(0.25);
    (void)stream->resume();
    EXPECT(fixture.render_until_full_buffer_of(0.25f));
}

TEST_CASE(losing_the_connection_reports_output_lost_and_settles_requests)
{
    Source source;
    RemoteFixture fixture;
    auto stream = fixture.create_stream(Audio::OutputState::Suspended, source);
    (void)stream->resume();
    EXPECT(fixture.pump_until([&] { return source.frames_supplied.load() >= 480; }));

    bool output_lost = false;
    stream->on_output_lost = [&] { output_lost = true; };

    // Dropping the server end closes the transport; the client notices and tells its streams.
    fixture.server = nullptr;
    EXPECT(fixture.pump_until([&] { return output_lost; }));

    bool drained = false;
    stream->drain_buffer_and_suspend()->when_resolved([&] { drained = true; }).when_rejected([](Error&&) {});
    EXPECT(fixture.pump_until([&] { return drained; }));
    auto requests_at_loss = source.requests.load();
    fixture.pump_until([&] { return false; }, AK::Duration::from_milliseconds(50));
    EXPECT_EQ(source.requests.load(), requests_at_loss);
}

TEST_CASE(watching_devices_reports_the_servers_list_at_once)
{
    RemoteFixture fixture;
    auto& devices = Audio::AudioDevices::the();
    EXPECT(fixture.pump_until([&] { return devices.has_device_list(); }));

    Optional<Audio::AudioDeviceEnumeration> enumeration;
    fixture.client->watch_devices([&](ErrorOr<Audio::AudioDeviceEnumeration> result) { enumeration = result.release_value(); });
    EXPECT(fixture.pump_until([&] { return enumeration.has_value(); }));
    EXPECT_EQ(enumeration->inputs.size(), devices.input_devices().size());
    EXPECT_EQ(enumeration->outputs.size(), devices.output_devices().size());
}

TEST_CASE(a_device_change_on_the_server_reaches_a_watching_client)
{
    RemoteFixture fixture;
    auto& devices = Audio::AudioDevices::the();
    EXPECT(fixture.pump_until([&] { return devices.has_device_list(); }));
    Audio::AudioDeviceEnumeration original { .inputs = devices.input_devices(), .outputs = devices.output_devices() };

    size_t reports = 0;
    Vector<Audio::AudioDeviceInfo> last_inputs;
    fixture.client->watch_devices([&](ErrorOr<Audio::AudioDeviceEnumeration> result) {
        reports++;
        last_inputs = result.release_value().inputs;
    });
    EXPECT(fixture.pump_until([&] { return reports == 1; }));

    // The source reports a new device, and the client hears the list again.
    Audio::AudioDeviceInfo microphone { .dom_device_id = "test:input:1", .label = "Test microphone", .group_id = {}, .sample_rate_hz = 48000, .channel_count = 1, .is_default = true };
    devices.report_device_list(Audio::AudioDeviceEnumeration { .inputs = { microphone }, .outputs = {} });
    EXPECT(fixture.pump_until([&] { return reports == 2; }));
    EXPECT_EQ(last_inputs.size(), 1u);
    EXPECT_EQ(last_inputs[0].label, "Test microphone"sv);

    devices.report_device_list(move(original));
}

TEST_CASE(losing_the_connection_reports_an_error_to_the_watcher)
{
    RemoteFixture fixture;
    bool errored = false;
    fixture.client->watch_devices([&](ErrorOr<Audio::AudioDeviceEnumeration> result) {
        if (result.is_error())
            errored = true;
    });

    fixture.server = nullptr;
    EXPECT(fixture.pump_until([&] { return errored; }));
}

TEST_CASE(destroying_a_stream_removes_it_from_the_mixer)
{
    Source source;
    RemoteFixture fixture;
    auto stream = fixture.create_stream(Audio::OutputState::Suspended, source);
    (void)stream->resume();
    EXPECT(fixture.pump_until([&] { return fixture.mixer->has_clients(); }));
    stream = nullptr;
    EXPECT(fixture.pump_until([&] { return !fixture.mixer->has_clients() && !fixture.device->is_playing(); }));
}

// A mixer on a fake device whose server connections are handed out through the process-wide transport factory, the
// way a sink in MediaServer reaches the AudioServer. Each request gets a fresh server end on the same mixer.
struct RemoteSinkFixture {
    Core::EventLoop loop;
    RefPtr<FakeDeviceStream> device;
    RefPtr<Audio::PlaybackStreamMixer> mixer;
    Vector<NonnullRefPtr<Audio::ServerConnection>> servers;
    size_t connections_requested { 0 };
    bool accepting_connections { true };

    RemoteSinkFixture()
    {
        mixer = Audio::PlaybackStreamMixer::create(loop, 100, [this](Audio::OutputState state, u32, Audio::PlaybackStream::AudioDataRequestCallback callback) {
            device = make_ref_counted<FakeDeviceStream>(state, move(callback));
            auto promise = Audio::PlaybackStream::CreatePromise::construct();
            promise->resolve(*device);
            return promise;
        });
        Audio::ClientConnection::set_transport_factory([this]() -> ErrorOr<NonnullOwnPtr<IPC::Transport>> {
            connections_requested++;
            if (!accepting_connections)
                return Error::from_string_literal("The fixture is no longer accepting connections");
            auto paired = TRY(IPC::Transport::create_paired());
            servers.append(Audio::ServerConnection::construct(move(paired.local), static_cast<int>(connections_requested), *mixer, Audio::ServerConnection::DeviceEnumeration::Platform, Audio::CaptureDevices::create([](Audio::SampleSpecification const&, u32, StringView, Audio::RecordStream::RecordCallback) { return Audio::RecordStream::CreatePromise::rejected(Error::from_string_literal("No capture in this test")); })));
            return paired.remote_handle.create_transport();
        });
    }

    ~RemoteSinkFixture()
    {
        Audio::ClientConnection::set_transport_factory({});
    }

    template<typename Condition>
    bool pump_until(Condition condition, AK::Duration timeout = AK::Duration::from_seconds(5))
    {
        auto deadline = MonotonicTime::now() + timeout;
        while (!condition()) {
            if (MonotonicTime::now() > deadline)
                return false;
            loop.pump(Core::EventLoop::WaitMode::PollForEvents);
        }
        return true;
    }

    // Closing every server end makes the client connection die. Done with the sink still alive, so that its reopen
    // asks for a connection and is refused: the sign that the client is gone, before the loop is.
    void close_connections_and_wait_for_the_client_to_die()
    {
        accepting_connections = false;
        auto requests_before = connections_requested;
        servers.clear();
        EXPECT(pump_until([&] { return connections_requested > requests_before; }));
        pump_until([] { return false; }, AK::Duration::from_milliseconds(10));
    }
};

TEST_CASE(audio_playback_sink_reopens_its_output_after_loss_with_a_seek_drain_in_flight)
{
    RemoteSinkFixture fixture;
    RefPtr<Media::AudioPlaybackSink> sink = MUST(Media::AudioPlaybackSink::try_create([](Media::PipelineStatus) { }, Media::AudioOutput::Platform));
    sink->start();
    sink->resume();
    EXPECT(fixture.pump_until([&] { return fixture.mixer->active_client_count() == 1; }));
    EXPECT_EQ(fixture.connections_requested, 1u);

    // The drain for the seek completes only once the device plays the ring out, which the fake never does.
    sink->seek(AK::Duration::from_seconds(1));
    fixture.pump_until([] { return false; }, AK::Duration::from_milliseconds(10));
    EXPECT(fixture.device->is_playing());

    // The server goes away with that drain in flight: the sink hears of the loss, then of the drain's completion.
    fixture.servers.clear();
    EXPECT(fixture.pump_until([&] { return fixture.connections_requested == 2; }));

    // The new stream seeks to the pending target and plays on.
    EXPECT(fixture.pump_until([&] { return fixture.mixer->active_client_count() == 1 && fixture.servers.size() == 1; }));
    EXPECT(fixture.pump_until([&] { return sink->time_reader().current_time() >= AK::Duration::from_seconds(1); }));

    fixture.close_connections_and_wait_for_the_client_to_die();
    sink = nullptr;
    fixture.pump_until([] { return false; }, AK::Duration::from_milliseconds(10));
}
