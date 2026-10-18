// SPH wave pool on NVRHI graphics compute or CUDA, with offline Slang shaders.
// SPDX-License-Identifier: Apache-2.0
#include "PortablePool.h"
#include "ExampleDevice.h"
#include <donut/engine/ShaderFactory.h>
#include <donut/core/vfs/VFS.h>
#include <nvrhi/utils.h>
#include <GLFW/glfw3.h>
#include <stb_image_write.h>
#include <chrono>
#include <fstream>
#include <memory>
#include <algorithm>
using namespace donut;
namespace g=gvdb::slang;
namespace es=ethereal::slang;
namespace {
void check(nvrhi::FRESULT rc,const char* action) {
    if(NVRHI_FAILED(rc))throw std::runtime_error(std::string(action)+" failed: "+std::to_string(rc));
}
struct Options {
    uint32_t capacity=4000000,steps=120,checkStep=0;
    int example=2,adapter=-1;bool headless=false,paused=false,validation=false,cuda=false;
    std::string screenshot,csv;
};
g::Float4 vector4(dm::float3 v,float w=0){return {v.x,v.y,v.z,w};}
struct ViewParameters {g::Float4 eye,right,up,forward,projection,extent;};
void report(g::SlangSphSimulation& simulation) {
    const auto points=simulation.readPositions(),velocities=simulation.readVelocities();
    dm::float3 lower(INFINITY),upper(-INFINITY);double speed=0;
    for(size_t i=0;i<points.size();++i) {
        const auto& p=points[i];const auto& v=velocities[i];
        if(!std::isfinite(p.x)||!std::isfinite(p.y)||!std::isfinite(p.z)||
           !std::isfinite(v.x)||!std::isfinite(v.y)||!std::isfinite(v.z))
            throw std::runtime_error("SPH generated a nonfinite particle");
        if(p.x==-1000&&p.y==-1000&&p.z==-1000)throw std::runtime_error("SPH particle left the acceleration grid");
        lower=dm::min(lower,dm::float3(p.x,p.y,p.z));upper=dm::max(upper,dm::float3(p.x,p.y,p.z));
        speed=std::max(speed,std::sqrt(double(v.x)*v.x+double(v.y)*v.y+double(v.z)*v.z));
    }
    std::printf("%zu particles; time %.6f; bounds [%g,%g,%g]..[%g,%g,%g]; maximum speed %.6g\n",
        points.size(),simulation.parameters().integration.x,lower.x,lower.y,lower.z,upper.x,upper.y,upper.z,speed);
}
void writeCsv(g::SlangSphSimulation& simulation,const std::string& path) {
    if(path.empty())return;
    const auto points=simulation.readPositions(),velocities=simulation.readVelocities();
    std::ofstream out(path);if(!out)throw std::runtime_error("Cannot create particle CSV");
    out.precision(9);out<<"x,y,z,vx,vy,vz\n";
    for(size_t i=0;i<points.size();++i)out<<points[i].x<<','<<points[i].y<<','<<points[i].z<<','
        <<velocities[i].x<<','<<velocities[i].y<<','<<velocities[i].z<<'\n';
    if(!out)throw std::runtime_error("Cannot write particle CSV");
}
class PoolView:public app::IRenderPass {
    NVRHI_INHERIT_INTERFACE_TABLE()
    Options m_options;
    sph::PortablePool m_initial;
    es::ComputeRuntime m_runtime;
    std::unique_ptr<g::SlangSphSimulation> m_simulation;
    nvrhi::ShaderHandle m_vs,m_ps;
    nvrhi::BindingLayoutHandle m_layout;
    nvrhi::GraphicsPipelineHandle m_pipeline;
    nvrhi::BufferHandle m_constants;
    nvrhi::CommandListHandle m_commands;
    dm::float3 m_center;
    float m_distance,m_yaw=0.7f,m_pitch=0.55f;
    uint32_t m_frame=0;bool m_paused;
public:
    PoolView(app::DeviceManager* manager,const Options& options)
        :IRenderPass(manager),m_options(options),m_initial(options.capacity,sph::SPHExample(options.example)),
        m_runtime(GetDevice(),app::GetDirectoryWithExecutable()/"shaders/gvdb_slang"/app::GetShaderTypeName(GetDevice()->getGraphicsAPI())),
        m_paused(options.paused) {
        reset();
        dm::float3 lower(INFINITY),upper(-INFINITY);
        for(const auto& p:m_initial.positions){lower=dm::min(lower,dm::float3(p.x,p.y,p.z));upper=dm::max(upper,dm::float3(p.x,p.y,p.z));}
        m_center=(lower+upper)*.5f;m_distance=std::max(10.f,dm::length(upper-lower)*1.6f);
        auto fs=MAKE_RC_OBJ_PTR(vfs::NativeFileSystem);
        engine::ShaderFactory factory(GetDevice(),fs,app::GetDirectoryWithExecutable()/"shaders/sph_fluid_pool"/app::GetShaderTypeName(GetDevice()->getGraphicsAPI()));
        m_vs=factory.CreateShader("Points.slang","main_vs",nullptr,nvrhi::ShaderType::Vertex);
        m_ps=factory.CreateShader("Points.slang","main_ps",nullptr,nvrhi::ShaderType::Pixel);
        if(!m_vs||!m_ps)throw std::runtime_error("Cannot load particle graphics artifacts");
        nvrhi::BindingLayoutDesc layout;layout.visibility=nvrhi::ShaderType::Vertex;
        layout.bindings={nvrhi::BindingLayoutItem::ConstantBuffer(0),nvrhi::BindingLayoutItem::StructuredBuffer_SRV(0),
            nvrhi::BindingLayoutItem::StructuredBuffer_SRV(1)};
        check(GetDevice()->createBindingLayout(layout,&m_layout),"create particle binding layout");
        nvrhi::BufferDesc desc;desc.byteSize=256;desc.isConstantBuffer=true;
        desc.initialState=nvrhi::ResourceStates::ConstantBuffer;desc.keepInitialState=true;
        check(GetDevice()->createBuffer(desc,&m_constants),"create view constants");
        check(GetDevice()->createCommandList(nvrhi::CommandListParameters(),&m_commands),"create particle commands");
    }
    void reset(){m_runtime.flush();m_simulation=std::make_unique<g::SlangSphSimulation>(m_runtime,m_initial.parameters,m_initial.positions);m_frame=0;}
    g::SlangSphSimulation& simulation(){return *m_simulation;}
    void step(uint32_t count){m_simulation->step(count);m_runtime.flush();m_frame+=count;}
    void BackBufferResizing()override{m_pipeline=nullptr;}
    bool KeyboardUpdate(int key,int,int action,int)override{
        if(action!=GLFW_PRESS&&action!=GLFW_REPEAT)return false;
        if(key==GLFW_KEY_SPACE)m_paused=!m_paused;
        else if(key==GLFW_KEY_R)reset();
        else if(key==GLFW_KEY_RIGHT)m_yaw+=.08f;
        else if(key==GLFW_KEY_LEFT)m_yaw-=.08f;
        else if(key==GLFW_KEY_UP)m_pitch=std::min(1.4f,m_pitch+.08f);
        else if(key==GLFW_KEY_DOWN)m_pitch=std::max(-1.4f,m_pitch-.08f);
        else return false;
        return true;
    }
    bool MouseScrollUpdate(double,double y)override{m_distance=std::max(1.f,m_distance*float(std::exp(-y*.1)));return true;}
    void Animate(float)override{
        if(!m_paused){step(1);if(m_options.checkStep&&m_frame==m_options.checkStep)report(*m_simulation);}
        GetDeviceManager()->SetInformativeWindowTitle(m_paused?"Slang SPH ? paused (Space/R/arrows/wheel)":"Slang SPH ? wave pool (Space/R/arrows/wheel)");
    }
    void Render(nvrhi::IFramebuffer* framebuffer)override{
        m_runtime.flush();
        const auto viewport=framebuffer->getFramebufferInfo().getViewport();
        if(!m_pipeline){
            nvrhi::GraphicsPipelineDesc p;p.VS=m_vs;p.PS=m_ps;p.bindingLayouts={m_layout};
            p.primType=nvrhi::PrimitiveType::TriangleList;p.renderState.rasterState.cullMode=nvrhi::RasterCullMode::None;
            p.renderState.depthStencilState.depthTestEnable=false;p.renderState.depthStencilState.depthWriteEnable=false;
            check(GetDevice()->createGraphicsPipeline1(p,framebuffer->getFramebufferInfo().getInfo(),&m_pipeline),"create particle pipeline");
        }
        const dm::float3 backwards(std::sin(m_yaw)*std::cos(m_pitch),std::sin(m_pitch),std::cos(m_yaw)*std::cos(m_pitch));
        const auto forward=-backwards,right=dm::normalize(dm::cross(forward,dm::float3(0,1,0))),up=dm::cross(right,forward);
        ViewParameters view{vector4(m_center+backwards*m_distance),vector4(right),vector4(up),vector4(forward),
            {0.65f,0.65f*viewport.height()/viewport.width(),1.5f,0.1f},{viewport.width(),viewport.height(),.05f,0}};
        uint8_t padded[256]{};std::memcpy(padded,&view,sizeof(view));
        nvrhi::BindingSetDesc bindings;bindings.bindings={nvrhi::BindingSetItem::ConstantBuffer(0,m_constants),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(0,m_simulation->positions()),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(1,m_simulation->velocities())};
        nvrhi::BindingSetHandle set;check(GetDevice()->createBindingSet(bindings,m_layout,&set),"bind simulation to rasterizer");
        m_commands->open();m_commands->writeBuffer(m_constants,padded,sizeof(padded));
        nvrhi::utils::ClearColorAttachment(m_commands,framebuffer,0,nvrhi::Color(.025f,.04f,.07f,1));
        nvrhi::GraphicsState state;state.pipeline=m_pipeline;state.framebuffer=framebuffer;state.bindings={set};
        state.viewport.addViewportAndScissorRect(viewport);m_commands->setGraphicsState(state);
        nvrhi::DrawArguments args;args.vertexCount=6;args.instanceCount=uint32_t(m_initial.positions.size());m_commands->draw(args);
        m_commands->close();GetDevice()->executeCommandList(m_commands);
    }
};
void renderOffscreen(PoolView& view,nvrhi::IDevice* device,const std::string& path) {
    nvrhi::TextureDesc desc;desc.width=640;desc.height=480;desc.format=nvrhi::Format::RGBA8_UNORM;
    desc.isRenderTarget=true;desc.initialState=nvrhi::ResourceStates::RenderTarget;desc.keepInitialState=true;
    nvrhi::TextureHandle color;check(device->createTexture(desc,&color),"create particle render target");
    nvrhi::FramebufferHandle fb;check(device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(color),&fb),"create framebuffer");
    view.Render(fb);
    nvrhi::StagingTextureHandle staging;check(device->createStagingTexture(desc,nvrhi::CpuAccessMode::Read,&staging),"create image readback");
    nvrhi::CommandListHandle commands;check(device->createCommandList(nvrhi::CommandListParameters(),&commands),"create readback commands");
    commands->open();commands->copyTexture2(staging,{},color,{});commands->close();device->executeCommandList(commands);device->waitForIdle();
    size_t pitch=0;const auto pixels=static_cast<const uint8_t*>(device->mapStagingTexture(staging,{},nvrhi::CpuAccessMode::Read,pitch));
    if(!pixels)throw std::runtime_error("Cannot read particle image");
    size_t covered=0;
    for(uint32_t y=0;y<desc.height;++y)for(uint32_t x=0;x<desc.width;++x){
        const auto p=pixels+y*pitch+x*4;if(p[0]>20||p[1]>25||p[2]>35)++covered;
    }
    const bool saved=path.empty()||stbi_write_png(path.c_str(),desc.width,desc.height,4,pixels,int(pitch))!=0;
    device->unmapStagingTexture(staging);
    if(!saved||!covered)throw std::runtime_error("Particle image is empty or cannot be written");
    std::printf("Rendered %zu particle pixels directly from GPU simulation buffers\n",covered);
}
}
int main(int argc,const char** argv){
    try{
        Options options;
        for(int i=1;i<argc;++i){
            const auto value=[&](){if(++i>=argc)throw std::invalid_argument("Missing option value");return argv[i];};
            if(!std::strcmp(argv[i],"--help")){std::puts("sph_fluid_pool [-dx11|-dx12|-vk|-cuda] [--headless] [--adapter N] [--particles N] [--example 0..3]\n"
                "  [--frames N] [--check N] [--paused] [--screenshot image.png] [--output particles.csv]\n"
                "  Interactive: Space pause, R restart, arrow keys orbit, mouse wheel zoom. CUDA runs headless.");return 0;}
            if(!std::strcmp(argv[i],"--particles"))options.capacity=uint32_t(std::stoul(value()));
            else if(!std::strcmp(argv[i],"--example"))options.example=std::stoi(value());
            else if(!std::strcmp(argv[i],"--adapter")){options.adapter=std::stoi(value());if(options.adapter<0)throw std::invalid_argument("Negative adapter ordinal");}
            else if(!std::strcmp(argv[i],"--frames"))options.steps=uint32_t(std::stoul(value()));
            else if(!std::strcmp(argv[i],"--check"))options.checkStep=uint32_t(std::stoul(value()));
            else if(!std::strcmp(argv[i],"--headless"))options.headless=true;
            else if(!std::strcmp(argv[i],"--paused"))options.paused=true;
            else if(!std::strcmp(argv[i],"--validation"))options.validation=true;
            else if(!std::strcmp(argv[i],"-cuda"))options.cuda=true;
            else if(!std::strcmp(argv[i],"--screenshot")){options.screenshot=value();options.headless=true;}
            else if(!std::strcmp(argv[i],"--output"))options.csv=value();
        }
        if(options.example<0||options.example>3||options.steps>1000000)throw std::invalid_argument("Invalid preset or frame count");
        if(options.cuda){
            if(!options.screenshot.empty())throw std::invalid_argument("CUDA has no rasterizer; select a graphics API for screenshots");
            es::ExampleDevice device(argc,argv);es::ComputeRuntime runtime(device.get(),device.shaderPath());
            sph::PortablePool pool(options.capacity,sph::SPHExample(options.example));
            g::SlangSphSimulation simulation(runtime,pool.parameters,pool.positions);
            if(!options.paused)simulation.step(options.steps);
            report(simulation);writeCsv(simulation,options.csv);return 0;
        }
        log::ConsoleApplicationMode();
        auto manager=nvrhi::TakeOver(app::DeviceManager::Create(app::GetGraphicsAPIFromCommandLine(argc,argv)));
        if(!manager)throw std::runtime_error("Requested graphics API is unavailable");
        app::DeviceCreationParameters p;p.enableNvrhiValidationLayer=true;p.enableDebugRuntime=options.validation;p.adapterIndex=options.adapter;
        const bool created=options.headless?manager->CreateHeadlessDevice(p):manager->CreateWindowDeviceAndSwapChain(p,"Slang SPH wave pool");
        if(!created)throw std::runtime_error("Cannot create graphics device");
        {
            PoolView view(manager,options);
            if(options.headless){
                if(!options.paused)view.step(options.steps);
                report(view.simulation());renderOffscreen(view,manager->GetDevice(),options.screenshot);
            }else{manager->AddRenderPassToBack(&view);manager->RunMessageLoop();manager->RemoveRenderPass(&view);}
            writeCsv(view.simulation(),options.csv);manager->GetDevice()->waitForIdle();
        }
        manager->Shutdown();return 0;
    }catch(const std::exception& error){std::fprintf(stderr,"sph_fluid_pool: %s\n",error.what());return 1;}
}
