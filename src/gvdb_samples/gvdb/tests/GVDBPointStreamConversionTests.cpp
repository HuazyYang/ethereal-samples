// Independent host encoding and whole-buffer guards for the raw stream adapter.
// SPDX-License-Identifier: Apache-2.0
#include "../GVDBPointStreamConversion.h"
#include "ExampleDevice.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>

namespace g = gvdb::portable;
using ethereal::slang::ComputeRuntime;
namespace {
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
void check(nvrhi::FRESULT result, const char* message) { require(NVRHI_SUCCEEDED(result), message); }
template <class T>
T read(const uint8_t* p) {
    T value;
    std::memcpy(&value, p, sizeof(value));
    return value;
}
template <class T>
void write(uint8_t* p, T value) {
    std::memcpy(p, &value, sizeof(value));
}
size_t padded(size_t bytes) { return (bytes + 3) & ~size_t(3); }
float decode(const uint8_t* p, g::PointComponent format) {
    switch (format) {
        case g::PointComponent::UInt8:
            return *p;
        case g::PointComponent::UInt16:
            return read<uint16_t>(p);
        case g::PointComponent::Float32:
            return read<float>(p);
        case g::PointComponent::Float64:
            return float(read<double>(p));
        default:
            throw std::runtime_error("fixture format");
    }
}
void encode(uint8_t* p, g::PointComponent format, float value) {
    switch (format) {
        case g::PointComponent::UInt8:
            *p = std::isfinite(value) ? uint8_t(std::clamp(value, 0.f, 255.f)) : 0;
            break;
        case g::PointComponent::UInt16:
            write(p, std::isfinite(value) ? uint16_t(std::clamp(value, 0.f, 65535.f)) : uint16_t(0));
            break;
        case g::PointComponent::Float32:
            write(p, value);
            break;
        case g::PointComponent::Float64:
            write(p, double(value));
            break;
        default:
            throw std::runtime_error("fixture format");
    }
}
float lane(g::Float4 v, uint32_t i) { return i == 0 ? v.x : i == 1 ? v.y : i == 2 ? v.z : v.w; }
float transform(float value, const g::PointStreamTransform& t, uint32_t c) {
    const float minimum = lane(t.minimum, c), delta = lane(t.delta, c);
    const float scale = lane(t.scale, c), translation = lane(t.translation, c);
    if (minimum == 0 && delta == 1 && scale == 1 && translation == 0)
        return value;
    volatile float a = value * delta;
    volatile float b = minimum + a;
    volatile float d = b * scale;
    return d + translation;
}
g::PointStream stream(nvrhi::IBuffer* buffer, uint32_t offset, uint32_t stride, uint32_t count,
                      g::PointComponent component, uint32_t components = 3) {
    g::PointStream result;
    result.buffer = buffer;
    result.byteOffset = offset;
    result.byteStride = stride;
    result.count = count;
    result.component = component;
    result.components = components;
    return result;
}
void reference(std::vector<uint8_t>& expected, const std::vector<uint8_t>& source, const g::PointStream& src,
               const g::PointStream& dst, const g::PointStreamTransform& t) {
    for (uint32_t i = 0; i < src.count; ++i)
        for (uint32_t c = 0; c < src.components; ++c) {
            float value = decode(
                source.data() + src.byteOffset + i * src.byteStride + c * g::componentBytes(src.component),
                src.component);
            encode(expected.data() + dst.byteOffset + i * dst.byteStride + c * g::componentBytes(dst.component),
                   dst.component, transform(value, t, c));
        }
}
void launch(ComputeRuntime& runtime, const g::PointStreamConversion& conversion) {
    if (!conversion.groupsX)
        return;
    runtime.dispatch("GVDBPointStreamConversion", "convertPointStream", &conversion.parameters,
                     sizeof(conversion.parameters),
                     {nvrhi::BindingSetItem::RawBuffer_SRV(0, conversion.source),
                      nvrhi::BindingSetItem::RawBuffer_UAV(0, conversion.destination)},
                     conversion.groupsX, conversion.groupsY);
}
void compare(ComputeRuntime& runtime, nvrhi::IBuffer* output, const std::vector<uint8_t>& expected,
             const char* label) {
    std::vector<uint8_t> actual(expected.size());
    runtime.read(output, actual.data(), actual.size());
    if (actual != expected) {
        for (size_t i = 0; i < actual.size(); ++i)
            if (actual[i] != expected[i]) {
                std::fprintf(stderr, "%s byte %zu: got %u expected %u\n", label, i, actual[i], expected[i]);
                break;
            }
        throw std::runtime_error(label);
    }
}
void formatMatrix(ComputeRuntime& runtime) {
    const std::array<g::PointComponent, 4> formats = {g::PointComponent::UInt8, g::PointComponent::UInt16,
                                                      g::PointComponent::Float32, g::PointComponent::Float64};
    uint32_t number = 0;
    for (auto input : formats)
        for (auto output : formats) {
            constexpr uint32_t count = 257, components = 3;
            const uint32_t sourceOffset = 1 + number % 3, destinationOffset = 1 + (number + 1) % 3;
            const uint32_t sourceStride = components * g::componentBytes(input) + 3;
            const uint32_t destinationStride = components * g::componentBytes(output) + 1;
            std::vector<uint8_t> source(padded(sourceOffset + count * sourceStride + 9), 0xa7);
            std::vector<uint8_t> destination(padded(destinationOffset + count * destinationStride + 11), 0xd3);
            for (uint32_t i = 0; i < count; ++i)
                for (uint32_t c = 0; c < components; ++c) {
                    auto* p = source.data() + sourceOffset + i * sourceStride + c * g::componentBytes(input);
                    if (input == g::PointComponent::UInt8)
                        *p = uint8_t(i + 31 * c);
                    else if (input == g::PointComponent::UInt16)
                        write(p, uint16_t(i * 257 + 101 * c));
                    else if (input == g::PointComponent::Float32)
                        write(p, float(i * 271.25 - 900 + c * .375));
                    else
                        write(p, i * 271.25 - 900 + c * .375 + 1e-7);
                }
            auto sourceBuffer = runtime.buffer(source.size(), 0, source.data(), true);
            auto destinationBuffer = runtime.buffer(destination.size(), 0, destination.data(), true);
            const auto src = stream(sourceBuffer, sourceOffset, sourceStride, count, input);
            const auto dst = stream(destinationBuffer, destinationOffset, destinationStride, count, output);
            g::PointStreamTransform t{
                {1,   -2,  4,    0},
                {.5f, 2,   .25f, 1},
                {2,   .5f, 4,    1},
                {-3,  7,   -20,  0}
            };
            auto expected = destination;
            reference(expected, source, src, dst, t);
            g::PointStreamConversion conversion;
            check(g::preparePointStreamConversion(src, dst, t, conversion), "prepare encoding matrix");
            launch(runtime, conversion);
            compare(runtime, destinationBuffer, expected, "encoding matrix/guard bytes");
            ++number;
        }
    std::puts(
        "PASS all 16 source/destination encoding combinations, unaligned offsets/strides and guard bytes");
}
void exceptionalAndRounding(ComputeRuntime& runtime) {
    const std::vector<double> values = {0,
                                        -0.,
                                        std::numeric_limits<double>::denorm_min(),
                                        double(std::numeric_limits<float>::denorm_min()),
                                        -double(std::numeric_limits<float>::denorm_min()),
                                        double(std::numeric_limits<float>::min()),
                                        double(std::numeric_limits<float>::max()),
                                        1. + std::ldexp(1., -24),
                                        1. + std::ldexp(3., -24),
                                        std::ldexp(1., -150),
                                        std::ldexp(3., -150),
                                        std::numeric_limits<double>::infinity(),
                                        -std::numeric_limits<double>::infinity()};
    std::vector<uint8_t> source(padded(1 + values.size() * 9 + 5), 0xee);
    for (size_t i = 0; i < values.size(); ++i)
        write(source.data() + 1 + i * 9, values[i]);
    auto srcBuffer = runtime.buffer(source.size(), 0, source.data(), true);
    for (auto encoding : {g::PointComponent::Float32, g::PointComponent::Float64, g::PointComponent::UInt8,
                          g::PointComponent::UInt16}) {
        const uint32_t width = g::componentBytes(encoding), stride = width + 1;
        std::vector<uint8_t> destination(padded(3 + values.size() * stride + 5), 0x9b), expected = destination;
        auto dstBuffer = runtime.buffer(destination.size(), 0, destination.data(), true);
        auto src = stream(srcBuffer, 1, 9, uint32_t(values.size()), g::PointComponent::Float64, 1);
        auto dst = stream(dstBuffer, 3, stride, uint32_t(values.size()), encoding, 1);
        reference(expected, source, src, dst, {});
        g::PointStreamConversion conversion;
        check(g::preparePointStreamConversion(src, dst, {}, conversion), "prepare rounding fixture");
        launch(runtime, conversion);
        compare(runtime, dstBuffer, expected, "software double rounding/widening");
    }
    const std::array<float, 4> nonfinite = {std::numeric_limits<float>::quiet_NaN(),
                                            std::numeric_limits<float>::infinity(),
                                            -std::numeric_limits<float>::infinity(), 65535.75f};
    std::vector<uint8_t> invalid(20, 0xee);
    for (uint32_t i = 0; i < 4; ++i)
        write(invalid.data() + 1 + i * 4, nonfinite[i]);
    auto invalidBuffer = runtime.buffer(invalid.size(), 0, invalid.data(), true);
    for (auto encoding : {g::PointComponent::UInt8, g::PointComponent::UInt16}) {
        std::vector<uint8_t> initial(16, 0xb4), expected = initial;
        auto dstBuffer = runtime.buffer(initial.size(), 0, initial.data(), true);
        auto src = stream(invalidBuffer, 1, 4, 4, g::PointComponent::Float32, 1);
        auto dst = stream(dstBuffer, 1, 3, 4, encoding, 1);
        reference(expected, invalid, src, dst, {});
        g::PointStreamConversion conversion;
        check(g::preparePointStreamConversion(src, dst, {}, conversion), "prepare exceptional integer fixture");
        launch(runtime, conversion);
        compare(runtime, dstBuffer, expected, "NaN/Inf integer policy and truncation");
    }
    std::puts("PASS binary64 ties, subnormals, signed zero, exact widening and exceptional integer conversion");
}
void associationAndValidation(ComputeRuntime& runtime) {
    constexpr uint32_t count = 17;
    std::vector<uint8_t> source(padded(count * 43 + 4), 0x61), initial(padded(count * 47 + 4), 0x97);
    for (uint32_t i = 0; i < count; ++i) {
        for (uint32_t c = 0; c < 3; ++c) {
            write(source.data() + i * 43 + 1 + c * 4, float(100 * i + c));
            write(source.data() + i * 43 + 15 + c * 4, float(-int32_t(100 * i + c) - .25f));
        }
        for (uint32_t c = 0; c < 4; ++c)
            source[i * 43 + 29 + c] = uint8_t(11 * i + 37 * c);
    }
    auto sourceBuffer = runtime.buffer(source.size(), 0, source.data(), true);
    auto destinationBuffer = runtime.buffer(initial.size(), 0, initial.data(), true);
    auto expected = initial;
    const std::array<g::PointStream, 3> sources = {
        stream(sourceBuffer, 1, 43, count, g::PointComponent::Float32),
        stream(sourceBuffer, 15, 43, count, g::PointComponent::Float32),
        stream(sourceBuffer, 29, 43, count, g::PointComponent::UInt8, 4)};
    const std::array<g::PointStream, 3> destinations = {
        stream(destinationBuffer, 3, 47, count, g::PointComponent::Float32),
        stream(destinationBuffer, 18, 47, count, g::PointComponent::Float64),
        stream(destinationBuffer, 43, 47, count, g::PointComponent::UInt8, 4)};
    g::PointStreamConversion conversion;
    for (uint32_t attribute = 0; attribute < 3; ++attribute) {
        reference(expected, source, sources[attribute], destinations[attribute], {});
        check(g::preparePointStreamConversion(sources[attribute], destinations[attribute], {}, conversion),
              "prepare interleaved attribute");
        launch(runtime, conversion);
    }
    compare(runtime, destinationBuffer, expected,
            "position/velocity/RGBA association and shared-word preservation");
    const auto retained = conversion;
    auto rejects = [&](g::PointStream src, g::PointStream dst, g::PointStreamTransform transform = {}) {
        require(NVRHI_FAILED(g::preparePointStreamConversion(src, dst, transform, conversion)),
                "invalid descriptor accepted");
        require(
            conversion.source == retained.source && conversion.destination == retained.destination &&
                std::memcmp(&conversion.parameters, &retained.parameters, sizeof(conversion.parameters)) == 0,
            "failed preparation changed output");
    };
    auto invalid = sources[0];
    invalid.byteOffset = UINT64_MAX;
    rejects(invalid, destinations[0]);
    invalid = sources[0];
    invalid.byteStride = 11;
    rejects(invalid, destinations[0]);
    invalid = sources[0];
    invalid.count = UINT32_MAX;
    rejects(invalid, destinations[0]);
    invalid = sources[0];
    invalid.component = g::PointComponent(99);
    rejects(invalid, destinations[0]);
    invalid = sources[0];
    invalid.components = 4;
    rejects(invalid, destinations[0]);
    invalid = sources[0];
    invalid.buffer = destinationBuffer;
    rejects(invalid, destinations[0]);
    g::PointStreamTransform bad;
    bad.scale.x = std::numeric_limits<float>::infinity();
    rejects(sources[0], destinations[0], bad);
    g::PointStream empty;
    check(g::preparePointStreamConversion(empty, empty, {}, conversion), "empty conversion");
    require(!conversion.source && !conversion.destination && !conversion.groupsX && !conversion.groupsY,
            "empty dispatch");
    compare(runtime, destinationBuffer, expected, "invalid requests must not alter GPU bytes");
    std::puts(
        "PASS attribute association, shared destination words, invalid descriptors, rollback and empty input");
}
}  // namespace
int main(int argc, const char** argv) {
    try {
        ethereal::slang::ExampleDevice device(argc, argv);
        auto shaderPath = device.shaderPath();
        for (int i = 1; i + 1 < argc; ++i)
            if (!std::strcmp(argv[i], "--shader-root"))
                shaderPath = std::filesystem::path(argv[++i]) /
                             donut::app::GetShaderTypeName(device.get()->getGraphicsAPI());
        ComputeRuntime runtime(device.get(), shaderPath);
        formatMatrix(runtime);
        exceptionalAndRounding(runtime);
        associationAndValidation(runtime);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Point stream conversion: %s\n", e.what());
        return 1;
    }
}
