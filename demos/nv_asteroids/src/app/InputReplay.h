#pragma once

// Recorded gamepad input used by the "-replay N" and "-benchmark" modes.
//
// Asteroids.exe: embedded in FeatureDemo at +1272 (48 bytes), constructor 0x140016DE0,
// frame accessor 0x140017C10, JSON loader 0x140018430 ("numFrames", "inputs", "buttons", "axes"),
// SetStartFrame 0x140018630, file loading in FeatureDemo 0x1400310B0 ("replay.%d.json").
// The class has no RTTI; the name is invented.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Json
{
    class Value;
}

class InputReplay
{
public:
    enum class Mode : int32_t
    {
        Off = 0,
        Recording = 1,
        Playback = 2
    };

    // One frame of gamepad input: 15 button bits and 6 axes (GLFW joystick numbering).
    struct Frame
    {
        uint16_t buttons = 0;
        uint16_t padding0 = 0;
        uint32_t padding1 = 0;
        float axes[6] = {};     // +8
    };
    static_assert(sizeof(Frame) == 32, "2018 replay frames are 32 bytes");

    static constexpr uint32_t c_NumButtons = 15;
    static constexpr uint32_t c_NumAxes = 6;

    // Returns the frame for 'frameIndex' (the global replay frame counter). Frames before the start frame,
    // and every frame when replay is off, map to a shared dummy frame. While recording the storage grows
    // (capacity at least 30000, doubled as needed); a playback that runs past its last frame stops.
    Frame& GetFrame(uint32_t frameIndex);

    // Loads a replay object ("numFrames", "inputs") and switches to playback. Returns false on a malformed file.
    bool Load(const Json::Value& root);

    // deviation: the binary contains no writer, but replay.0.json is jsoncpp output of exactly the
    // structure the loader expects ({"inputs": [{"axes": [6], "buttons": n}], "numFrames": n}); this
    // reconstructs it so that recording mode (1) is usable.
    void Save(Json::Value& root) const;

    void StartRecording(uint32_t startFrame);
    void SetStartFrame(int64_t frame) { m_StartFrame = frame; }

    Mode GetMode() const { return m_Mode; }
    int64_t GetStartFrame() const { return m_StartFrame; }
    int64_t GetNumFrames() const { return m_NumFrames; }

    bool IsPlaying() const { return m_Mode == Mode::Playback && m_StartFrame >= 0; }
    bool IsRecording() const { return m_Mode == Mode::Recording && m_StartFrame >= 0; }

private:
    Mode m_Mode = Mode::Off;        // +0
    int64_t m_StartFrame = -1;      // +8
    int64_t m_NumFrames = 0;        // +16
    std::vector<Frame> m_Frames;    // +24
};
