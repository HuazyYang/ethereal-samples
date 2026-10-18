// Uses only the installed public SDK; no CUDA SDK headers or concrete owner.
#include "../DownsampleApiGoldens.h"
#include <nvrhi/cuda.h>
#include <cstdio>
#include <fstream>
#include <stdexcept>
namespace g=gvdb::portable;
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void check(nvrhi::FRESULT result,const char* message){require(NVRHI_SUCCEEDED(result),message);}
int main(int argc,char** argv){
    try{
        require(argc==2,"artifact argument");std::ifstream file(argv[1],std::ios::binary);require(bool(file),"artifact");
        std::vector<char> artifact{std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};
        nvrhi::cuda::DeviceDesc desc;desc.enableOptiX=false;
        nvrhi::DeviceHandle device=nvrhi::cuda::createDevice(desc);require(bool(device),"device");
        g::DownsampleCreateDesc create;create.device=device;create.artifact=artifact.data();create.artifactBytes=artifact.size();
        g::DownsampleHandle operation;check(g::createDownsample(create,&operation),"factory");
        nvrhi::CommandListHandle commands;check(device->createCommandList({},&commands),"command list");commands->open();
        auto goldens=downsample_fixture::goldens();std::vector<g::DownsampleTicketHandle> tickets;
        for(auto& golden:goldens){
            g::DownsampleTicketHandle ticket;check(operation->prepare(golden.request(),{},&ticket),"prepare");
            std::fill(golden.source.begin(),golden.source.end(),-999.0f); // Prepared data must be independent.
            check(operation->record({device,commands},ticket),"record");tickets.push_back(std::move(ticket));
        }
        commands->close();operation.Reset();artifact.clear(); // Completion retains every resource independently.
        device->executeCommandList(commands);device->waitForIdle();
        for(size_t i=0;i<tickets.size();++i){
            g::BorrowedBytes bytes;check(tickets[i]->finishAfterCallerCompletion(bytes),"finish");
            require(bytes.byteCount==goldens[i].expected.size()*4,"result size");const auto* values=static_cast<const float*>(bytes.data);
            for(size_t j=0;j<goldens[i].expected.size();++j){
                const auto expected=goldens[i].expected[j];bool matches=false;
                switch(expected.kind){
                case downsample_fixture::Expected::Exact:matches=downsample_fixture::bits(values[j])==expected.bitPattern;break;
                case downsample_fixture::Expected::NaN:matches=std::isnan(values[j]);break;
                case downsample_fixture::Expected::PositiveInfinity:matches=std::isinf(values[j])&&values[j]>0;break;
                case downsample_fixture::Expected::NegativeInfinity:matches=std::isinf(values[j])&&values[j]<0;break;
                }
                if(!matches){std::fprintf(stderr,"golden %s [%zu] got %08x expected %08x\n",goldens[i].name.c_str(),j,downsample_fixture::bits(values[j]),expected.bitPattern);throw std::runtime_error("independent golden");}
            }
            g::BorrowedBytes repeated;check(tickets[i]->getValues(repeated),"repeat values");require(repeated.data==bytes.data&&repeated.byteCount==bytes.byteCount,"retained bytes");
            require(tickets[i]->finishAfterCallerCompletion(repeated)==nvrhi::FE_INVALID_ARGS&&repeated.data==bytes.data,"one finish/output preservation");
        }
        std::printf("Installed downsample: %zu exact/classification goldens, copied input, owner-free completion PASS\n",goldens.size());return 0;
    }catch(const std::exception& error){std::fprintf(stderr,"FAIL: %s\n",error.what());return 1;}
}
