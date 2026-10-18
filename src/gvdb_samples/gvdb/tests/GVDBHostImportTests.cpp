// SPDX-License-Identifier: Apache-2.0
#include "../GVDBHostImport.h"
#include "../slang/GVDBPortableVbx.h"
#include "../slang/GVDBTopologyBuilder.h"
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <stdexcept>

namespace g = gvdb::portable;
namespace s = gvdb::slang;
namespace {
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
void check(nvrhi::FRESULT result, const char* message) { require(NVRHI_SUCCEEDED(result), message); }
g::NodeId id(uint32_t group, uint32_t level, uint64_t index) {
    g::NodeId result;
    require(g::NodeId::encode(group, level, index, result), "fixture reference");
    return result;
}
g::LevelConfig shape() {
    g::LevelConfig config;
    config.levelCount = 4;
    config.logDimension = {2, 1, 1, 2};
    return config;
}
g::PackedHostTopology fixture() {
    g::HostVolume source;
    check(source.configure(shape()), "configure");
    for (double x : {-4., 0., 4., 12.}) {
        g::NodeId leaf;
        check(source.activate(x, 0, 0, leaf), "activate");
    }
    check(source.setActive(id(0, 0, 1), false), "inactive leaf");
    auto* node = source.editNode(id(0, 0, 2));
    node->priority = 37;
    node->reserved = 93;
    node->range[0] = -2.5f;
    node->range[1] = 7.25f;
    node->mask = 0x1020304050607080ull;
    check(source.finishTopology(), "finish");
    g::PackedHostTopology packed;
    check(source.packTopology(packed), "pack");
    return packed;
}
void compare(const g::PackedHostTopology& source, const g::HostVolume& imported) {
    require(!imported.dirty() && imported.activeLeafCount() == source.activeLeafCount,
            "active count preserved");
    require(imported.root() == id(0, source.volume.hierarchy.x, source.volume.hierarchy.y),
            "root ID preserved");
    for (uint32_t level = 0; level < source.levels.size(); ++level) {
        const auto& info = source.levels[level];
        require(imported.count(0, level) == info.nodes.z && imported.count(1, level) == info.children.z,
                "pool counts");
        for (uint32_t index = 0; index < info.nodes.z; ++index) {
            g::Node expected;
            std::memcpy(&expected,
                        reinterpret_cast<const uint8_t*>(source.nodes.data()) + info.nodes.x +
                            uint64_t(index) * info.nodes.y,
                        sizeof(expected));
            expected.atlasInterior = {-1, -1, -1};
            require(std::memcmp(&expected, imported.node(id(0, level, index)), sizeof(expected)) == 0,
                    "node bytes and IDs");
        }
        for (uint32_t index = 0; index < info.children.z; ++index)
            require(std::memcmp(imported.childList(id(1, level, index)),
                                reinterpret_cast<const uint8_t*>(source.children.data()) + info.children.x +
                                    uint64_t(index) * info.children.y,
                                imported.stride(1, level)) == 0,
                    "child references");
    }
    require(imported.isActive({-4, 0, 0}) && !imported.isActive({0, 0, 0}) && imported.isAllocated({0, 0, 0}),
            "inactive leaf survives import");
    require(imported.bounds().lower.x == -4 && imported.bounds().upper.x == 16, "recomputed bounds");
}
g::PackedHostTopology padded(const g::PackedHostTopology& source) {
    auto result = source;
    // Reverse physical level order, with unrelated poison padding and wider records.
    result.nodes.assign(16, 0xdeadbeef);
    result.children.assign(2, 0xdeadbeef);
    for (uint32_t reverse = uint32_t(source.levels.size()); reverse; --reverse) {
        const uint32_t level = reverse - 1;
        const auto& input = source.levels[level];
        auto& output = result.levels[level];
        output.nodes.x = uint32_t(result.nodes.size() * 4);
        output.nodes.y = input.nodes.y + 16;
        result.nodes.resize(result.nodes.size() + size_t(output.nodes.y / 4) * input.nodes.z, 0xa5a5a5a5);
        for (uint32_t i = 0; i < input.nodes.z; ++i)
            std::memcpy(
                reinterpret_cast<uint8_t*>(result.nodes.data()) + output.nodes.x + uint64_t(i) * output.nodes.y,
                reinterpret_cast<const uint8_t*>(source.nodes.data()) + input.nodes.x +
                    uint64_t(i) * input.nodes.y,
                sizeof(g::Node));
        output.children.x = uint32_t(result.children.size() * 4);
        output.children.y = input.children.y + 8;
        result.children.resize(result.children.size() + size_t(output.children.y / 4) * input.children.z,
                               0xa5a5a5a5);
        for (uint32_t i = 0; i < input.children.z; ++i)
            std::memcpy(reinterpret_cast<uint8_t*>(result.children.data()) + output.children.x +
                            uint64_t(i) * output.children.y,
                        reinterpret_cast<const uint8_t*>(source.children.data()) + input.children.x +
                            uint64_t(i) * input.children.y,
                        input.children.y);
    }
    return result;
}
void corruptions(const g::PackedHostTopology& source, std::unique_ptr<g::HostVolume>& output) {
    const auto* previous = output.get();
    const auto root = previous->root();
    g::PackedHostTopology before;
    check(previous->packTopology(before), "capture destination before failed import");
    auto rejects = [&](const std::function<void(g::PackedHostTopology&)>& edit) {
        auto bad = source;
        edit(bad);
        require(NVRHI_FAILED(g::importHostTopology(bad, output)), "corrupt topology rejected");
        require(output.get() == previous && output->root() == root && !output->dirty(),
                "failure preserves destination");
        g::PackedHostTopology after;
        check(output->packTopology(after), "capture destination after failed import");
        require(before.nodes == after.nodes && before.children == after.children &&
                    before.revision.epoch == after.revision.epoch &&
                    before.revision.topology == after.revision.topology &&
                    before.activeLeafCount == after.activeLeafCount,
                "failed import preserves bytes and revisions");
    };
    rejects([](auto& x) { x.levels[0].nodes.x = UINT32_MAX - 15; });
    rejects([](auto& x) { x.levels[0].nodes.y = UINT32_MAX - 15; });
    rejects([](auto& x) { x.levels[0].nodes.z = UINT32_MAX; });
    rejects([](auto& x) { x.levels[0].nodes.y = 48; });
    rejects([](auto& x) { x.levels[0].nodes.x = 4; });
    rejects([](auto& x) { x.levels[1].nodes.x = x.levels[0].nodes.x; });
    rejects([](auto& x) { x.levels[1].children.x = UINT32_MAX - 7; });
    rejects([](auto& x) { x.levels[1].children.y = 8; });
    rejects([](auto& x) { x.levels[2].children.x = x.levels[1].children.x; });
    rejects([](auto& x) {
        x = padded(x);
        x.children.pop_back();
    });
    rejects([](auto& x) {
        const auto& root = x.levels[x.volume.hierarchy.x];
        const auto word = (root.nodes.x + uint64_t(x.volume.hierarchy.y) * root.nodes.y + 40) / 4;
        x.nodes[size_t(word)] = x.nodes[size_t(word + 1)] = 0;
    });
    rejects([](auto& x) { x.levels[1].childExtent.x = std::numeric_limits<float>::quiet_NaN(); });
    rejects([](auto& x) { x.levels[0].nodeExtent.z += 1; });
    rejects([](auto& x) { x.levels[2].nodes.w = 31; });
    rejects([](auto& x) { x.volume.hierarchy.y = UINT32_MAX; });
    rejects([](auto& x) { x.volume.hierarchy.x = 9; });
    rejects([](auto& x) { x.volume.hierarchy.z = 0; });
    rejects([](auto& x) { x.nodes[x.levels[0].nodes.x / 4 + 10] = UINT32_MAX; });  // Parent group/level/index.
    rejects([](auto& x) { x.nodes[x.levels[0].nodes.x / 4 + 1] += 1; });           // Reciprocal geometry.
    rejects([](auto& x) { x.nodes[x.levels[0].nodes.x / 4] |= 1; });  // Leaf in the wrong level pool.
    rejects([](auto& x) { x.children[x.levels[1].children.x / 4] = 0xffff0000; });
    require(g::importHostTopology(source, output, {64}) == nvrhi::FE_OUT_OF_MEMORY && output.get() == previous,
            "allocation budget preserves output");
}
void emptyAndLeaf() {
    g::HostVolume empty;
    check(empty.configure(shape()), "configure empty");
    check(empty.finishTopology(), "finish empty");
    g::PackedHostTopology packed;
    check(empty.packTopology(packed), "pack empty");
    std::unique_ptr<g::HostVolume> imported;
    check(g::importHostTopology(packed, imported), "import empty");
    require(imported->root().isNull() && imported->bounds().empty && imported->activeLeafCount() == 0,
            "empty stays empty");
    g::NodeId leaf;
    check(imported->activate(-.5, -.5, -.5, leaf), "edit imported empty");
    check(imported->finishTopology(), "finish leaf");
    check(imported->packTopology(packed), "pack leaf");
    check(g::importHostTopology(packed, imported), "replace with leaf root");
    require(imported->root().level() == 0 && imported->isActive({-1, -1, -1}), "root below configured maximum");
    check(imported->setActive(imported->root(), false), "deactivate sole leaf");
    check(imported->finishTopology(), "finish inactive");
    check(imported->packTopology(packed), "pack inactive");
    check(g::importHostTopology(packed, imported), "import inactive");
    require(!imported->root().isNull() && imported->activeLeafCount() == 0 && imported->bounds().empty,
            "inactive allocated graph");
}
void portableInputs(const char* asset) {
    std::string error;
    s::TopologyBuildOptions options;
    options.logDimensions = {2, 1, 1, 2};
    s::PackedVolume packed;
    require(s::buildTopology(
                {
                    {-4, 0, 0, 0},
                    {0,  0, 0, 0},
                    {12, 4, 0, 0}
    },
                options, packed, error),
            "build portable input");
    std::unique_ptr<g::HostVolume> imported;
    check(g::importHostTopology(packed, imported), "import portable builder topology");
    require(
        imported->activeLeafCount() == 3 && imported->isActive({12, 4, 0}) && !imported->isAllocated({4, 0, 0}),
        "portable builder spatial identity");
    s::VbxRawVolume vbx;
    require(s::loadRawVBXFile(asset, vbx, error), error.c_str());
    check(g::importHostTopology(vbx.packed, imported), "import real VBX topology");
    uint32_t active = 0;
    const auto& info = vbx.packed.levels[0];
    for (uint32_t index = 0; index < info.nodes.z; ++index) {
        g::Node node;
        std::memcpy(&node,
                    reinterpret_cast<const uint8_t*>(vbx.packed.nodes.data()) + info.nodes.x +
                        uint64_t(index) * info.nodes.y,
                    sizeof(node));
        require(imported->findLeaf(node.position) == id(0, 0, index), "VBX leaf IDs/positions preserved");
        active += node.flags != 0;
    }
    require(active > 0 && imported->activeLeafCount() == active, "VBX active count");
    std::printf("Imported explosion VBX: %u allocated, %u active leaves\n", info.nodes.z, active);
}
}  // namespace
int main(int argc, char** argv) {
    try {
        require(argc == 2, "expected explosion.vbx path");
        const auto source = fixture();
        std::unique_ptr<g::HostVolume> imported;
        check(g::importHostTopology(source, imported), "import canonical");
        compare(source, *imported);
        auto extended = padded(source);
        // Serialized bounds do not override graph-derived active bounds.
        extended.volume.boundsMin.x = std::numeric_limits<float>::quiet_NaN();
        extended.volume.boundsMax.x = -100;
        check(g::importHostTopology(extended, imported), "import padded/reordered pools");
        compare(extended, *imported);
        corruptions(source, imported);
        emptyAndLeaf();
        portableInputs(argv[1]);
        std::puts("GVDB host topology import tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
