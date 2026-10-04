#include "audio/SoundEngine.h"

#include <donut/core/vfs/VFS.h>

#include <objbase.h>

#include <cstring>

namespace audio
{
    namespace
    {
        // Process-wide engine instance (qword_1402DF190/198 in the binary, released at exit).
        std::shared_ptr<Engine> g_Engine;
        // Global error list (xmmword_1402DF178).
        std::vector<std::string> g_Errors;

        constexpr uint32_t FourCC(char a, char b, char c, char d)
        {
            return uint32_t(uint8_t(a)) | (uint32_t(uint8_t(b)) << 8) | (uint32_t(uint8_t(c)) << 16) | (uint32_t(uint8_t(d)) << 24);
        }

        constexpr uint32_t c_RiffTag = FourCC('R', 'I', 'F', 'F');
        constexpr uint32_t c_WaveTag = FourCC('W', 'A', 'V', 'E');
        constexpr uint32_t c_FmtTag = FourCC('f', 'm', 't', ' ');
        constexpr uint32_t c_DataTag = FourCC('d', 'a', 't', 'a');

        uint32_t ReadU32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
        uint16_t ReadU16(const uint8_t* p) { uint16_t v; memcpy(&v, p, 2); return v; }
    }

    // ------------------------------------------------------------------------------------------
    // Engine

    Engine::~Engine()
    {
        if (m_MasteringVoice)
        {
            m_MasteringVoice->DestroyVoice();
            m_MasteringVoice = nullptr;
        }
        if (m_XAudio2)
        {
            m_XAudio2->Release();
            m_XAudio2 = nullptr;
        }
    }

    std::shared_ptr<Engine> Engine::Get()
    {
        if (g_Engine)
            return g_Engine;

        g_Engine = std::shared_ptr<Engine>(new Engine());
        g_Engine->Initialize();
        return g_Engine;
    }

