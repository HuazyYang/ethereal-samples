// SPDX-License-Identifier: Apache-2.0
#include "ApronApiFixture.h"
#include <donut/app/ApplicationBase.h>
#include <donut/app/DeviceManager.h>
#include <donut/core/log.h>
#include <nvrhi/core/foundation.h>
#if ETHEREAL_TEST_CUDA
#include <nvrhi/cuda.h>
#endif
#include <atomic>
#include <cstdio>
#include <fstream>
using namespace nvrhi;
namespace {
std::atomic_uint problems{0};
void require(bool condition, const char* text) { if (!condition) throw std::runtime_error(text); }
void check(FRESULT status, const char* text) {
    if (NVRHI_FAILED(status)) throw std::runtime_error(std::string(text) + ": " + std::to_string(status));
}
struct Messages : ObjectImpl<IMessageCallback> {
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(Messages)
        NVRHI_IMPLEMENTS_INTERFACE(IMessageCallback)
        NVRHI_IMPLEMENTS_INTERFACE(IRHIObject)
    NVRHI_END_INTERFACE_TABLE()
    void message(MessageSeverity severity, const char* text) noexcept override {
        if (severity >= MessageSeverity::Warning) ++problems;
        std::fprintf(stderr,"%s\n",text);
    }
};
struct Device {
    AutoPtr<donut::app::DeviceManager> manager;
    DeviceHandle device;
    AutoPtr<IMessageCallback> callback;
    Device(int argc, const char** argv) {
        bool useCuda = false, validation = false;
        for (int i = 1; i < argc; ++i) {
            useCuda |= !std::strcmp(argv[i],"-cuda"); validation |= !std::strcmp(argv[i],"--validation");
        }
        if (useCuda) {
#if ETHEREAL_TEST_CUDA
            cuda::DeviceDesc desc;
            desc.enableOptiX = false; callback = MAKE_RC_OBJ_PTR(Messages); desc.errorCB = callback;
            device = cuda::createDevice(desc);
#else
            throw std::runtime_error("CUDA backend unavailable");
#endif
        } else {
            manager = TakeOver(donut::app::DeviceManager::Create(donut::app::GetGraphicsAPIFromCommandLine(argc,argv)));
            require(bool(manager),"create graphics manager");
            donut::app::DeviceCreationParameters desc;
            desc.enableNvrhiValidationLayer = true; desc.enableDebugRuntime = validation;
            require(manager->CreateHeadlessDevice(desc),"create graphics device"); device = manager->GetDevice();
        }
        require(bool(device),"create test device");
    }
    ~Device() { if (device) device->waitForIdle(); device = nullptr; if (manager) manager->Shutdown(); }
};

std::vector<char> readFile(const std::filesystem::path& path){std::ifstream file(path,std::ios::binary);require(bool(file),"artifact file");return {(std::istreambuf_iterator<char>(file)),std::istreambuf_iterator<char>()};}
}
int main(int argc,const char** argv){
    donut::log::ConsoleApplicationMode();
    donut::log::SetCallback([](donut::log::Severity severity,const char* text){if(severity>=donut::log::Severity::Warning)++problems;std::fprintf(stderr,"%s\n",text);});
    try{
        {Device device(argc,argv);const auto base=donut::app::GetDirectoryWithExecutable()/"shaders";
        const auto type=donut::app::GetShaderTypeName(device.device->getGraphicsAPI());
        auto remap=base/"gvdb_owner"/type/"GVDBAtlasRepack_relocateAtlas.bin";
        auto artifact=base/"gvdb_voxel"/type/"GVDBAtlasMutation_mutateAtlas.bin";
        for(int i=1;i+1<argc;++i){if(!std::strcmp(argv[i],"--owner-artifact"))remap=argv[i+1];if(!std::strcmp(argv[i],"--mutation-artifact"))artifact=argv[i+1];}
        apron_fixture::run(device.device,readFile(remap),readFile(artifact));}
        require(problems==0,"native/NVRHI warnings or errors");std::puts("Public apron: five-format signed neighbor oracle, overrides/defaults, NaN/-0, metadata/no-op/validation/lifetime PASS");return 0;
    }catch(const std::exception& error){std::fprintf(stderr,"FAIL: %s\n",error.what());return 1;}
}
