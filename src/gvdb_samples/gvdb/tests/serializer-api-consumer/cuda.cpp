// Installed SDK serialization/lifetime fixture; no backend SDK or CPU parser includes.
#include "GVDBNvrhiSerializer.h"
#include <nvrhi/cuda.h>
#include <cstdio>
#include <fstream>
#include <vector>
namespace g = gvdb::portable;
int main(int argc, char** argv) {
    using namespace nvrhi;
    if (argc != 2) return 1;
    std::ifstream file(argv[1],std::ios::binary);
    if (!file) return 2;
    const std::vector<char> artifact((std::istreambuf_iterator<char>(file)),std::istreambuf_iterator<char>());
    cuda::DeviceDesc native; native.enableOptiX = false;
    DeviceHandle device = cuda::createDevice(native);
    if (!device) return 3;
    g::VolumeCreateDesc desc;
    desc.device = device; desc.configuration.levelCount = 1; desc.configuration.logDimension = {1};
    desc.remapArtifact = artifact.data(); desc.remapArtifactBytes = artifact.size();
    g::VolumeHandle volume;
    if (NVRHI_FAILED(g::createVolume(desc,&volume))) return 4;
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
    commands->open(); const g::RecordingContext context{device,commands};
    if (NVRHI_FAILED(volume->recordAndPublish(context,ticket))) return 9;
    g::SnapshotHandle snapshot;
    if (NVRHI_FAILED(volume->getSnapshot(&snapshot))) return 10;
    g::SerializerCreateDesc serializerDesc; serializerDesc.volume = volume;
    g::SerializerHandle serializer;
    if (NVRHI_FAILED(g::createSerializer(serializerDesc,&serializer)) || serializer->getVolume()!=volume.Get()) return 11;
    g::VbxMetadata metadata; metadata.translation = {1,2,3}; metadata.gridName[255] = 197;
    g::SaveTicketHandle save;
    if (NVRHI_FAILED(serializer->prepareSaveVBX(snapshot,&metadata,{},&save))
        || NVRHI_FAILED(serializer->recordSaveReadback(context,save))) return 12;
    volume=nullptr; serializer=nullptr; snapshot=nullptr; ticket=nullptr;
    commands->close(); device->executeCommandList(commands);
    AutoPtr<IDataBlob> blob;
    if (!device->waitForIdle() || NVRHI_FAILED(save->finishAfterCallerCompletion(&blob)) || !blob->GetSize()) return 13;
    save=nullptr;
    if (NVRHI_FAILED(g::createVolume(desc,&volume))) return 14;
    serializerDesc.volume = volume;
    if (NVRHI_FAILED(g::createSerializer(serializerDesc,&serializer))) return 15;
    if (NVRHI_FAILED(serializer->prepareLoadVBX(blob,{},&ticket))) return 16;
    blob=nullptr;
    commands->open();
    if (NVRHI_FAILED(volume->recordAndPublish(context,ticket)) || NVRHI_FAILED(volume->getSnapshot(&snapshot))) return 17;
    g::VbxMetadata loaded;
    if (NVRHI_FAILED(serializer->getMetadata(snapshot,loaded)) || loaded.translation.y!=2 || loaded.gridName[255]!=197) return 18;
    g::ReadbackHandle readback;
    if (NVRHI_FAILED(volume->prepareReadback(snapshot,1,&readback)) || NVRHI_FAILED(volume->recordReadback(context,readback))) return 19;
    volume=nullptr; serializer=nullptr; snapshot=nullptr; ticket=nullptr;
    commands->close(); device->executeCommandList(commands);
    if (!device->waitForIdle() || NVRHI_FAILED(readback->finishAfterCallerCompletion())) return 20;
    g::BorrowedBytes bytes;
    if (NVRHI_FAILED(readback->getChannelData(0,bytes)) || bytes.byteCount!=64) return 21;
    const auto* actual = static_cast<const uint8_t*>(bytes.data);
    for (uint32_t z=0;z<4;++z) for (uint32_t y=0;y<4;++y) for (uint32_t x=0;x<4;++x) {
        const bool inside=x>=1 && x<=2 && y>=1 && y<=2 && z>=1 && z<=2;
        const auto expected=inside ? uint8_t(100+(x-1)+2*(y-1)+4*(z-1)) : uint8_t(13);
        if (actual[(z*4+y)*4+x]!=expected) return 22;
    }
    readback=nullptr; commands=nullptr; device->runGarbageCollection();
    std::puts("Installed serializer: save/load, exact apron bytes, snapshot metadata and owner-free completion PASS");
    return 0;
}
