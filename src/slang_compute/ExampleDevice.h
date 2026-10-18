// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "ComputeRuntime.h"
#include <donut/app/DeviceManager.h>
#include <donut/app/ApplicationBase.h>
#include <donut/core/log.h>
#include <nvrhi/core/foundation.h>
#include <nvrhi/utils.h>
#if ETHEREAL_TEST_CUDA
#include <nvrhi/cuda.h>
#endif
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace ethereal::slang {
class ExampleDevice {
    nvrhi::AutoPtr<donut::app::DeviceManager> m_manager;
    nvrhi::DeviceHandle m_device;
public:
    ExampleDevice(int argc,const char** argv,bool requestRayTracing=false) {
        donut::log::ConsoleApplicationMode();
        int adapter=-1;bool cuda=false,validation=false;
        for(int i=1;i<argc;++i) {
            if(!std::strcmp(argv[i],"-cuda"))cuda=true;
            else if(!std::strcmp(argv[i],"--validation"))validation=true;
            else if(!std::strcmp(argv[i],"--adapter")) {
                if(++i>=argc)throw std::invalid_argument("--adapter requires an ordinal");
                adapter=std::stoi(argv[i]);if(adapter<0)throw std::invalid_argument("negative adapter ordinal");
            }
        }
        if(cuda) {
#if ETHEREAL_TEST_CUDA
            nvrhi::cuda::DeviceDesc desc;desc.deviceOrdinal=adapter<0?0:adapter;desc.enableOptiX=requestRayTracing;
            m_device=nvrhi::cuda::createDevice(desc);
#else
            throw std::runtime_error("This build has no CUDA backend");
#endif
        } else {
            const auto api=donut::app::GetGraphicsAPIFromCommandLine(argc,argv);
            m_manager=nvrhi::TakeOver(donut::app::DeviceManager::Create(api));
            if(!m_manager)throw std::runtime_error("Requested graphics backend was not built");
            donut::app::DeviceCreationParameters p;p.adapterIndex=adapter;p.enableNvrhiValidationLayer=true;
            p.enableDebugRuntime=validation;p.enableRayTracingExtensions=requestRayTracing;
            if(!m_manager->CreateHeadlessDevice(p))throw std::runtime_error("Cannot create requested graphics device");
            m_device=m_manager->GetDevice();
        }
        if(!m_device)throw std::runtime_error("Cannot create requested compute device");
        std::printf("Slang backend: %s\n",nvrhi::utils::GraphicsAPIToString(m_device->getGraphicsAPI()));
    }
    ~ExampleDevice() {if(m_device)m_device->waitForIdle();m_device=nullptr;if(m_manager)m_manager->Shutdown();}
    nvrhi::IDevice* get() const {return m_device;}
    std::filesystem::path rayTracingShaderPath() const {
        return donut::app::GetDirectoryWithExecutable()/"shaders/gvdb_slang"/
            (m_device->getGraphicsAPI()==nvrhi::GraphicsAPI::CUDA?"optix":
                donut::app::GetShaderTypeName(m_device->getGraphicsAPI()));
    }
    std::filesystem::path shaderPath() const {
        return donut::app::GetDirectoryWithExecutable()/"shaders/gvdb_slang"/
            donut::app::GetShaderTypeName(m_device->getGraphicsAPI());
    }
};
} // namespace ethereal::slang
