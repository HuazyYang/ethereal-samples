// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "TopologyEditApiFixture.h"
#include <string>
namespace topology_edit_fixture {
inline void runActivity(nvrhi::IDevice* device, const std::vector<char>& remap,
                        const std::array<std::vector<char>, 5>& artifacts) {
    g::TopologyActivityCreateDesc createActivity;
    createActivity.device = device;
    for (size_t i = 0; i < artifacts.size(); ++i)
        createActivity.artifacts[i] = {artifacts[i].data(), artifacts[i].size()};
    g::TopologyActivityHandle activity;
    check(g::createTopologyActivity(createActivity, &activity), "activity factory");
    auto wrong = createActivity;
    wrong.artifacts[1] = wrong.artifacts[0];
    auto* rejected = reinterpret_cast<g::IGVDBTopologyActivity*>(uintptr_t(1));
    require(NVRHI_FAILED(g::createTopologyActivity(wrong, &rejected)) && !rejected,
            "activity rejects wrong entry and clears output");
    for (int scenario = 0; scenario < 5; ++scenario) {
        const uint32_t count = scenario == 0 ? 0 : scenario == 1 ? 1 : 257;
        const int32_t base = scenario == 3 ? 4194304 : scenario == 4 ? -4194304 : -260;
        g::VolumeCreateDesc create;
        create.device = device;
        create.configuration.levelCount = 10;
        create.configuration.logDimension.fill(1);
        create.remapArtifact = remap.data();
        create.remapArtifactBytes = remap.size();
        g::VolumeHandle owner;
        check(g::createVolume(create, &owner), "activity owner");
        std::vector<g::NodeId> leaves(count);
        for (uint32_t i = 0; i < count; ++i)
            check(owner->activate(double(base) + i * 2, (i % 3) * 2, 0, leaves[i]), "activity leaf");
        check(owner->finishTopology(), "activity initial finish");
        check(owner->setChannel(0,
                                {
                                    g::AtlasFormat::R8_UINT, {0, 0, 0, 0}
        }),
              "activity channel");
        CommandListHandle commands;
        check(device->createCommandList({}, &commands), "activity commands");
        g::RecordingContext context{device, commands};
        auto submit = [&] {
            commands->close();
            device->executeCommandList(commands);
            require(device->waitForIdle(), "activity caller completion");
            device->runGarbageCollection();
        };
        auto publish = [&](g::IGVDBGenerationTicket* ticket) {
            commands->open();
            check(owner->recordAndPublish(context, ticket), "activity publication");
            submit();
        };
        g::GenerationDesc generation;
        g::GenerationTicketHandle initial;
        check(owner->prepareGeneration(generation, &initial), "activity initial prepare");
        publish(initial);
        std::array<std::vector<g::Node>, 10> original;
        std::array<std::vector<uint8_t>, 10> childBytes;
        for (uint32_t level = 0; level < 10; ++level) {
            for (uint32_t i = 0; i < owner->getPoolCount(0, level); ++i) {
                g::NodeId id;
                require(g::NodeId::encode(0, level, i, id), "activity encode");
                original[level].push_back(*owner->getNode(id));
            }
            auto pool = owner->getPool(1, level);
            if (pool.count) {
                auto* start = static_cast<const uint8_t*>(pool.data);
                childBytes[level].assign(start, start + pool.count * pool.stride);
            }
        }
        g::TopologyEditCreateDesc editCreate;
        editCreate.volume = owner;
        g::TopologyEditHandle edit;
        check(g::createTopologyEdit(editCreate, &edit), "activity edit factory");
        BufferHandle markers;
        if (count) {
            BufferDesc d;
            d.byteSize = uint64_t(count + 4) * 4;
            d.structStride = 4;
            d.initialState = ResourceStates::ShaderResource;
            d.keepInitialState = true;
            check(device->createBuffer(d, &markers), "activity markers");
        }
        for (uint32_t pass = 0; pass < 6; ++pass) {
            std::vector<uint32_t> values(count + 4, 0xdeadbeef);
            std::array<std::vector<uint8_t>, 10> expected;
            for (uint32_t level = 0; level < 10; ++level)
                expected[level].resize(original[level].size());
            uint32_t activeCount = 0;
            for (uint32_t i = 0; i < count; ++i) {
                bool active = pass == 1 || pass == 5 || (pass == 2 && i % 7 == 0) || (pass == 3 && i == 0) ||
                              (pass == 4 && i + 1 == count);
                values[i + 2] = active ? (i % 2 ? 0x80000000u : 2u) : 0;
                if (!active)
                    continue;
                ++activeCount;
                auto id = leaves[i];
                while (!id.isNull()) {
                    expected[id.level()][size_t(id.index())] = 1;
                    id = original[id.level()][size_t(id.index())].parent;
                }
            }
            g::TopologyEditTicketHandle ticket;
            check(edit->prepare({}, &ticket), "activity prepare edit");
            g::TopologyActivityDesc d;
            d.markers = {markers.Get(), count ? 8u : 0u, uint64_t(count) * 4};
            d.maxAllocatedBytes = count ? (count == 1 ? 264 : (2 + 3 * owner->getRootId().level()) * 256) : 0;
            d.maxBuffers = count ? (count == 1 ? 2 : 1) : 0;
            commands->open();
            require(NVRHI_FAILED(activity->record(context, ticket, d)), "activity before begin rejected");
            check(edit->recordBegin(context, ticket), "activity begin edit");
            if (count) {
                commands->writeBuffer(markers, values.data(), values.size() * 4);
                auto bad = d;
                --bad.maxAllocatedBytes;
                require(activity->record(context, ticket, bad) == FE_OUT_OF_MEMORY,
                        "activity allocation budget");
                bad = d;
                bad.maxBuffers = 0;
                require(activity->record(context, ticket, bad) == FE_OUT_OF_MEMORY, "activity buffer budget");
                bad = d;
                bad.markers.byteOffset = 1;
                require(activity->record(context, ticket, bad) == FE_INVALID_ARGS,
                        "activity unaligned markers");
                bad = d;
                bad.markers.byteSize -= 4;
                require(activity->record(context, ticket, bad) == FE_INVALID_ARGS, "activity short markers");
            }
            check(activity->record(context, ticket, d), "record activity sequence");
            // The sequence is repeatable before fetch, including all-inactive trees.
            check(activity->record(context, ticket, d), "record repeated activity sequence");
            check(edit->recordFetch(context, ticket), "activity fetch");
            require(NVRHI_FAILED(activity->record(context, ticket, d)), "activity after fetch rejected");
            submit();
            g::GenerationTicketHandle publication;
            const auto fetchResult = ticket->finishAfterCallerCompletion(&publication);
            if (NVRHI_FAILED(fetchResult)) {
                throw std::runtime_error("activity graph validation scenario " + std::to_string(scenario) +
                                         " pass " + std::to_string(pass));
            }
            publish(publication);
            require(owner->getActiveLeafCount() == activeCount, "activity exact leaf count");
            for (uint32_t level = 0; level < 10; ++level) {
                require(owner->getPoolCount(0, level) == original[level].size(),
                        "activity retains pool counts");
                for (uint32_t i = 0; i < original[level].size(); ++i) {
                    g::NodeId id;
                    require(g::NodeId::encode(0, level, i, id), "activity encode result");
                    const auto* node = owner->getNode(id);
                    const auto& before = original[level][i];
                    require(node && node->flags == expected[level][i], "CPU oracle internal and leaf flags");
                    require(node->parent == before.parent && node->children == before.children &&
                                std::memcmp(&node->position, &before.position, sizeof(before.position)) == 0,
                            "activity retains coordinates and reciprocal links");
                }
                auto pool = owner->getPool(1, level);
                require(pool.count * pool.stride == childBytes[level].size(), "activity child pool bytes");
                if (!childBytes[level].empty())
                    require(std::memcmp(pool.data, childBytes[level].data(), childBytes[level].size()) == 0,
                            "activity restores every allocated child slot exactly");
            }
        }
        // A successful host edit makes this prepared private operation stale.
        g::TopologyEditTicketHandle stale;
        check(edit->prepare({}, &stale), "activity stale prepare");
        commands->open();
        check(edit->recordBegin(context, stale), "activity stale begin");
        check(owner->finishTopology(), "activity host revision change");
        g::TopologyActivityDesc d;
        d.markers = {markers.Get(), count ? 8u : 0u, uint64_t(count) * 4};
        require(NVRHI_FAILED(activity->record(context, stale, d)), "activity rejects stale host revision");
        submit();
    }
}
}  // namespace topology_edit_fixture
