// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "GVDBNvrhiRayQueryApi.h"
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>
namespace ray_query_fixture {
namespace g = gvdb::portable;
using namespace nvrhi;
inline void require(bool ok, const char* text) {
    if (!ok)
        throw std::runtime_error(text);
}
inline void check(FRESULT result, const char* text) { require(NVRHI_SUCCEEDED(result), text); }
inline void run(IDevice* device, const std::vector<char>& remap, const std::vector<char>& artifact) {
    g::VolumeCreateDesc d;
    d.device = device;
    d.configuration.levelCount = 1;
    d.configuration.logDimension = {3};
    d.remapArtifact = remap.data();
    d.remapArtifactBytes = remap.size();
    g::VolumeHandle owner;
    check(g::createVolume(d, &owner), "ray volume");
    g::NodeId leaf;
    check(owner->allocateNode(0, {-8, -8, -8}, true, leaf), "ray signed root");
    check(owner->setRoot(leaf), "ray root");
    check(owner->finishTopology(), "ray finish topology");
    check(owner->setChannel(31, {g::AtlasFormat::R32_FLOAT, {}}), "ray high scalar channel");
    check(owner->setChannel(1, {g::AtlasFormat::R8_UINT, {}}), "ray non scalar channel");
    std::vector<float> atlas(1000, 233);
    for (int z = 0; z < 8; ++z)
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x)
                atlas[((z + 1) * 10 + y + 1) * 10 + x + 1] = (x + .5f) / 8;
    const g::ChannelUploadDesc upload{31, 0, atlas.data(), atlas.size() * 4};
    g::GenerationDesc generation;
    generation.placement.minimumBrickCount = {1, 1, 1};
    generation.uploadCount = 1;
    generation.uploads = &upload;
    g::GenerationTicketHandle publication;
    check(owner->prepareGeneration(generation, &publication), "ray prepare volume");
    CommandListHandle commands;
    check(device->createCommandList({}, &commands), "ray commands");
    const g::RecordingContext context{device, commands};
    auto submit = [&] {
        commands->close();
        device->executeCommandList(commands);
        require(device->waitForIdle(), "ray caller completion");
        device->runGarbageCollection();
    };
    commands->open();
    check(owner->recordAndPublish(context, publication), "ray publish volume");
    submit();
    g::SnapshotHandle snapshot;
    check(owner->getSnapshot(&snapshot), "ray snapshot");
    g::RayQueryCreateDesc create;
    create.device = device;
    create.artifact = artifact.data();
    create.artifactBytes = artifact.size();
    g::RayQueryHandle op, other;
    check(g::createRayQuery(create, &op), "ray factory");
    check(g::createRayQuery(create, &other), "ray other factory");
    std::vector<g::RayRecord> rays(259);
    for (size_t i = 0; i < rays.size(); ++i) {
        auto& r = rays[i];
        r.origin[0] = -10;
        r.origin[1] = r.origin[2] = -4;
        r.direction[0] = 1;
        r.hit[0] = 99;
        r.normal[0] = 7;
        r.normal[1] = -5;
        r.normal[2] = 11;
        r.color = 0xff000000u + uint32_t(i);
        r.pointNode = uint32_t(i) * 37;
        r.pointIndex = UINT32_MAX - uint32_t(i);
        r.padding = 0x7fc00001u + uint32_t(i);
        switch (i % 5) {
            case 1:
                r.origin[0] = 2;
                r.direction[0] = -1;
                break;
            case 2:
                r.origin[1] = 2;
                break;
            case 3:
                r.direction[0] = 0;
                break;
            case 4:
                r.origin[0] = std::numeric_limits<float>::quiet_NaN();
                break;
        }
    }
    const auto original = rays;
    g::RayQueryRequestDesc request;
    request.snapshot = snapshot;
    request.channel = 31;
    request.rays = rays.data();
    request.count = rays.size();
    auto bad = [&](g::RayQueryRequestDesc r, g::RayQueryPreparationBudget budget, FRESULT expected) {
        auto* out = reinterpret_cast<g::IGVDBRayQueryTicket*>(uintptr_t(1));
        require(op->prepare(r, budget, &out) == expected && !out, "ray invalid prepare clears output");
    };
    auto invalid = request;
    invalid.structSize--;
    bad(invalid, {}, FE_INVALID_ARGS);
    invalid = request;
    invalid.reserved = 1;
    bad(invalid, {}, FE_INVALID_ARGS);
    invalid = request;
    invalid.reserved2 = 1;
    bad(invalid, {}, FE_INVALID_ARGS);
    invalid = request;
    invalid.rays = nullptr;
    bad(invalid, {}, FE_INVALID_ARGS);
    invalid = request;
    invalid.snapshot = nullptr;
    bad(invalid, {}, FE_INVALID_ARGS);
    invalid = request;
    invalid.channel = 32;
    bad(invalid, {}, FE_INVALID_ARGS);
    invalid = request;
    invalid.channel = 0;
    bad(invalid, {}, FE_NOT_FOUND);
    invalid = request;
    invalid.channel = 1;
    bad(invalid, {}, FE_UNSUPPORTED);
    invalid = request;
    invalid.count = UINT32_MAX;
    bad(invalid, {}, FE_UNSUPPORTED);
    invalid = request;
    invalid.step = 0;
    bad(invalid, {}, FE_INVALID_ARGS);
    invalid = request;
    invalid.epsilon = -1;
    bad(invalid, {}, FE_INVALID_ARGS);
    invalid = request;
    invalid.maxLeafSteps = 0;
    bad(invalid, {}, FE_INVALID_ARGS);
    invalid = request;
    invalid.maxLeafVisits = 0;
    bad(invalid, {}, FE_INVALID_ARGS);
    for (int field = 0; field < 4; ++field) {
        invalid = request;
        float* parameters[] = {&invalid.step, &invalid.threshold, &invalid.epsilon, &invalid.bias};
        *parameters[field] = std::numeric_limits<float>::infinity();
        bad(invalid, {}, FE_INVALID_ARGS);
    }
    g::RayQueryPreparationBudget budget;
    budget.maxHostBytes = request.count * 160;
    budget.maxAllocatedBytes = 384 + request.count * 256;
    budget.maxBuffers = 8;
    auto low = budget;
    low.maxHostBytes--;
    bad(request, low, FE_OUT_OF_MEMORY);
    low = budget;
    low.maxAllocatedBytes--;
    bad(request, low, FE_OUT_OF_MEMORY);
    low = budget;
    low.maxBuffers--;
    bad(request, low, FE_OUT_OF_MEMORY);
    low = budget;
    low.reserved = 1;
    bad(request, low, FE_INVALID_ARGS);
    g::RayQueryTicketHandle ticket;
    check(op->prepare(request, budget, &ticket), "ray exact budgets");
    require(ticket->getDevice() == device && ticket->getCount() == rays.size(), "ray ticket identity/count");
    g::BorrowedBufferBinding binding{reinterpret_cast<IBuffer*>(uintptr_t(1)), 42, 43};
    require(
        NVRHI_FAILED(ticket->getOutputBuffer(g::RayQueryOutput::Records, binding)) && binding.byteSize == 43,
        "ray early buffer output preserved");
    require(NVRHI_FAILED(ticket->finishAfterCallerCompletion()), "ray early finish");
    g::BorrowedBytes bytes{reinterpret_cast<const void*>(uintptr_t(1)), 55};
    require(NVRHI_FAILED(ticket->getValues(g::RayQueryOutput::Records, bytes)) && bytes.byteCount == 55,
            "ray early host output preserved");
    std::memset(rays.data(), 0, rays.size() * sizeof(rays[0]));
    commands->open();
    require(NVRHI_FAILED(other->record(context, ticket)), "ray foreign operator ticket");
    require(NVRHI_FAILED(op->record({}, ticket)), "ray missing context");
    check(op->record(context, ticket), "ray record");
    require(NVRHI_FAILED(op->record(context, ticket)), "ray one use recording");
    require(NVRHI_FAILED(ticket->getOutputBuffer(g::RayQueryOutput(3), binding)), "ray invalid output kind");
    check(ticket->getOutputBuffer(g::RayQueryOutput::Records, binding), "ray raw record range");
    require(
        binding.buffer && binding.byteSize == original.size() * 64 && binding.buffer->getDesc().canHaveRawViews,
        "ray record storage");
    // Prepare queries of the old snapshot after the owner has published all-inactive topology.
    check(owner->setActive(leaf, false), "ray remove leaf");
    check(owner->finishTopology(), "ray finish removal");
    generation.uploadCount = 0;
    generation.uploads = nullptr;
    publication = nullptr;
    check(owner->prepareGeneration(generation, &publication), "ray prepare removal");
    check(owner->recordAndPublish(context, publication), "ray publish removal");
    g::SnapshotHandle emptySnapshot;
    check(owner->getSnapshot(&emptySnapshot), "ray empty snapshot");
    request.rays = original.data();
    request.count = 1;
    g::RayQueryTicketHandle limited, emptyVolume, zero, scaled;
    request.maxLeafSteps = 1;
    check(op->prepare(request, {}, &limited), "ray old snapshot after removal");
    check(op->record(context, limited), "ray record exhausted query");
    request.maxLeafSteps = 256;
    request.snapshot = emptySnapshot;
    check(op->prepare(request, {}, &emptyVolume), "ray empty topology");
    check(op->record(context, emptyVolume), "ray empty topology record");
    request.count = 0;
    request.rays = nullptr;
    check(op->prepare(request, {0, 0, 0, 0}, &zero), "ray zero allocation empty input");
    check(op->record(context, zero), "ray zero record");
    auto fast = original[0];
    fast.direction[0] = 1e7f;
    request.snapshot = snapshot;
    request.count = 1;
    request.rays = &fast;
    request.step = 2.5e-8f;
    request.epsilon = 0;
    request.bias = 0;
    request.maxLeafVisits = 1;
    check(op->prepare(request, {}, &scaled), "ray unnormalized input");
    check(op->record(context, scaled), "ray unnormalized record");
    owner = nullptr;
    snapshot = nullptr;
    emptySnapshot = nullptr;
    publication = nullptr;
    op = nullptr;
    other = nullptr;
    submit();
    for (auto t : {ticket, limited, emptyVolume, zero, scaled})
        check(t->finishAfterCallerCompletion(), "ray retained finish");
    check(ticket->getValues(g::RayQueryOutput::Records, bytes), "ray completed records");
    const auto* records = static_cast<const g::RayRecord*>(bytes.data);
    g::BorrowedBytes hitBytes, normalBytes;
    check(ticket->getValues(g::RayQueryOutput::Hits, hitBytes), "ray completed hits");
    check(ticket->getValues(g::RayQueryOutput::Normals, normalBytes), "ray completed normals");
    const auto* hits = static_cast<const g::Float4*>(hitBytes.data);
    const auto* normals = static_cast<const g::Float4*>(normalBytes.data);
    for (size_t i = 0; i < original.size(); ++i) {
        require(!std::memcmp(records[i].origin, original[i].origin, 40),
                "ray source and metadata bytes preserved");
        const bool hit = i % 5 < 2;
        require(hits[i].w == (hit ? 1.f : 0.f), "ray hit status");
        require(!std::memcmp(records[i].hit, &hits[i], 12), "ray record and hit coherence");
        if (hit) {
            const float expected = i % 5 == 0 ? -5.4999f : -.2501f;
            require(std::abs(hits[i].x - expected) < 3e-5f && hits[i].y == -4 && hits[i].z == -4,
                    "ray analytic wavefront/bias");
            require(!std::memcmp(records[i].normal, &normals[i], 12), "ray hit normal coherence");
            require(std::abs(normals[i].x - (i % 5 == 0 ? -1.f : 1.f)) < 1e-5f, "ray outward gradient");
        } else {
            require(hits[i].x == 1e10f && hits[i].y == 1e10f && hits[i].z == 1e10f, "ray legacy miss sentinel");
            require(!std::memcmp(records[i].normal, original[i].normal, 12),
                    "ray legacy miss preserves normal");
            require(normals[i].x == 0 && normals[i].y == 0 && normals[i].z == 0, "ray separate miss normal");
        }
    }
    check(limited->getValues(g::RayQueryOutput::Hits, bytes), "ray limit data");
    require(static_cast<const g::Float4*>(bytes.data)->w == -1, "ray exhaustion distinct from miss");
    check(emptyVolume->getValues(g::RayQueryOutput::Hits, bytes), "ray empty volume data");
    require(static_cast<const g::Float4*>(bytes.data)->w == 0, "ray empty volume misses");
    check(scaled->getValues(g::RayQueryOutput::Hits, bytes), "ray scaled data");
    require(static_cast<const g::Float4*>(bytes.data)->w == 1 &&
                std::abs(static_cast<const g::Float4*>(bytes.data)->x + 5.5f) < 3e-5f,
            "ray unnormalized parameter-space sampling");
    check(zero->getValues(g::RayQueryOutput::Records, bytes), "ray zero data");
    require(!bytes.data && !bytes.byteCount, "ray zero data null");
    check(zero->getOutputBuffer(g::RayQueryOutput::Hits, binding), "ray zero GPU range");
    require(!binding.buffer && !binding.byteSize, "ray zero no allocation");
    require(NVRHI_FAILED(ticket->finishAfterCallerCompletion()), "ray one use completion");
    // Validate actual exposed GPU output against completed CPU data, not just CPU metadata.
    for (auto which : {g::RayQueryOutput::Records, g::RayQueryOutput::Hits, g::RayQueryOutput::Normals}) {
        check(ticket->getOutputBuffer(which, binding), "ray retained GPU range");
        check(ticket->getValues(which, bytes), "ray retained host range");
        BufferHandle staging;
        check(device->createBuffer(BufferDesc()
                                       .setByteSize(binding.byteSize)
                                       .setCpuAccess(CpuAccessMode::Read)
                                       .setInitialState(ResourceStates::CopyDest)
                                       .setKeepInitialState(true),
                                   &staging),
              "ray test staging");
        commands->open();
        commands->copyBuffer(staging, 0, binding.buffer, binding.byteOffset, binding.byteSize);
        submit();
        const void* actual = device->mapBuffer(staging, CpuAccessMode::Read);
        require(actual && !std::memcmp(actual, bytes.data, size_t(bytes.byteCount)), "ray GPU/CPU bytes match");
        device->unmapBuffer(staging);
    }
}
}  // namespace ray_query_fixture
