#pragma once

// Keyframe animation tracks of animations.json (namespace Animation in the binary).
//
// Asteroids.exe: Animation::AbstractTrack, Animation::Track<bool/int/float/float2/float3>
//   (vtables 0x140257E58.., slots: 0 Load, 1 GetStartTime, 2 GetEndTime),
//   track loaders 0x14002FCD0 (bool), 0x14002F1B0 (int), 0x14002F420 (float), 0x14002F690 (float2),
//   0x14002F990 (float3), evaluation 0x14002E4D0 (float; the others are the same template),
//   sequence loader 0x14002FF40, sequence constructor 0x140024620, track lookup 0x14001C3B0.
//
// deviation: class_map.md suggests mapping these onto donut::engine::animation::Sampler. The 2018 tracks
// clamp to the first key, optionally hold the last key, and use a Catmull-Rom spline whose end tangents
// repeat the boundary keys, which donut's sampler does not reproduce exactly, so the small 2018 classes are
// reconstructed as-is.

#include <donut/core/math/math.h>

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Json
{
    class Value;
}

namespace Animation
{
    enum class InterpolationMode : int
    {
        Step = 0,       // "step"
        Linear = 1,     // "linear"
        Spline = 2      // "spline" (Catmull-Rom)
    };

    class AbstractTrack
    {
    public:
        virtual ~AbstractTrack() = default;
        virtual void Load(const Json::Value& node) = 0;
        virtual float GetStartTime() const = 0;
        virtual float GetEndTime() const = 0;
    };

    template<typename T>
    class Track : public AbstractTrack
    {
    public:
        struct Keyframe
        {
            float time = 0.f;
            T value{};
        };

        void Load(const Json::Value& node) override;
        float GetStartTime() const override { return m_Keyframes.empty() ? 0.f : m_Keyframes.front().time; }
        float GetEndTime() const override { return m_Keyframes.empty() ? 0.f : m_Keyframes.back().time; }

        // Before the first key: the first value. After the last key: the last value when 'holdLastValue'
        // is set, otherwise no value. Inside: step / linear / spline interpolation of the bracketing keys.
        std::optional<T> Evaluate(float time, bool holdLastValue) const;

        const std::vector<Keyframe>& GetKeyframes() const { return m_Keyframes; }
        InterpolationMode GetMode() const { return m_Mode; }

    private:
        static T ParseValue(const Json::Value& node);

        std::vector<Keyframe> m_Keyframes;
        InterpolationMode m_Mode = InterpolationMode::Step;
    };

    // A named set of tracks ("start", "start_fast" in animations.json) plus playback state.
    // 2018 layout: unordered_map tracks (+0), startTime (+64), currentTime (+68), duration (+72).
    class Sequence
    {
    public:
        // Asteroids.exe: 0x14002FF40. 'node' is the JSON array of track descriptions.
        void Load(const Json::Value& node);

        template<typename T>
        std::optional<T> Evaluate(const std::string& trackName, std::optional<float> time, bool holdLastValue) const
        {
            auto it = m_Tracks.find(trackName);
            if (it == m_Tracks.end() || !it->second)
                return std::nullopt;

            auto track = std::dynamic_pointer_cast<Track<T>>(it->second);
            if (!track)
                return std::nullopt;

            return track->Evaluate(time.has_value() ? *time : m_CurrentTime, holdLastValue);
        }

        // Restarts the sequence on the next Animate (startTime < 0 means "not started").
        void Restart() { m_StartTime = -1.f; }

        bool IsFinished() const { return m_Duration < m_CurrentTime || m_StartTime < 0.f; }

        float GetStartTime() const { return m_StartTime; }
        float GetCurrentTime() const { return m_CurrentTime; }
        float GetDuration() const { return m_Duration; }
        void SetStartTime(float t) { m_StartTime = t; }
        void SetCurrentTime(float t) { m_CurrentTime = t; }

    private:
        std::unordered_map<std::string, std::shared_ptr<AbstractTrack>> m_Tracks;
        float m_StartTime = -1.f;
        float m_CurrentTime = 0.f;
        float m_Duration = 0.f;
    };
}
