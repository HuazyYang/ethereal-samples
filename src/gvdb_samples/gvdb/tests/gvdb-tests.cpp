// Tests of the GVDB library on the nvrhi CUDA backend, without a graphics device:
//   primitives  prefix sum, the 64-bit key sort and min/max (GVDBPrimitives.h) against the CPU
//   volume      a topology built from points on the GPU, a channel filled, smoothed and reduced
// usage: gvdb_tests <test>
#include <gvdb/GVDB.h>
#include "../GVDBPrimitives.h"
#include <nvrhi/factory.h>
#include <donut/app/ApplicationBase.h>
#include <donut/core/vfs/VFS.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

using namespace donut;

namespace {

void check(bool condition, const std::string &what) {
    if (!condition) throw std::runtime_error(what);
}

struct Messages : nvrhi::ObjectImpl<nvrhi::IMessageCallback> {
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(Messages)
    NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IMessageCallback)
    NVRHI_END_INTERFACE_TABLE()

    int errors = 0;
    void message(nvrhi::MessageSeverity severity, const char *text) noexcept override {
        if (severity == nvrhi::MessageSeverity::Error || severity == nvrhi::MessageSeverity::Fatal) ++errors;
        std::printf("[nvrhi] %s\n", text);
    }
};

struct Context {
    nvrhi::AutoPtr<Messages> messages = MAKE_RC_OBJ_PTR(Messages);
    nvrhi::cuda::DeviceHandle device;
    nvrhi::AutoPtr<gvdb::IComputeContext> compute;

    Context() {
        nvrhi::GraphicsFactoryDesc factoryDesc;
        factoryDesc.messageCallback = messages;
        nvrhi::GraphicsFactoryHandle factory = nvrhi::createGraphicsFactory(factoryDesc);
        check(factory != nullptr, "create the graphics factory");
        nvrhi::cuda::DeviceDesc desc;
        desc.messageCallback = messages;
        desc.enableOptix = false;
        check(NVRHI_SUCCEEDED(factory->createCudaDevice(desc, &device)) && device, "create the CUDA device");

        auto root = MAKE_RC_OBJ_PTR(vfs::RootFileSystem);
        root->mount("shaders/gvdb", app::GetDirectoryWithExecutable() / "shaders" / "gvdb");
        check(NVRHI_SUCCEEDED(gvdb::createComputeContext(device, root, &compute)), "create the compute context");
    }

    nvrhi::BufferHandle upload(const void *data, uint64_t bytes) {
        nvrhi::BufferHandle buffer;
        nvrhi::BufferDesc desc;
        desc.byteSize = std::max<uint64_t>(bytes, 16);
        desc.debugName = "test data";
        check(NVRHI_SUCCEEDED(device->createBuffer(desc, &buffer)), "create a buffer");
        if (bytes) compute->getCommandList()->writeBuffer(buffer, data, bytes, 0);
        return buffer;
    }
};

void testPrimitives(Context &ctx) {
    gvdb::GVDBPrimitives primitives(ctx.compute);
    std::mt19937_64 random(1234);

    for (uint32_t count : {1u, 7u, 1024u, 1025u, 3000u, (1u << 20) + 5u}) {
        std::vector<uint32_t> values(count);
        for (auto &v : values) v = uint32_t(random() % 11);
        nvrhi::BufferHandle input = ctx.upload(values.data(), count * 4ull);
        nvrhi::BufferHandle output = ctx.upload(nullptr, count * 4ull);
        primitives.prefixSum(input, output, count);
        std::vector<uint32_t> result(count);
        ctx.compute->readBuffer(output, 0, result.data(), count * 4ull);
        uint32_t sum = 0;
        for (uint32_t i = 0; i < count; ++i) {
            check(result[i] == sum, "prefix sum of " + std::to_string(count) + " at " + std::to_string(i) + ": " +
                                        std::to_string(result[i]) + ", expected " + std::to_string(sum));
            sum += values[i];
        }
    }
    std::printf("primitives: prefix sums of 1 .. 1M + 5 elements OK\n");

    for (uint32_t count : {1u, 2u, 255u, 256u, 257u, 100000u, 1u << 20}) {
        // Keys like the brick keys of GVDB (16-bit z, y, x, 8-bit level) with duplicates, and arbitrary ones.
        std::vector<uint64_t> keys(count);
        for (uint32_t i = 0; i < count; ++i) {
            keys[i] = (i % 3 == 0) ? random()
                                   : (uint64_t(random() % 8) << 48) | (uint64_t(random() % 64) << 32) |
                                         (uint64_t(random() % 64) << 16) | uint64_t(random() % 64);
        }
        nvrhi::BufferHandle buffer = ctx.upload(keys.data(), count * 8ull);
        primitives.sortKeys64(buffer, count);
        std::vector<uint64_t> result(count);
        ctx.compute->readBuffer(buffer, 0, result.data(), count * 8ull);
        std::sort(keys.begin(), keys.end());
        check(result == keys, "sort of " + std::to_string(count) + " 64-bit keys");
    }
    std::printf("primitives: radix sorts of 1 .. 1M 64-bit keys OK\n");

    for (uint32_t count : {1u, 1000u, 300000u}) {
        std::vector<float> values(count);
        std::uniform_real_distribution<float> dist(-1000.f, 1000.f);
        for (auto &v : values) v = dist(random);
        nvrhi::BufferHandle buffer = ctx.upload(values.data(), count * 4ull);
        float mn = 0.f, mx = 0.f;
        primitives.minMax(buffer, count, mn, mx);
        const auto expected = std::minmax_element(values.begin(), values.end());
        check(mn == *expected.first && mx == *expected.second, "min / max of " + std::to_string(count) + " floats");
    }
    std::printf("primitives: min / max OK\n");
}

