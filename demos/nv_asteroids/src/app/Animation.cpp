#include "app/Animation.h"

#include <json/json.h>

#include <algorithm>

using namespace donut::math;

namespace Animation
{
    namespace
    {
        InterpolationMode ParseMode(const Json::Value& node, InterpolationMode current)
        {
            const Json::Value& mode = node["mode"];
            if (!mode.isString())
                return current;

            const std::string s = mode.asString();
            if (s == "step")
                return InterpolationMode::Step;
            if (s == "linear")
                return InterpolationMode::Linear;
            if (s == "spline")
                return InterpolationMode::Spline;
            return current;
        }

        // Interpolation between p1 and p2 with neighbours p0 and p3 (0x14002E4D0).
        template<typename T>
        T Interpolate(InterpolationMode mode, const T& p0, const T& p1, const T& p2, const T& p3, float t)
        {
            switch (mode)
            {
            case InterpolationMode::Linear:
                return (p2 - p1) * t + p1;
            case InterpolationMode::Spline:
                return ((((p1 * 3.f - p0 - p2 * 3.f + p3) * t + ((p0 + p0) - p1 * 5.f + p2 * 4.f - p3)) * t + (p2 - p0)) * 0.5f) * t + p1;
            case InterpolationMode::Step:
            default:
                return p1;
            }
        }

        // unresolved: bool and int tracks use the same template in the binary; the arithmetic forms make
        // no sense for them and no shipped animation uses them, so they always step.
        template<>
        bool Interpolate<bool>(InterpolationMode, const bool&, const bool& p1, const bool&, const bool&, float)
        {
            return p1;
        }

        template<>
        int Interpolate<int>(InterpolationMode, const int&, const int& p1, const int&, const int&, float)
        {
            return p1;
        }
    }

    // Value parsing, one per track type (0x14002FCD0, 0x14002F1B0, 0x14002F420, 0x14002F690, 0x14002F990).

    template<>
    bool Track<bool>::ParseValue(const Json::Value& node)
    {
        return node.isBool() ? node.asBool() : false;
    }

    template<>
    int Track<int>::ParseValue(const Json::Value& node)
    {
        return node.isInt() ? node.asInt() : 0;
    }

    template<>
    float Track<float>::ParseValue(const Json::Value& node)
    {
        return node.isNumeric() ? node.asFloat() : 0.f;
    }

    template<>
    float2 Track<float2>::ParseValue(const Json::Value& node)
    {
        if (node.isArray() && node.size() == 2)
            return float2(node[0].asFloat(), node[1].asFloat());
        if (node.isNumeric())
            return float2(node.asFloat());
        return float2(0.f);
    }

    template<>
    float3 Track<float3>::ParseValue(const Json::Value& node)
    {
        if (node.isArray() && node.size() == 3)
            return float3(node[0].asFloat(), node[1].asFloat(), node[2].asFloat());
        if (node.isNumeric())
            return float3(node.asFloat());
        return float3(0.f);
    }

    template<typename T>
    void Track<T>::Load(const Json::Value& node)
    {
        m_Mode = ParseMode(node, m_Mode);

        const Json::Value& values = node["values"];
        if (!values.isArray())
            return;

        for (const Json::Value& key : values)
        {
            Keyframe keyframe;
            keyframe.time = key["time"].asFloat();
            keyframe.value = ParseValue(key["value"]);
            m_Keyframes.push_back(keyframe);
        }

        std::sort(m_Keyframes.begin(), m_Keyframes.end(),
            [](const Keyframe& a, const Keyframe& b) { return a.time < b.time; });
    }

    template<typename T>
    std::optional<T> Track<T>::Evaluate(float time, bool holdLastValue) const
    {
        const size_t count = m_Keyframes.size();
        if (count == 0)
            return std::nullopt;

        if (m_Keyframes.front().time >= time)
            return m_Keyframes.front().value;

        if (count == 1 || time >= m_Keyframes.back().time)
        {
            if (holdLastValue)
                return m_Keyframes.back().value;
            return std::nullopt;
        }

        for (size_t i = 0; i < count; i++)
        {
            const Keyframe& k1 = m_Keyframes[i];
            if (time < k1.time)
                continue;

            const Keyframe& k2 = m_Keyframes[i + 1];
            if (k2.time <= time)
                continue;

            const T& p0 = (i != 0) ? m_Keyframes[i - 1].value : k1.value;
            const T& p3 = (i < count - 2) ? m_Keyframes[i + 2].value : k2.value;
            const float t = (time - k1.time) / (k2.time - k1.time);

            return Interpolate<T>(m_Mode, p0, k1.value, k2.value, p3, t);
        }

        return std::nullopt;
    }

    template class Track<bool>;
    template class Track<int>;
    template class Track<float>;
    template class Track<float2>;
    template class Track<float3>;

    void Sequence::Load(const Json::Value& node)
    {
        m_Duration = 0.f;

        for (const Json::Value& trackNode : node)
        {
            const std::string type = trackNode["type"].asString();
            const std::string name = trackNode["name"].asString();

            std::shared_ptr<AbstractTrack> track;
            if (type == "bool")
                track = std::make_shared<Track<bool>>();
            else if (type == "int")
                track = std::make_shared<Track<int>>();
            else if (type == "float")
                track = std::make_shared<Track<float>>();
            else if (type == "float2")
                track = std::make_shared<Track<float2>>();
            else if (type == "float3")
                track = std::make_shared<Track<float3>>();
            else
                continue;

            track->Load(trackNode);
            m_Tracks[name] = track;
            m_Duration = std::max(m_Duration, track->GetEndTime());
        }
    }
}
