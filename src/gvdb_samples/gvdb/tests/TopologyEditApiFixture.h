// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "GVDBNvrhiTopologyEditApi.h"
#include <nvrhi/shader-program.h>
#include <array>
#include <cstring>
#include <stdexcept>
#include <vector>
namespace topology_edit_fixture {
namespace g = gvdb::portable;
using namespace nvrhi;
inline void require(bool ok, const char* text) {
    if (!ok)
        throw std::runtime_error(text);
}
inline void check(FRESULT result, const char* text) { require(NVRHI_SUCCEEDED(result), text); }
inline void run(IDevice* device, const std::vector<char>& remap, const std::vector<char>& markerArtifact) {
    g::VolumeCreateDesc create;
    create.device = device;
    create.configuration.levelCount = 4;
    create.configuration.logDimension = {1, 1, 1, 1};
    create.remapArtifact = remap.data();
    create.remapArtifactBytes = remap.size();
    g::VolumeHandle owner;
    check(g::createVolume(create, &owner), "topology edit owner");
    std::array<g::NodeId, 3> leaves;
    for (int i = 0; i < 3; ++i)
        check(owner->activate(i * 2 - 2, 0, 0, leaves[i]), "signed edit leaf");
    check(owner->finishTopology(), "edit finish initial topology");
    check(owner->setChannel(0,
                            {
                                g::AtlasFormat::R8_UINT, {23, 0, 0, 0}
    }),
          "edit channel");
    CommandListHandle commands;
    check(device->createCommandList({}, &commands), "edit commands");
    g::RecordingContext context{device, commands};
    auto submit = [&] {
        commands->close();
        device->executeCommandList(commands);
        require(device->waitForIdle(), "edit caller completion");
        device->runGarbageCollection();
    };
    auto publish = [&](g::GenerationTicketHandle ticket) {
        commands->open();
        check(owner->recordAndPublish(context, ticket), "edit publish");
        submit();
    };
    g::GenerationDesc generation;
    generation.placement.minimumBrickCount = {3, 1, 1};
    g::GenerationTicketHandle initial;
    check(owner->prepareGeneration(generation, &initial), "edit initial generation");
    publish(initial);
    g::SnapshotHandle old;
    check(owner->getSnapshot(&old), "edit old snapshot");
    const auto revision = owner->getTopologyRevision();
    g::TopologyEditCreateDesc factory;
    factory.volume = owner;
    g::TopologyEditHandle op, other;
    check(g::createTopologyEdit(factory, &op), "edit factory");
    check(g::createTopologyEdit(factory, &other), "other edit factory");
    g::TopologyEditDesc options;
    options.placement = generation.placement;
    g::BorrowedBufferBinding sourceNodes, sourceChildren;
    check(old->getTopologyBuffer(g::TopologyBuffer::Nodes, sourceNodes), "old node range");
    check(old->getTopologyBuffer(g::TopologyBuffer::Children, sourceChildren), "old child range");
    const uint64_t bytes = sourceNodes.byteSize + sourceChildren.byteSize;
    auto bad = [&](g::TopologyEditDesc d, FRESULT expected) {
        auto* ticket = reinterpret_cast<g::IGVDBTopologyEditTicket*>(uintptr_t(1));
        require(op->prepare(d, &ticket) == expected && !ticket, "edit invalid request clears output");
    };
    auto invalid = options;
    invalid.structSize--;
    bad(invalid, FE_INVALID_ARGS);
    invalid = options;
    invalid.reserved = 1;
    bad(invalid, FE_INVALID_ARGS);
    invalid = options;
    invalid.preserveMetadata = 2;
    bad(invalid, FE_INVALID_ARGS);
    invalid = options;
    invalid.placement.preserve = 2;
    bad(invalid, FE_INVALID_ARGS);
    invalid = options;
    invalid.maxReadbackBytes = bytes - 1;
    bad(invalid, FE_OUT_OF_MEMORY);
    invalid = options;
    invalid.maxAllocatedBytes = bytes * 2 - 1;
    bad(invalid, FE_OUT_OF_MEMORY);
    invalid = options;
    invalid.maxBuffers = 3;
    bad(invalid, FE_OUT_OF_MEMORY);
    options.maxReadbackBytes = bytes;
    options.maxAllocatedBytes = bytes * 2;
    options.maxBuffers = 4;
    g::TopologyEditTicketHandle edit;
    check(op->prepare(options, &edit), "edit exact budget");
    g::BorrowedBufferBinding scratch;
    require(NVRHI_FAILED(edit->getTopologyBuffer(g::TopologyBuffer::Nodes, scratch)),
            "edit private buffer phase");
    auto* rejected = reinterpret_cast<g::IGVDBGenerationTicket*>(uintptr_t(1));
    require(NVRHI_FAILED(edit->finishAfterCallerCompletion(&rejected)) && !rejected,
            "edit finish before fetch");
    commands->open();
    require(NVRHI_FAILED(other->recordBegin(context, edit)), "edit foreign factory");
    require(NVRHI_FAILED(op->recordFetch(context, edit)), "fetch before begin");
    check(op->recordBegin(context, edit), "begin private copy");
    require(NVRHI_FAILED(op->recordBegin(context, edit)), "duplicate edit begin");
    check(edit->getTopologyBuffer(g::TopologyBuffer::Nodes, scratch), "edit writable nodes");
    require(scratch.buffer != sourceNodes.buffer && scratch.byteSize == sourceNodes.byteSize,
            "private topology allocation");
    g::BorrowedBufferBinding levels, children;
    check(edit->getTopologyBuffer(g::TopologyBuffer::Levels, levels), "edit read-only levels");
    check(edit->getTopologyBuffer(g::TopologyBuffer::Children, children), "edit writable children");
    require(edit->getTopologyBuffer(g::TopologyBuffer::AtlasMap, levels) == FE_UNSUPPORTED,
            "edit no writable atlas map");
    ShaderProgramHandle program;
    check(createShaderProgram(device, markerArtifact.data(), markerArtifact.size(), &program),
          "marker program");
    uint32_t entry;
    check(program->findEntryPoint("applyLeafMarkers", ShaderType::Compute, &entry), "marker entry");
    BindingLayoutDesc layoutDesc;
    check(program->getBindingLayoutDesc(entry, 0, layoutDesc), "marker layout description");
    BindingLayoutHandle layout;
    check(device->createBindingLayout(layoutDesc, &layout), "marker layout");
    BufferDesc markerDesc;
    markerDesc.byteSize = 12;
    markerDesc.structStride = 4;
    markerDesc.initialState = ResourceStates::ShaderResource;
    markerDesc.keepInitialState = true;
    BufferHandle markers;
    check(device->createBuffer(markerDesc, &markers), "marker buffer");
    const uint32_t values[3] = {0, 1, 1};
    commands->writeBuffer(markers, values, sizeof(values));
    BufferDesc constantDesc;
    constantDesc.byteSize = 256;
    constantDesc.isConstantBuffer = true;
    constantDesc.initialState = ResourceStates::ConstantBuffer;
    constantDesc.keepInitialState = true;
    BufferHandle constants;
    check(device->createBuffer(constantDesc, &constants), "marker constants");
    std::array<uint8_t, 256> constantData{};
    auto source = edit->getSourceDesc();
    std::memcpy(constantData.data(), &source.volume, sizeof(source.volume));
    commands->writeBuffer(constants, constantData.data(), constantData.size());
    BindingSetDesc set;
    set.trackLiveness = true;
    for (const auto& item : layoutDesc.bindings) {
        if (item.type == ResourceType::ConstantBuffer)
            set.addItem(BindingSetItem::ConstantBuffer(item.slot, constants));
        else if (item.type == ResourceType::StructuredBuffer_SRV && item.slot == 0)
            set.addItem(BindingSetItem::StructuredBuffer_SRV(0, levels.buffer, Format::UNKNOWN,
                                                             {levels.byteOffset, levels.byteSize}));
        else if (item.type == ResourceType::StructuredBuffer_SRV && item.slot == 1)
            set.addItem(BindingSetItem::StructuredBuffer_SRV(1, markers));
        else if (item.type == ResourceType::RawBuffer_UAV && item.slot == 0)
            set.addItem(BindingSetItem::RawBuffer_UAV(0, scratch.buffer));
        else if (item.type == ResourceType::RawBuffer_UAV && item.slot == 1)
            set.addItem(BindingSetItem::RawBuffer_UAV(1, children.buffer));
        else
            throw std::runtime_error("Unexpected marker binding");
    }
    BindingSetHandle bindings;
    check(device->createBindingSet(set, layout, &bindings), "marker binding set");
    ShaderHandle shader;
    check(program->createShader(entry, &shader), "marker shader");
    ComputePipelineHandle pipeline;
    check(device->createComputePipeline(ComputePipelineDesc().setComputeShader(shader).addBindingLayout(layout),
                                        &pipeline),
          "marker pipeline");
    commands->setComputeState(ComputeState().setPipeline(pipeline).addBindingSet(bindings));
    commands->dispatch(1);
    check(op->recordFetch(context, edit), "record edited topology fetch");
    require(NVRHI_FAILED(op->recordFetch(context, edit)), "duplicate topology fetch");
    require(NVRHI_FAILED(edit->getTopologyBuffer(g::TopologyBuffer::Nodes, scratch)),
            "no mutable borrow after fetch");
    submit();
    g::GenerationTicketHandle publication;
    check(edit->finishAfterCallerCompletion(&publication), "validate GPU topology bytes");
    require(owner->getActiveLeafCount() == 3 && owner->getTopologyRevision().epoch == revision.epoch,
            "fetch preparation does not mutate owner");
    rejected = reinterpret_cast<g::IGVDBGenerationTicket*>(uintptr_t(1));
    require(NVRHI_FAILED(edit->finishAfterCallerCompletion(&rejected)) && !rejected,
            "single-use completed edit");
    publish(publication);
    require(owner->getActiveLeafCount() == 2 && owner->getBounds().lower.x == 0 &&
                owner->getTopologyRevision().epoch == revision.epoch + 1,
            "GPU markers publish recomputed bounds and fresh epoch");
    for (size_t i = 0; i < leaves.size(); ++i)
        require(owner->getNode(leaves[i])->flags == values[i], "exact GPU flag publication");
    g::SnapshotHandle current;
    check(owner->getSnapshot(&current), "edited snapshot");
    require(old->getDesc().activeLeafCount == 3 && current->getDesc().activeLeafCount == 2,
            "retained generation metadata");
    g::ReadbackHandle oldData, newData;
    check(owner->prepareReadback(old, 1, &oldData), "old atlas readback");
    check(owner->prepareReadback(current, 1, &newData), "new atlas readback");
    commands->open();
    check(owner->recordReadback(context, oldData), "record old atlas");
    check(owner->recordReadback(context, newData), "record new atlas");
    BufferDesc retainedDesc;
    retainedDesc.byteSize = sourceNodes.byteSize;
    retainedDesc.cpuAccess = CpuAccessMode::Read;
    BufferHandle retainedNodes;
    check(device->createBuffer(retainedDesc, &retainedNodes), "retained old node staging");
    commands->copyBuffer(retainedNodes, 0, sourceNodes.buffer, sourceNodes.byteOffset, sourceNodes.byteSize);
    submit();
    const auto* retainedBytes = static_cast<const uint8_t*>(device->mapBuffer(retainedNodes, CpuAccessMode::Read));
    require(retainedBytes != nullptr, "map retained old nodes");
    g::LevelInfo oldLeaf;
    check(old->getLevel(0, oldLeaf), "old leaf pool metadata");
    bool originalFlags = true;
    for (const auto id : leaves)
        originalFlags &= retainedBytes[oldLeaf.nodes.x + id.index() * oldLeaf.nodes.y + 1] == 1;
    device->unmapBuffer(retainedNodes);
    require(originalFlags, "GPU marker edit leaves published source node bytes unchanged");
    check(oldData->finishAfterCallerCompletion(), "finish old atlas");
    check(newData->finishAfterCallerCompletion(), "finish new atlas");
    g::BorrowedBytes before, after;
    check(oldData->getChannelData(0, before), "old atlas bytes");
    check(newData->getChannelData(0, after), "new atlas bytes");
    require(before.byteCount && after.byteCount, "retained channel payloads");
    for (uint64_t i = 0; i < before.byteCount; ++i)
        require(static_cast<const uint8_t*>(before.data)[i] == 23, "old channel untouched");
    const auto placed = current->getDesc().volume;
    for (uint64_t i = 0; i < after.byteCount; ++i) {
        const uint32_t x = uint32_t(i) % placed.atlasResolutionAndApron.x;
        const uint8_t expected = x / placed.atlasBrickCountAndWidth.w < 2 ? 23 : 0;
        require(static_cast<const uint8_t*>(after.data)[i] == expected,
                "remapped interiors/aprons and zero unused slots");
    }
    // A device-written orphan is not a publishable host graph. Fetch rejects it
    // transactionally; an incremental delink builder must reconnect or compact.
    auto fresh = [&] {
        g::TopologyEditTicketHandle value;
        check(op->prepare(options, &value), "fresh topology edit");
        return value;
    };
    auto malformed = fresh();
    commands->open();
    check(op->recordBegin(context, malformed), "begin malformed edit");
    check(malformed->getTopologyBuffer(g::TopologyBuffer::Nodes, scratch), "malformed node buffer");
    g::LevelInfo leaf;
    check(malformed->getLevel(0, leaf), "leaf metadata");
    const uint64_t nullParent = UINT64_MAX;
    commands->writeBuffer(scratch.buffer, &nullParent, 8, leaf.nodes.x + 40);
    check(op->recordFetch(context, malformed), "fetch malformed graph");
    submit();
    rejected = reinterpret_cast<g::IGVDBGenerationTicket*>(uintptr_t(1));
    require(NVRHI_FAILED(malformed->finishAfterCallerCompletion(&rejected)) && !rejected, "reject GPU orphan");
    require(owner->getActiveLeafCount() == 2, "invalid fetch preserves owner");
    auto stale = fresh();
    check(owner->prepareGeneration(generation, &initial), "intervening generation");
    publish(initial);
    commands->open();
    require(NVRHI_FAILED(op->recordBegin(context, stale)), "publication stales edit");
    submit();
    stale = fresh();
    commands->open();
    check(op->recordBegin(context, stale), "begin stale finish edit");
    check(op->recordFetch(context, stale), "fetch before CPU edit");
    submit();
    require(owner->editNode(leaves[1]) != nullptr, "dirty live host");
    rejected = reinterpret_cast<g::IGVDBGenerationTicket*>(uintptr_t(1));
    require(NVRHI_FAILED(stale->finishAfterCallerCompletion(&rejected)) && !rejected,
            "CPU edit stales fetched bytes");
    check(owner->finishTopology(), "finish live CPU edit");
    rejected = reinterpret_cast<g::IGVDBGenerationTicket*>(uintptr_t(1));
    require(NVRHI_FAILED(stale->finishAfterCallerCompletion(&rejected)) && !rejected,
            "finished CPU revision still stale");
    auto* noEdit = reinterpret_cast<g::IGVDBTopologyEditTicket*>(uintptr_t(1));
    require(NVRHI_FAILED(op->prepare(options, &noEdit)) && !noEdit,
            "unpublished host topology cannot be overwritten");
    check(owner->prepareGeneration(generation, &initial), "publish host revision before edit");
    publish(initial);
    // Channel reconfiguration is stale even when the descriptor is unchanged.
    stale = fresh();
    check(owner->setChannel(0,
                            {
                                g::AtlasFormat::R8_UINT, {23, 0, 0, 0}
    }),
          "change channel incarnation");
    commands->open();
    require(NVRHI_FAILED(op->recordBegin(context, stale)), "channel config stales edit");
    submit();
    check(owner->prepareGeneration(generation, &initial), "publish channel incarnation");
    publish(initial);
    // Publication resource limits fail before touching current topology.
    auto limited = options;
    limited.maxPublicationBuffers = 0;
    check(op->prepare(limited, &stale), "prepare publication-budget edit");
    commands->open();
    check(op->recordBegin(context, stale), "begin publication-budget edit");
    check(op->recordFetch(context, stale), "fetch publication-budget edit");
    submit();
    rejected = reinterpret_cast<g::IGVDBGenerationTicket*>(uintptr_t(1));
    require(stale->finishAfterCallerCompletion(&rejected) == FE_OUT_OF_MEMORY && !rejected,
            "publication allocation rollback");
    require(owner->getActiveLeafCount() == 2, "publication budget preserves current topology");
    auto publishFlags = [&](std::array<uint8_t, 3> flags) {
        auto change = fresh();
        commands->open();
        check(op->recordBegin(context, change), "begin flag-byte edit");
        check(change->getTopologyBuffer(g::TopologyBuffer::Nodes, scratch), "flag-byte private nodes");
        g::LevelInfo info;
        check(change->getLevel(0, info), "flag-byte leaf metadata");
        for (size_t i = 0; i < leaves.size(); ++i) {
            uint32_t word;
            std::memcpy(&word, owner->getNode(leaves[i]), 4);
            word = (word & ~0xff00u) | (uint32_t(flags[i]) << 8);
            commands->writeBuffer(scratch.buffer, &word, 4, info.nodes.x + leaves[i].index() * info.nodes.y);
        }
        check(op->recordFetch(context, change), "fetch exact flag bytes");
        submit();
        check(change->finishAfterCallerCompletion(&publication), "validate exact flag bytes");
        publish(publication);
        for (size_t i = 0; i < leaves.size(); ++i)
            require(owner->getNode(leaves[i])->flags == flags[i], "preserve nonboolean flag bytes");
    };
    publishFlags({0, 0, 0});
    require(owner->getActiveLeafCount() == 0 && owner->getBounds().empty, "all-inactive GPU graph");
    publishFlags({2, 255, 0});
    require(owner->getActiveLeafCount() == 2, "reactivate private GPU graph with exact flags");
    // A published empty graph has no GPU payload and needs no edit/staging buffers.
    check(owner->clearTopology(), "clear edit topology");
    check(owner->finishTopology(), "finish empty graph");
    check(owner->prepareGeneration(generation, &initial), "publish empty graph");
    publish(initial);
    options.maxAllocatedBytes = options.maxReadbackBytes = 0;
    options.maxBuffers = 0;
    check(op->prepare(options, &stale), "empty topology zero-budget edit");
    commands->open();
    check(op->recordBegin(context, stale), "begin empty topology edit");
    check(stale->getTopologyBuffer(g::TopologyBuffer::Nodes, scratch), "empty topology binding");
    require(!scratch.buffer && scratch.byteSize == 0, "empty topology has no payload allocation");
    check(op->recordFetch(context, stale), "fetch empty topology");
    submit();
    // Ticket lifetime is independent of both factory objects and caller owner handle.
    op = nullptr;
    other = nullptr;
    factory.volume = nullptr;
    owner = nullptr;
    check(stale->finishAfterCallerCompletion(&publication), "finish retained empty edit after factory release");
}
}  // namespace topology_edit_fixture
