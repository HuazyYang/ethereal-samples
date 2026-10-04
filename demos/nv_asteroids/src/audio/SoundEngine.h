#pragma once

// Small XAudio2 sound player of the 2018 demo (namespace audio in the binary).
//
// deviation: donut main ships donut::engine::audio::Engine (voice pool, update thread,
// music cross-fading, 3D listener). The 2018 demo used a much smaller player: one
// XAudio2 instance + mastering voice behind a process-wide shared_ptr, and one source
// voice per loaded .wav (ambience music, ship jets, shield). It is reconstructed here as-is
// instead of mapping it onto donut's engine, which would change the voice lifetime model.

#include <nvrhi/core/autoptr.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <xaudio2.h>

namespace donut::vfs
{
    class IFileSystem;
}

namespace audio
{
    // Asteroids.exe: audio::Engine (RTTI std::_Ref_count_obj<audio::Engine>, accessor 0x140015C00, error sink 0x140015B10)
    class Engine
    {
    public:
        ~Engine();

        // Returns the process-wide engine, creating and initializing it on first use:
        // CoInitializeEx(MULTITHREADED), XAudio2Create, CreateMasteringVoice.
        // A failed initialization is not retried; the returned engine then has no XAudio2 objects
        // (IsInitialized() == false) and the failure is recorded in GetErrors().
        static std::shared_ptr<Engine> Get();

        bool IsInitialized() const { return m_XAudio2 != nullptr && m_MasteringVoice != nullptr; }
        IXAudio2* GetXAudio2() const { return m_XAudio2; }
        IXAudio2MasteringVoice* GetMasteringVoice() const { return m_MasteringVoice; }

        float GetMasterVolume() const;
        void SetMasterVolume(float volume);

        // 0x140016120: muting saves the master volume and sets it to 0; unmuting restores it.
        void SetMute(bool mute);

        // Errors are collected in a global list (the 2018 code never logged them directly);
        // SpaceScene prints them as "Sound engine errors : ..." after initialization.
        void AddError(const char* message);
        static const std::vector<std::string>& GetErrors();
        static void ClearErrors();

    private:
        Engine() = default;
        bool Initialize();

        float m_SavedVolume = 1.f;      // master volume saved by SetMute(true); -1 when not muted
        IXAudio2* m_XAudio2 = nullptr;
        IXAudio2MasteringVoice* m_MasteringVoice = nullptr;
    };

    // In-memory PCM .wav file (canonical RIFF/WAVE with a 'fmt ' chunk at offset 12).
    // Asteroids.exe: reader 0x140016170, data chunk lookup 0x1400160A0
    class WavFile
    {
    public:
        static std::unique_ptr<WavFile> Read(donut::vfs::IFileSystem& fs, const std::filesystem::path& path);

        WAVEFORMATEX GetFormat() const;
        const uint8_t* GetSamples() const;
        uint32_t GetSamplesSize() const;

    private:
        const uint8_t* FindDataChunk() const;
        std::vector<uint8_t> m_Data;
    };

    // One XAudio2 source voice with its submitted buffer.
    // Asteroids.exe: Sound (64 bytes: voice, XAUDIO2_BUFFER, wav data), create 0x140014EF0, destroy 0x140014DF0
    class Sound
    {
    public:
        ~Sound();
        Sound(const Sound&) = delete;
        Sound& operator=(const Sound&) = delete;

        // Loads 'path' (PCM .wav) from 'fs' and creates a source voice routed to the mastering voice.
        // When 'loop' is set the buffer loops forever (XAUDIO2_LOOP_INFINITE).
        // The voice is created stopped; call Play(). Returns null on failure (see Engine::GetErrors()).
        static std::unique_ptr<Sound> Create(nvrhi::AutoPtr<donut::vfs::IFileSystem> fs,
            const std::filesystem::path& path, bool loop);

        void Play();                        // IXAudio2SourceVoice::Start
        void Stop();                        // IXAudio2SourceVoice::Stop
        void ExitLoop();
        // Stops, flushes and re-submits the buffer so that the next Play() starts from the beginning (0x1400163F0).
        bool Rewind();
        void SetVolume(float volume);
        float GetVolume() const;
        void SetFrequencyRatio(float ratio);
        // 0x1400160F0: non-zero XAUDIO2_VOICE_STATE::SamplesPlayed. The counter restarts when the
        // END_OF_STREAM buffer finishes, so a one-shot sound reads as "not playing" once it has ended.
        bool IsPlaying() const;

        IXAudio2SourceVoice* GetVoice() const { return m_Voice; }

    private:
        Sound() = default;

        IXAudio2SourceVoice* m_Voice = nullptr;
        XAUDIO2_BUFFER m_Buffer{};
        std::unique_ptr<WavFile> m_Wav;
    };
}