    bool Engine::Initialize()
    {
        if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)))
        {
            AddError("Error initializing multi-threaded mode");
            return false;
        }

        if (FAILED(XAudio2Create(&m_XAudio2, 0, XAUDIO2_DEFAULT_PROCESSOR)))
        {
            AddError("XAudio2Create failed");
            return false;
        }

        if (FAILED(m_XAudio2->CreateMasteringVoice(&m_MasteringVoice, XAUDIO2_DEFAULT_CHANNELS, XAUDIO2_DEFAULT_SAMPLERATE,
            0, nullptr, nullptr, AudioCategory_GameEffects)))
        {
            m_XAudio2->Release();
            m_XAudio2 = nullptr;
            AddError("CreateMasteringVoice failed");
            return false;
        }

        return true;
    }

    float Engine::GetMasterVolume() const
    {
        float volume = 0.f;
        if (m_MasteringVoice)
            m_MasteringVoice->GetVolume(&volume);
        return volume;
    }

    void Engine::SetMasterVolume(float volume)
    {
        if (m_MasteringVoice)
            m_MasteringVoice->SetVolume(volume);
    }

    void Engine::SetMute(bool mute)
    {
        if (!m_MasteringVoice)
            return;

        if (mute)
        {
            m_SavedVolume = GetMasterVolume();
            m_MasteringVoice->SetVolume(0.f);
        }
        else if (m_SavedVolume >= 0.f)
        {
            m_MasteringVoice->SetVolume(m_SavedVolume);
            m_SavedVolume = -1.f;
        }
    }

    void Engine::AddError(const char* message)
    {
        g_Errors.emplace_back(message);
    }

    const std::vector<std::string>& Engine::GetErrors()
    {
        return g_Errors;
    }

    void Engine::ClearErrors()
    {
        g_Errors.clear();
    }

    // ------------------------------------------------------------------------------------------
    // WavFile

    std::unique_ptr<WavFile> WavFile::Read(donut::vfs::IFileSystem& fs, const std::filesystem::path& path)
    {
        nvrhi::AutoPtr<nvrhi::IDataBlob> blob;
        if (NVRHI_FAILED(fs.readFile(path, &blob)) || !blob)
            return nullptr;

        auto wav = std::unique_ptr<WavFile>(new WavFile());
        wav->m_Data.resize(blob->GetSize());
        if (blob->GetSize())
            memcpy(wav->m_Data.data(), blob->GetDataPtr(), blob->GetSize());

        // The 2018 reader only accepts the canonical layout: RIFF size consistent with the file,
        // 'fmt ' as the first sub-chunk, PCM format, and a 'data' tag somewhere in the file.
        const std::vector<uint8_t>& d = wav->m_Data;
        if (d.size() < 36)
            return nullptr;
        if (ReadU32(&d[0]) != c_RiffTag || ReadU32(&d[12]) != c_FmtTag || ReadU32(&d[8]) != c_WaveTag)
            return nullptr;
        if (ReadU32(&d[4]) != d.size() - 8)
            return nullptr;
        if (ReadU32(&d[16]) < 16)
            return nullptr;
        if (ReadU16(&d[20]) != WAVE_FORMAT_PCM)
            return nullptr;
        if (!wav->FindDataChunk())
            return nullptr;

        return wav;
    }

    const uint8_t* WavFile::FindDataChunk() const
    {
        // Byte-wise scan for the 'data' tag, exactly like the binary (no sub-chunk walking).
        if (m_Data.size() < 4)
            return nullptr;
        const size_t end = m_Data.size() - 4;
        for (size_t offset = 0; offset < end; ++offset)
        {
            if (ReadU32(&m_Data[offset]) == c_DataTag)
                return &m_Data[offset];
        }
        return nullptr;
    }

    WAVEFORMATEX WavFile::GetFormat() const
    {
        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = ReadU16(&m_Data[22]);
        format.nSamplesPerSec = ReadU32(&m_Data[24]);
        format.nAvgBytesPerSec = ReadU32(&m_Data[28]);
        format.nBlockAlign = ReadU16(&m_Data[32]);
        format.wBitsPerSample = ReadU16(&m_Data[34]);
        format.cbSize = 0;
        return format;
    }

    const uint8_t* WavFile::GetSamples() const
    {
        const uint8_t* chunk = FindDataChunk();
        return chunk ? chunk + 8 : nullptr;
    }

    uint32_t WavFile::GetSamplesSize() const
    {
        const uint8_t* chunk = FindDataChunk();
        if (!chunk)
            return 0;
        // deviation: clamp the declared chunk size to the file size (the binary trusts the header).
        const size_t available = m_Data.size() - size_t(chunk + 8 - m_Data.data());
        const uint32_t declared = ReadU32(chunk + 4);
        return declared <= available ? declared : uint32_t(available);
    }

    // ------------------------------------------------------------------------------------------
    // Sound

    std::unique_ptr<Sound> Sound::Create(nvrhi::AutoPtr<donut::vfs::IFileSystem> fs,
        const std::filesystem::path& path, bool loop)
    {
        std::shared_ptr<Engine> engine = Engine::Get();
        if (!engine || !engine->IsInitialized() || !fs)
            return nullptr;

        std::unique_ptr<WavFile> wav = WavFile::Read(*fs, path);
        if (!wav)
        {
            engine->AddError(("error reading : " + path.string()).c_str());
            return nullptr;
        }

        const WAVEFORMATEX format = wav->GetFormat();

        XAUDIO2_SEND_DESCRIPTOR send{ XAUDIO2_SEND_USEFILTER, engine->GetMasteringVoice() };
        XAUDIO2_VOICE_SENDS sends{ 1, &send };

        IXAudio2SourceVoice* voice = nullptr;
        if (FAILED(engine->GetXAudio2()->CreateSourceVoice(&voice, &format, 0, XAUDIO2_DEFAULT_FREQ_RATIO,
            nullptr, &sends, nullptr)))
        {
            engine->AddError(("error CreateSourceVoice : " + path.string()).c_str());
            return nullptr;
        }

        XAUDIO2_BUFFER buffer{};
        buffer.Flags = XAUDIO2_END_OF_STREAM;
        buffer.AudioBytes = wav->GetSamplesSize();
        buffer.pAudioData = wav->GetSamples();
        buffer.LoopCount = loop ? XAUDIO2_LOOP_INFINITE : 0;

        if (FAILED(voice->SubmitSourceBuffer(&buffer, nullptr)))
        {
            engine->AddError(("error SubmitSourceBuffer : " + path.string()).c_str());
            voice->DestroyVoice();
            return nullptr;
        }

        auto sound = std::unique_ptr<Sound>(new Sound());
        sound->m_Voice = voice;
        sound->m_Buffer = buffer;
        sound->m_Wav = std::move(wav);
        return sound;
    }

    Sound::~Sound()
    {
        if (m_Voice)
        {
            m_Voice->DestroyVoice();
            m_Voice = nullptr;
        }
    }

    void Sound::Play()
    {
        if (m_Voice)
            m_Voice->Start(0);
    }

    void Sound::Stop()
    {
        if (m_Voice)
            m_Voice->Stop(0);
    }

    void Sound::ExitLoop()
    {
        if (m_Voice)
            m_Voice->ExitLoop();
    }

    bool Sound::Rewind()
    {
        if (!m_Voice)
            return false;
        m_Voice->Stop(0);
        m_Voice->FlushSourceBuffers();
        return m_Voice->SubmitSourceBuffer(&m_Buffer, nullptr) == S_OK;
    }

    void Sound::SetVolume(float volume)
    {
        if (m_Voice)
            m_Voice->SetVolume(volume);
    }

    float Sound::GetVolume() const
    {
        float volume = 0.f;
        if (m_Voice)
            m_Voice->GetVolume(&volume);
        return volume;
    }

    void Sound::SetFrequencyRatio(float ratio)
    {
        if (m_Voice)
            m_Voice->SetFrequencyRatio(ratio);
    }

    bool Sound::IsPlaying() const
    {
        if (!m_Voice)
            return false;
        XAUDIO2_VOICE_STATE state{};
        m_Voice->GetState(&state, 0);
        return state.SamplesPlayed != 0;
    }
}
