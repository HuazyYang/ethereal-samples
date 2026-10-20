// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "GVDBNvrhiApi.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <utility>
#include <vector>

namespace leaf_marker_fixture {
namespace g = gvdb::portable;
using namespace nvrhi;
inline void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
inline void check(FRESULT result, const char* message) { require(NVRHI_SUCCEEDED(result), message); }

// The same public-only workflow runs through integrated and installed facades.
inline void run(IDevice* device, const std::vector<char>& artifact) {
    g::VolumeCreateDesc desc;
    desc.device = device;
    desc.configuration.levelCount = 4;
    desc.configuration.logDimension = {1,1,1,1};
    desc.remapArtifact = artifact.data(); desc.remapArtifactBytes = artifact.size();
    g::VolumeHandle owner;
    check(g::createVolume(desc, &owner), "marker volume");
    std::array<g::NodeId,3> leaves;
    for (int i = 0; i < 3; ++i)
        check(owner->activate(i*2-2, 0, 0, leaves[i]), "marker signed leaf");
    check(owner->finishTopology(), "marker initial finish");
    check(owner->setChannel(0, {g::AtlasFormat::R8_UINT,{23,0,0,0}}), "marker channel");
    g::GenerationDesc request;
    request.placement.minimumBrickCount = {3,1,1};
    std::vector<uint8_t> upload(192, 233);
    for (int i = 0; i < 3; ++i) for (int z = 0; z < 2; ++z)
        for (int y = 0; y < 2; ++y) for (int x = 0; x < 2; ++x)
            upload[((z+1)*4+y+1)*12+i*4+x+1] = uint8_t(40+i*40+x+y*2+z*4);
    g::ChannelUploadDesc source{0,0,upload.data(),upload.size()};
    request.uploads = &source; request.uploadCount = 1;
    CommandListHandle commands;
    check(device->createCommandList({}, &commands), "marker command list");
    commands->open();
    const g::RecordingContext context{device,commands};
    struct Capture {
        g::SnapshotHandle snapshot;
        g::ReadbackHandle readback;
        BufferHandle nodes;
        std::vector<g::Node> expectedNodes;
        std::array<uint8_t,3> flags;
        std::array<bool,3> hasData;
    };
    std::vector<Capture> captures;
    auto publish = [&](std::array<uint8_t,3> flags, std::array<bool,3> hasData) {
        Capture capture;
        capture.flags = flags; capture.hasData = hasData;
        for (uint32_t level = 0; level < desc.configuration.levelCount; ++level) {
            const auto pool = owner->getPool(0,level);
            if (pool.count) {
                const auto* nodes = static_cast<const g::Node*>(pool.data);
                capture.expectedNodes.insert(capture.expectedNodes.end(),nodes,nodes+pool.count);
            }
        }
        g::GenerationTicketHandle ticket;
        check(owner->prepareGeneration(request, &ticket), "marker prepare");
        check(owner->recordAndPublish(context, ticket), "marker publish");
        check(owner->getSnapshot(&capture.snapshot), "marker snapshot");
        const uint32_t active = uint32_t(flags[0]!=0)+uint32_t(flags[1]!=0)+uint32_t(flags[2]!=0);
        require(capture.snapshot->getDesc().activeLeafCount == active, "marker snapshot active count");
        captures.push_back(std::move(capture));
    };
    publish({1,1,1}, {true,true,true});
    request.uploads = nullptr; request.uploadCount = 0;

    g::GenerationTicketHandle stale;
    check(owner->prepareGeneration(request, &stale), "marker prepare stale ticket");
    const auto revision = owner->getTopologyRevision().topology;
    require(!owner->editNode({}) && owner->setActive({},false) == FE_INVALID_ARGS &&
        !owner->isTopologyDirty(), "marker invalid node must not dirty topology");
    check(owner->setActive(leaves[0],true), "marker identical active flag");
    require(!owner->isTopologyDirty(), "marker identical flag preserves clean state");
    // CUDA's int-to-byte assignment preserves these exact low eight bits.
    const int markers[] = {256,258,-1};
    for (int i = 0; i < 3; ++i) {
        auto* node = owner->editNode(leaves[i]);
        require(node != nullptr, "marker edit node");
        node->flags = uint8_t(markers[i]);
    }
    require(owner->isTopologyDirty() && owner->getTopologyRevision().topology == revision &&
        owner->getActiveLeafCount() == 3, "marker dirty state retains last finished metadata");
    auto* rejected = reinterpret_cast<g::IGVDBGenerationTicket*>(uintptr_t(1));
    require(NVRHI_FAILED(owner->prepareGeneration(request,&rejected)) && !rejected,
        "marker dirty preparation clears output");
    require(NVRHI_FAILED(owner->recordAndPublish(context,stale)), "marker dirty ticket rejected");
    require(owner->isAllocated({-2,0,0}) && !owner->isActive({-2,0,0}) &&
        owner->findLeaf({-2,0,0}) == leaves[0], "marker inactive leaf retains identity");
    check(owner->finishTopology(), "marker finish exact flags");
    require(!owner->isTopologyDirty() && owner->getTopologyRevision().topology == revision+1 &&
        owner->getActiveLeafCount() == 2 && owner->getBounds().lower.x == 0 &&
        owner->getBounds().upper.x == 4, "marker finished count bounds revision");
    require(NVRHI_FAILED(owner->recordAndPublish(context,stale)), "marker finished stale ticket rejected");
    g::SnapshotHandle unchanged;
    check(owner->getSnapshot(&unchanged), "marker unchanged publication");
    require(unchanged->getDesc().revision.placement == captures[0].snapshot->getDesc().revision.placement &&
        unchanged->getDesc().activeLeafCount == 3, "marker edits and finish never publish implicitly");
    unchanged = nullptr; stale = nullptr;
    publish({0,2,255}, {false,true,true});

    // A boolean operation normalizes nonzero flag bytes, even when already active.
    for (auto leaf : leaves) check(owner->setActive(leaf,true), "marker normalize/reactivate");
    require(owner->isTopologyDirty(), "marker normalized flags dirty topology");
    for (auto leaf : leaves) require(owner->getNode(leaf)->flags == 1, "marker exact boolean flag");
    check(owner->finishTopology(), "marker finish reactivation");
    require(owner->getPoolCount(0,0) == 3 && owner->findLeaf({-2,0,0}) == leaves[0] &&
        owner->getBounds().lower.x == -2, "marker reactivation retains pool and signed bounds");
    publish({1,1,1}, {false,true,true});

    check(owner->prepareGeneration(request,&stale), "marker before mutable borrow");
    require(owner->editNode(leaves[1]) != nullptr && owner->isTopologyDirty(), "marker borrow dirties even without write");
    require(NVRHI_FAILED(owner->recordAndPublish(context,stale)), "marker mutable borrow invalidates ticket");
    check(owner->finishTopology(), "marker finish unchanged borrow");
    require(NVRHI_FAILED(owner->recordAndPublish(context,stale)), "marker borrow commit keeps ticket stale");
    stale = nullptr;
    for (auto leaf : leaves) check(owner->setActive(leaf,false), "marker deactivate all");
    check(owner->finishTopology(), "marker finish empty");
    require(owner->getActiveLeafCount() == 0 && owner->getBounds().empty &&
        owner->getPoolCount(0,0) == 3, "marker empty retains allocated leaves");
    publish({0,0,0}, {false,false,false});
    for (auto leaf : leaves) check(owner->setActive(leaf,true), "marker reactivate empty");
    check(owner->finishTopology(), "marker finish from empty");
    publish({1,1,1}, {false,false,false});

    // Read old generations only after all publications, detecting in-place changes.
    for (auto& capture : captures) {
        g::BorrowedBufferBinding binding;
        check(capture.snapshot->getTopologyBuffer(g::TopologyBuffer::Nodes,binding), "marker node binding");
        require(binding.byteSize == capture.expectedNodes.size()*sizeof(g::Node), "marker full node bytes");
        check(device->createBuffer(BufferDesc().setByteSize(binding.byteSize).setCpuAccess(CpuAccessMode::Read)
            .setInitialState(ResourceStates::CopyDest).setKeepInitialState(true), &capture.nodes), "marker staging");
        commands->copyBuffer(capture.nodes,0,binding.buffer,binding.byteOffset,binding.byteSize);
        check(owner->prepareReadback(capture.snapshot,1,&capture.readback), "marker retained readback prepare");
        check(owner->recordReadback(context,capture.readback), "marker retained readback record");
    }
    owner = nullptr;
    commands->close(); device->executeCommandList(commands);
    require(device->waitForIdle(), "marker caller completion");
    for (auto& capture : captures) {
        const auto* mapped = device->mapBuffer(capture.nodes,CpuAccessMode::Read);
        require(mapped != nullptr, "marker map nodes");
        std::vector<g::Node> nodes(capture.expectedNodes.size());
        std::memcpy(nodes.data(),mapped,nodes.size()*sizeof(g::Node));
        device->unmapBuffer(capture.nodes);
        for (size_t i = 0; i < nodes.size(); ++i) {
            auto actual = nodes[i];
            // Placement owns this field; every other serialized byte must survive.
            actual.atlasInterior = capture.expectedNodes[i].atlasInterior;
            require(!std::memcmp(&actual,&capture.expectedNodes[i],sizeof(actual)), "marker exact node bytes");
        }
        for (size_t i = 0; i < leaves.size(); ++i)
            require(nodes[size_t(leaves[i].index())].flags == capture.flags[i], "marker GPU exact flags");
        check(capture.readback->finishAfterCallerCompletion(), "marker finish readback");
        g::BorrowedBytes bytes;
        check(capture.readback->getChannelData(0,bytes), "marker channel bytes");
        const auto size = capture.snapshot->getDesc().volume.atlasResolutionAndApron;
        require(bytes.byteCount == uint64_t(size.x)*size.y*size.z, "marker atlas size");
        const auto* data = static_cast<const uint8_t*>(bytes.data);
        for (uint32_t z = 0; z < size.z; ++z) for (uint32_t y = 0; y < size.y; ++y)
            for (uint32_t x = 0; x < size.x; ++x) {
                uint8_t expected = 0; // Unused atlas slots are zero, not the channel void value.
                for (size_t i = 0; i < leaves.size(); ++i) if (capture.flags[i]) {
                    const auto& node = nodes[size_t(leaves[i].index())];
                    const int dx = int(x)-node.atlasInterior.x, dy = int(y)-node.atlasInterior.y,
                        dz = int(z)-node.atlasInterior.z;
                    if (dx < -1 || dx > 2 || dy < -1 || dy > 2 || dz < -1 || dz > 2) continue;
                    expected = 23;
                    const int wx = node.position.x+dx;
                    for (int j = 0; j < 3; ++j) if (capture.flags[j] && capture.hasData[j] &&
                        wx >= j*2-2 && wx < j*2 && dy >= 0 && dy < 2 && dz >= 0 && dz < 2)
                        expected = uint8_t(40+j*40+(wx-(j*2-2))+dy*2+dz*4);
                }
                const uint8_t actual = data[(size_t(z)*size.y+y)*size.x+x];
                if (actual != expected) {
                    std::fprintf(stderr,"Marker generation %zu at (%u,%u,%u): actual %u expected %u\n",
                        size_t(&capture-captures.data()),x,y,z,unsigned(actual),unsigned(expected));
                    throw std::runtime_error("marker retained interior/apron bytes");
                }
            }
    }
    std::puts("PASS leaf markers: exact flags, dirty/finished/stale tickets, signed inactive/reactivated leaves, empty topology, retained GPU nodes/channels");
}
} // namespace leaf_marker_fixture