void testVolume(Context &ctx) {
    nvrhi::AutoPtr<gvdb::IGVDBVolume> volume;
    check(NVRHI_SUCCEEDED(gvdb::createGVDBVolume(ctx.compute, &volume)), "create the volume");
    volume->configure(gvdb::GVDBLevelConfig::fromBranching(3, 3, 3, 3, 3));
    gvdb::GVDBChannelDesc channelDesc;
    channelDesc.format = gvdb::ATLAS_FORMAT_R32_FLOAT;
    check(volume->addChannel(channelDesc) == 0, "add a channel");

    // Points on a sphere of radius 60 around (100, 100, 100).
    std::mt19937 random(42);
    std::uniform_real_distribution<float> dist(-1.f, 1.f);
    std::vector<dm::float3> points;
    while (points.size() < 20000) {
        dm::float3 d(dist(random), dist(random), dist(random));
        if (dm::length(d) < 0.1f || dm::length(d) > 1.f) continue;
        points.push_back(dm::float3(100.f) + dm::normalize(d) * 60.f);
    }
    nvrhi::BufferHandle pointBuffer = ctx.upload(points.data(), points.size() * sizeof(dm::float3));

    nvrhi::AutoPtr<gvdb::IGVDBPointOps> pointOps;
    check(NVRHI_SUCCEEDED(gvdb::createGVDBPointOps(volume, &pointOps)), "create the point operations");
    gvdb::PointData pos;
    pos.buffer = pointBuffer;
    pos.stride = sizeof(dm::float3);
    pos.count = uint32_t(points.size());
    pointOps->setPoints(pos, {}, {});
    pointOps->rebuildTopology(uint32_t(points.size()), 2.f, dm::float3::zero());
    volume->finishTopology(true);
    volume->updateAtlas();

    const uint64_t bricks = volume->getNumActiveBricks();
    check(bricks > 0, "the points activate bricks");
    for (const dm::float3 &p : points)
        check(volume->isActive(dm::int3(int(std::floor(p.x)), int(std::floor(p.y)), int(std::floor(p.z)))),
              "every point lies in an active brick");
    std::printf("volume: %llu active bricks hold all %zu points\n", (unsigned long long)bricks, points.size());

    // A constant channel: the reduction sums every voxel of the packed atlas; smoothing keeps the constant.
    nvrhi::AutoPtr<gvdb::IGVDBVoxelOps> voxelOps;
    check(NVRHI_SUCCEEDED(gvdb::createGVDBVoxelOps(volume, &voxelOps)), "create the voxel operations");
    voxelOps->fillChannel(0, dm::float4(1.f, 0.f, 0.f, 0.f));
    voxelOps->updateApron(0, 1.f);
    const dm::uint3 packed = volume->getAtlasPackedResDim();
    const double voxels = double(packed.x) * packed.y * packed.z;
    float sum = voxelOps->reduction(0);
    check(std::abs(sum - voxels) < 1e-3 * voxels, "the reduction of a constant 1 is the voxel count: " +
                                                      std::to_string(sum) + ", expected " + std::to_string(voxels));
    voxelOps->compute(gvdb::ComputeOp::Smooth, 0, 2, dm::float3(1.f, 0.f, 0.f), true, true, 1.f);
    sum = voxelOps->reduction(0);
    check(std::abs(sum - voxels) < 1e-3 * voxels, "smoothing keeps a constant channel: " + std::to_string(sum));
    std::printf("volume: fill, apron, smooth and reduction over %.0f voxels OK\n", voxels);
}

}  // namespace

int main(int argc, const char **argv) {
    try {
        check(argc >= 2, "usage: gvdb_tests <primitives|volume>");
        const std::string test = argv[1];
        Context ctx;
        if (test == "primitives")
            testPrimitives(ctx);
        else if (test == "volume")
            testVolume(ctx);
        else
            throw std::runtime_error("unknown test " + test);
        ctx.compute->finish();
        check(ctx.messages->errors == 0, std::to_string(ctx.messages->errors) + " error messages");
        std::printf("PASS: %s\n", test.c_str());
        return 0;
    } catch (const std::exception &e) {
        std::printf("FAIL: %s\n", e.what());
        return 1;
    }
}
