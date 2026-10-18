// Installed public SDK fixture, including no CUDA SDK headers or concrete owner.
#include "../ApronApiFixture.h"
#include <nvrhi/cuda.h>
#include <cstdio>
#include <fstream>
std::vector<char> read(const char* path){std::ifstream file(path,std::ios::binary);apron_fixture::require(bool(file),"artifact");return {(std::istreambuf_iterator<char>(file)),std::istreambuf_iterator<char>()};}
int main(int argc,char** argv){
    try{
        apron_fixture::require(argc==3,"artifact arguments");
        nvrhi::cuda::DeviceDesc desc;desc.enableOptiX=false;
        nvrhi::DeviceHandle device=nvrhi::cuda::createDevice(desc);apron_fixture::require(bool(device),"device");
        apron_fixture::run(device,read(argv[1]),read(argv[2]));
        std::puts("Installed apron: five-format boundaries, old snapshots, no-op/descriptor guards, owner-free readback PASS");return 0;
    }catch(const std::exception& error){std::fprintf(stderr,"FAIL: %s\n",error.what());return 1;}
}
