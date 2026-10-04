#include "app/InputReplay.h"

#include <json/json.h>

#include <algorithm>

InputReplay::Frame& InputReplay::GetFrame(uint32_t frameIndex)
{
    // Function-local static in the binary (guarded by dword_1402DF1C8).
    static Frame s_DummyFrame;

    if (m_Mode == Mode::Off || m_StartFrame < 0)
        return s_DummyFrame;

    const int64_t index = int64_t(frameIndex) - m_StartFrame;
    if (index < 0)
        return s_DummyFrame;

    if (index >= m_NumFrames)
    {
        if (m_Mode != Mode::Recording)
        {
            if (m_Mode == Mode::Playback)
                m_StartFrame = -1; // end of the recorded stream
            return s_DummyFrame;
        }

        uint64_t capacity = std::max<uint64_t>(30000, m_Frames.size());
        // deviation: the binary loops while (capacity < index), which leaves no slot for index == capacity.
        while (capacity <= uint64_t(index))
            capacity *= 2;
        m_Frames.resize(size_t(capacity));

        // unresolved: the binary stores the index, not index + 1, so the frame being recorded is not
        // counted until the next one arrives. Kept as-is.
        m_NumFrames = index;
    }

    return m_Frames[size_t(index)];
}

bool InputReplay::Load(const Json::Value& root)
{
    const Json::Value& numFrames = root["numFrames"];
    if (!numFrames.isNumeric())
        return false;
    m_NumFrames = numFrames.asInt();

    const Json::Value& inputs = root["inputs"];
    if (!inputs.isArray())
        return false;
    if (int64_t(inputs.size()) != m_NumFrames)
        return false;

    m_Frames.resize(size_t(m_NumFrames));

    for (Json::ArrayIndex i = 0; int64_t(i) < m_NumFrames; i++)
    {
        Frame& frame = m_Frames[i];
        const Json::Value& input = inputs[i];

        const Json::Value& buttons = input["buttons"];
        frame.buttons = buttons.isNumeric() ? uint16_t(buttons.asInt()) : 0;

        const Json::Value& axes = input["axes"];
        if (axes.isArray() && axes.size() == c_NumAxes)
        {
            for (Json::ArrayIndex axis = 0; axis < c_NumAxes; axis++)
                frame.axes[axis] = axes[axis].asFloat();
        }
    }

    m_Mode = Mode::Playback;
    return true;
}

void InputReplay::Save(Json::Value& root) const
{
    Json::Value inputs(Json::arrayValue);
    const int64_t count = std::min<int64_t>(m_NumFrames, int64_t(m_Frames.size()));
    for (int64_t i = 0; i < count; i++)
    {
        const Frame& frame = m_Frames[size_t(i)];

        Json::Value input(Json::objectValue);
        Json::Value axes(Json::arrayValue);
        for (uint32_t axis = 0; axis < c_NumAxes; axis++)
            axes.append(double(frame.axes[axis]));
        input["axes"] = axes;
        input["buttons"] = int(frame.buttons);
        inputs.append(input);
    }

    root = Json::Value(Json::objectValue);
    root["inputs"] = inputs;
    root["numFrames"] = Json::Int64(count);
}

void InputReplay::StartRecording(uint32_t startFrame)
{
    m_Mode = Mode::Recording;
    m_StartFrame = startFrame;
    m_NumFrames = 0;
    m_Frames.clear();
}
