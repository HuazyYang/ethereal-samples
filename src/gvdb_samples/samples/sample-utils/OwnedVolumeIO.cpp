// SPDX-License-Identifier: Apache-2.0
#include "OwnedVolumeIO.h"
#include <gvdb/GVDBNvrhiSerializer.h>
#include <nvrhi/core/datablob.h>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
namespace gvdb::sample {
namespace {
void check(nvrhi::FRESULT status,const char* operation) {
    if(NVRHI_FAILED(status))throw std::runtime_error(std::string(operation)+": "+std::to_string(status));
}
nvrhi::AutoPtr<nvrhi::IDataBlob> readBlob(const std::filesystem::path& path) {
    std::ifstream input(path,std::ios::binary|std::ios::ate);
    if(!input)throw std::runtime_error("Cannot open "+path.string());
    const auto size=input.tellg();
    if(size<0||uint64_t(size)>SIZE_MAX)throw std::runtime_error("Invalid file size: "+path.string());
    nvrhi::AutoPtr<nvrhi::IDataBlob> result;
    check(nvrhi::CreateBlob(size_t(size),&result),"allocate input blob");
    input.seekg(0);
    if(size&&!input.read(static_cast<char*>(result->GetDataPtr()),size))throw std::runtime_error("Cannot read "+path.string());
    return result;
}
portable::SnapshotHandle load(nvrhi::IDevice* device,const std::filesystem::path& artifact,nvrhi::IDataBlob* input) {
    if(!device||!input)throw std::invalid_argument("Owned VBX load requires device and input");
    const auto remap=readBlob(artifact);
    portable::VolumeCreateDesc create;create.device=device;create.configuration.levelCount=1;
    create.configuration.logDimension[0]=1;create.remapArtifact=remap->GetDataPtr();create.remapArtifactBytes=remap->GetSize();
    portable::VolumeHandle owner;check(portable::createVolume(create,&owner),"create volume owner");
    portable::SerializerCreateDesc ioDesc;ioDesc.volume=owner;
    portable::SerializerHandle io;check(portable::createSerializer(ioDesc,&io),"create serializer");
    portable::GenerationTicketHandle ticket;nvrhi::AutoPtr<nvrhi::IDataBlob> diagnostic;
    const auto status=io->prepareLoadVBX(input,{},&ticket,&diagnostic);
    if(NVRHI_FAILED(status))throw std::runtime_error(diagnostic?
        std::string(static_cast<const char*>(diagnostic->GetDataPtr()),diagnostic->GetSize()):"Cannot load VBX");
    nvrhi::CommandListHandle commands;check(device->createCommandList({},&commands),"create upload list");
    struct CloseOnExit {
        nvrhi::ICommandList* commands;bool open=false;
        ~CloseOnExit(){if(open)commands->close();}
    } recording{commands};
    commands->open();recording.open=true;
    check(owner->recordAndPublish({device,commands},ticket),"record loaded volume");
    commands->close();recording.open=false;
    const auto submission=device->executeCommandList(commands);
    if(!submission&&device->getGraphicsAPI()!=nvrhi::GraphicsAPI::D3D11)throw std::runtime_error("Submit volume upload failed");
    if(!device->waitForIdle())throw std::runtime_error("Volume upload failed");
    device->runGarbageCollection();
    portable::SnapshotHandle snapshot;check(owner->getSnapshot(&snapshot),"retain volume snapshot");
    return snapshot;
}
}
portable::SnapshotHandle loadOwnedVBX(nvrhi::IDevice* device,const std::filesystem::path& artifact,
    const std::filesystem::path& input) {
    const auto data=readBlob(input);return load(device,artifact,data);
}
portable::SnapshotHandle loadOwnedVBX(nvrhi::IDevice* device,const std::filesystem::path& artifact,
    const void* input,size_t inputBytes) {
    if(inputBytes&&!input)throw std::invalid_argument("Owned VBX input bytes missing");
    nvrhi::AutoPtr<nvrhi::IDataBlob> data;check(nvrhi::CreateBlob(inputBytes,&data),"allocate input blob");
    if(inputBytes)std::memcpy(data->GetDataPtr(),input,inputBytes);
    return load(device,artifact,data);
}
} // namespace gvdb::sample
