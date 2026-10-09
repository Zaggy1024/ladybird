/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefPtr.h>
#include <AK/Types.h>
#include <AudioServer/CaptureDevices.h>
#include <AudioServer/PlaybackStreamMixer.h>
#include <AudioServer/ServerConnection.h>
#include <LibAudio/AudioOutput.h>
#include <LibIPC/TransportHandle.h>

namespace AudioServer {

// The tabs with connected client processes: each has a mixer, and the Browser's word on whether it may capture.
class Tabs {
public:
    static Tabs& the();

    // Decides what every tab's device stream is; headless instances discard their mix into a null stream and have no
    // devices to capture from.
    void set_audio_output(Audio::AudioOutput audio_output) { m_audio_output = audio_output; }

    ErrorOr<IPC::TransportHandle> connect_client(u64 tab_id);
    // The Browser has let the tab's pages capture: every client of the tab, present and future, may open capture
    // streams.
    void allow_capture(u64 tab_id);

private:
    struct Tab {
        NonnullRefPtr<Audio::PlaybackStreamMixer> mixer;
        HashMap<int, NonnullRefPtr<Audio::ServerConnection>> connections;
    };

    Tab& tab_for(u64 tab_id);
    Audio::CaptureDevices& capture_devices();
    void connection_died(u64 tab_id, int client_id);

    HashMap<u64, Tab> m_tabs;
    HashTable<u64> m_capture_allowed_tabs;
    RefPtr<Audio::CaptureDevices> m_capture_devices;
    int m_next_client_id { 1 };
    Audio::AudioOutput m_audio_output { Audio::AudioOutput::Platform };
};

}
