// SDK-free installed-package lifetime test. CUDA is created only through NVRHI.
#include "GVDBNvrhiVoxelApi.h"
#include <nvrhi/cuda.h>
#include <cstdio>
#include <fstream>
#include <vector>
namespace g = gvdb::portable;
int main(int argc, char** argv) {
    using namespace nvrhi;
    if (argc != 3) return 1;
    std::ifstream file(argv[1],std::ios::binary);
    if (!file) return 2;
    const std::vector<char> artifact((std::istreambuf_iterator<char>(file)),std::istreambuf_iterator<char>());
    cuda::DeviceDesc native;
    native.enableOptiX = false;
    DeviceHandle device = cuda::createDevice(native);
    if (!device) return 3;
    g::VolumeCreateDesc desc;
    desc.device = device;
    desc.configuration.levelCount = 1; desc.configuration.logDimension = {1};
    desc.remapArtifact = artifact.data(); desc.remapArtifactBytes = artifact.size();
    g::VolumeHandle volume;
    if (NVRHI_FAILED(g::createVolume(desc,&volume))) return 4;
    std::ifstream operatorsFile(argv[2],std::ios::binary);
    if(!operatorsFile)return 17;
    const std::vector<char> operators((std::istreambuf_iterator<char>(operatorsFile)),std::istreambuf_iterator<char>());
    g::VoxelOpsCreateDesc operatorsDesc;
    operatorsDesc.volume=volume;operatorsDesc.artifact=operators.data();operatorsDesc.artifactBytes=operators.size();
    g::VoxelOpsHandle ops;
    if(NVRHI_FAILED(g::createVoxelOps(operatorsDesc,&ops)) || ops->getVolume()!=volume.Get())return 18;
    g::NodeId leaf;
    if (NVRHI_FAILED(volume->activate(0,0,0,leaf)) || NVRHI_FAILED(volume->finishTopology())) return 5;
    if (NVRHI_FAILED(volume->setChannel(0,{g::AtlasFormat::R8_UINT,{13,0,0,0}}))) return 6;
    std::vector<uint8_t> upload(64,233);
    for (uint32_t z=0;z<2;++z) for (uint32_t y=0;y<2;++y) for (uint32_t x=0;x<2;++x)
        upload[((z+1)*4+y+1)*4+x+1] = uint8_t(100+x+2*y+4*z);
    const g::ChannelUploadDesc source{0,0,upload.data(),upload.size()};
    g::GenerationDesc request;
    request.placement.minimumBrickCount = {1,1,1}; request.uploads = &source; request.uploadCount = 1;
    g::GenerationTicketHandle ticket;
    if (NVRHI_FAILED(volume->prepareGeneration(request,&ticket))) return 7;
    CommandListHandle commands;
    if (NVRHI_FAILED(device->createCommandList({},&commands))) return 8;
    commands->open();
    const g::RecordingContext context{device,commands};
    if (NVRHI_FAILED(volume->recordAndPublish(context,ticket))) return 9;
    g::GenerationTicketHandle fill,apron;
    if(NVRHI_FAILED(ops->prepareFill(0,{17,0,0,0},{},&fill)) || NVRHI_FAILED(volume->recordAndPublish(context,fill)))return 19;
    if(NVRHI_FAILED(ops->prepareApron(1,{},&apron)) || NVRHI_FAILED(volume->recordAndPublish(context,apron)))return 20;
    g::SnapshotHandle snapshot;
    if (NVRHI_FAILED(volume->getSnapshot(&snapshot))) return 10;
    g::HostVolumeHandle host;
    if (NVRHI_FAILED(volume->QueryInterface(uuid_of<g::IGVDBHostVolume>(),reinterpret_cast<void**>(host.GetAddressOf())))) return 11;
    if (host->getActiveLeafCount()!=1 || snapshot->getDesc().channelMask!=1) return 12;
    g::ReadbackHandle readback;
    if (NVRHI_FAILED(volume->prepareReadback(snapshot,1,&readback)) || NVRHI_FAILED(volume->recordReadback(context,readback))) return 13;
    volume = nullptr; host = nullptr; snapshot = nullptr; ticket = nullptr;
    fill=nullptr;apron=nullptr;ops=nullptr;
    commands->close(); device->executeCommandList(commands);
    if (!device->waitForIdle() || NVRHI_FAILED(readback->finishAfterCallerCompletion())) return 14;
    g::BorrowedBytes bytes;
    if (NVRHI_FAILED(readback->getChannelData(0,bytes)) || !bytes.data || bytes.byteCount!=64) return 15;
    const auto* actual = static_cast<const uint8_t*>(bytes.data);
    for (uint32_t z=0;z<4;++z) for (uint32_t y=0;y<4;++y) for (uint32_t x=0;x<4;++x) {
        const bool inside = x>=1 && x<=2 && y>=1 && y<=2 && z>=1 && z<=2;
        const uint8_t expected = inside ? uint8_t(17) : 13;
        if (actual[(z*4+y)*4+x]!=expected) return 16;
    }
    readback=nullptr; commands=nullptr; device->runGarbageCollection();
    std::puts("Installed GVDB voxel API: creation/QI, fill/apron and retained readback across library boundary PASS");
    return 0;
}
