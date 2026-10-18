// VBX file format from NVIDIA GVDB, rewritten as validated CPU-only I/O.
// Copyright 2017 NVIDIA Corporation. SPDX-License-Identifier: Apache-2.0
#include "GVDBPortableVbx.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <utility>
namespace gvdb::slang {
namespace {
void require(bool condition,const char* message) {if(!condition)throw std::runtime_error(message);}
uint32_t u32(const uint8_t* p) {return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);}
uint64_t u64(const uint8_t* p) {return uint64_t(u32(p))|(uint64_t(u32(p+4))<<32);}
uint64_t normalizeReference(uint64_t ref) {return ref==0xffffffffull?UINT64_MAX:ref;}
void setReference(uint32_t* words,uint64_t ref) {words[0]=uint32_t(ref);words[1]=uint32_t(ref>>32);}
uint64_t getReference(const uint32_t* words) {return uint64_t(words[0])|(uint64_t(words[1])<<32);}
struct Reader {
    const uint8_t* data;size_t size,offset=0;
    const uint8_t* take(uint64_t count) {
        require(count<=size-offset,"truncated VBX data");
        const auto* result=data+offset;offset+=size_t(count);return result;
    }
    uint8_t byte(){return *take(1);}
    uint32_t uint(){return u32(take(4));}
    int32_t integer(){return int32_t(uint());}
    uint64_t ulong(){return u64(take(8));}
    float real(){uint32_t bits=uint();float value;std::memcpy(&value,&bits,4);return value;}
    VbxFloat3 vector(){return {real(),real(),real()};}
    Int4 intVector(){return {integer(),integer(),integer(),0};}
};
struct Writer {
    std::vector<uint8_t> bytes;
    void append(const void* data,size_t count){if(!count)return;const auto* p=static_cast<const uint8_t*>(data);bytes.insert(bytes.end(),p,p+count);}
    void byte(uint8_t value){bytes.push_back(value);}
    void uint(uint32_t value){for(unsigned i=0;i<4;++i)byte(uint8_t(value>>(8*i)));}
    void integer(int32_t value){uint(uint32_t(value));}
    void ulong(uint64_t value){uint(uint32_t(value));uint(uint32_t(value>>32));}
    void real(float value){uint32_t bits;std::memcpy(&bits,&value,4);uint(bits);}
    void vector(VbxFloat3 value){real(value.x);real(value.y);real(value.z);}
    void intVector(uint32_t x,uint32_t y,uint32_t z){uint(x);uint(y);uint(z);}
};
struct DiskLevel {
    int32_t logDimension,resolution;
    Int4 extent;
    int32_t nodes,nodeStride,lists,listStride;
    const uint8_t* nodeData=nullptr;
    const uint8_t* listData=nullptr;
};
void checkTransform(const VbxTransform& x) {
    const VbxFloat3 values[]={x.preTranslation,x.angles,x.scale,x.translation};
    for(const auto& v:values)require(std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z),"VBX transform contains nonfinite values");
}
void checkVoxelSize(VbxFloat3 value) {require(std::isfinite(value.x)&&std::isfinite(value.y)&&std::isfinite(value.z)&&value.x>0&&value.y>0&&value.z>0,"VBX voxel size is invalid");}
VolumeSource sourceFromPacked(const PackedVolume& packed) {
    VolumeSource source;source.volume=packed.volume;
    for(const auto& level:packed.levels) {
        require(uint64_t(level.nodes.x)+uint64_t(level.nodes.y)*level.nodes.z<=uint64_t(packed.nodes.size())*4,"packed node pool exceeds its storage");
        require(uint64_t(level.children.x)+uint64_t(level.children.y)*level.children.z<=uint64_t(packed.children.size())*4,"packed child pool exceeds its storage");
        const void* nodes=level.nodes.z?reinterpret_cast<const uint8_t*>(packed.nodes.data())+level.nodes.x:nullptr;
        const void* children=level.children.z?reinterpret_cast<const uint8_t*>(packed.children.data())+level.children.x:nullptr;
        source.levels.push_back({{nodes,level.nodes.z,level.nodes.y},{children,level.children.z,level.children.y},level.nodes.w});
    }
    source.atlasMap=packed.atlasMap.data();source.atlasMapCount=packed.atlasMap.size();return source;
}
}
bool loadRawVBX(const void* data,size_t bytes,VbxRawVolume& output,std::string& error) {
    try {
        require(data&&bytes>=6,"VBX input is empty or truncated");
        Reader r{static_cast<const uint8_t*>(data),bytes};
        const uint8_t major=r.byte(),minor=r.byte();
        require((major==1&&(minor==0||minor>=11))||major==2,"unsupported VBX version");
        VbxRawVolume result;
        if((major==1&&minor>=11)||major==2) {
            result.transform.preTranslation=r.vector();result.transform.angles=r.vector();
            result.transform.scale=r.vector();result.transform.translation=r.vector();
        }
        checkTransform(result.transform);
        const int32_t grids=r.integer();
        require(grids==1,"unsupported VBX grid count: exactly one grid is required");
        uint8_t masks=major==1&&minor==0?1:0;
        if(major==2)masks=r.byte();
        require(masks<=1,"VBX bitmask flag is invalid");
        const uint64_t gridOffset=r.ulong();
        if(gridOffset) {require(gridOffset>=r.offset&&gridOffset<=bytes,"VBX grid offset is out of bounds");r.offset=size_t(gridOffset);}
        std::memcpy(result.gridName.data(),r.take(256),256);
        const uint8_t dtype=r.byte(),components=r.byte(),compression=r.byte();
        require(dtype=='f'&&components==1,"unsupported VBX grid metadata type");
        require(compression==0,"unsupported compressed VBX atlas");
        result.voxelSize=r.vector();checkVoxelSize(result.voxelSize);
        const int32_t leafCount=r.integer();
        const auto leafDimension=r.intVector();
        const int32_t apron=r.integer(),channels=r.integer();
        const uint64_t atlasSize=r.ulong();
        const uint8_t topology=r.byte();const int32_t reuse=r.integer();const uint8_t layout=r.byte();
        require(topology==2&&reuse==0&&layout==0,"unsupported VBX topology reuse or atlas layout");
        require(channels>0&&uint64_t(channels)<=uint64_t(r.size-r.offset)/8,"VBX channel count is invalid");
        require(apron>=0&&leafCount>0,"VBX leaf count or apron is invalid");
        const auto grid=r.intVector(),resolution=r.intVector();
        require(grid.x>0&&grid.y>0&&grid.z>0&&resolution.x>0&&resolution.y>0&&resolution.z>0,"VBX atlas dimensions are invalid");
        require(uint64_t(resolution.x)*resolution.y<=UINT32_MAX,"VBX atlas dimensions overflow");
        const uint64_t voxelCount=uint64_t(resolution.x)*resolution.y*resolution.z;
        require(voxelCount<=UINT32_MAX,"VBX atlas voxel count exceeds the portable range");
        const int32_t levelCount=r.integer();
        const uint64_t root=normalizeReference(r.ulong());
        require(levelCount>0&&levelCount<=int32_t(kMaxLevels)&&root!=UINT64_MAX,"VBX hierarchy or root is invalid");
        std::vector<DiskLevel> disk(size_t(levelCount),DiskLevel{});
        uint64_t extent=1;
        for(auto& level:disk) {
            level.logDimension=r.integer();level.resolution=r.integer();level.extent=r.intVector();
            level.nodes=r.integer();level.nodeStride=r.integer();level.lists=r.integer();level.listStride=r.integer();
            require(level.logDimension>0&&level.logDimension<=10,"unsupported VBX branching dimension");
            require(level.resolution==(1<<level.logDimension),"VBX branching resolution is inconsistent");
            extent*=uint32_t(level.resolution);
            require(extent<=INT32_MAX&&level.extent.x==int32_t(extent)&&level.extent.y==int32_t(extent)&&level.extent.z==int32_t(extent),"VBX node extent is inconsistent");
            require(level.nodes>=0&&level.lists>=0&&level.nodeStride>=64&&level.nodeStride%8==0&&level.listStride>=0&&level.listStride%8==0,"VBX pool description is invalid");
            require(!level.lists||level.listStride>0,"VBX child pool has zero stride");
        }
        require(disk[0].nodes==leafCount&&disk[0].lists==0,"VBX leaf pool does not match grid metadata");
        require(leafDimension.x==disk[0].resolution&&leafDimension.y==leafDimension.x&&leafDimension.z==leafDimension.x,"VBX leaf dimensions are inconsistent");
        const uint64_t brickWidth=uint64_t(leafDimension.x)+2ull*uint32_t(apron);
        require(brickWidth<=UINT32_MAX&&uint64_t(grid.x)*brickWidth==uint32_t(resolution.x)&&uint64_t(grid.y)*brickWidth==uint32_t(resolution.y)&&uint64_t(grid.z)*brickWidth==uint32_t(resolution.z),"VBX atlas brick dimensions are inconsistent");
        for(auto& level:disk)level.nodeData=r.take(uint64_t(level.nodes)*uint32_t(level.nodeStride));
        for(auto& level:disk)level.listData=r.take(uint64_t(level.lists)*uint32_t(level.listStride));
        for(int32_t channel=0;channel<channels;++channel) {
            const int32_t type=r.integer(),stride=r.integer();
            AtlasFormat format;
            switch(type) {
            case 0:format=AtlasFormat::R8_UINT;break;
            case 2:format=AtlasFormat::RGBA8_UINT;break;
            case 3:format=AtlasFormat::R32_FLOAT;break;
            case 4:format=AtlasFormat::RGB32_FLOAT;break;
            case 5:format=AtlasFormat::RGBA32_FLOAT;break;
            default:throw std::runtime_error("unsupported VBX channel type");
            }
            require(stride==int32_t(atlasStride(format)),"VBX channel stride does not match its format");
            const uint64_t channelBytes=voxelCount*uint32_t(stride);
            require(channelBytes<=uint64_t(UINT32_MAX)+1,"VBX channel exceeds portable raw-buffer addressing");
            if(channel==0)require(channelBytes==atlasSize,"VBX atlas size is inconsistent with its first channel");
            const auto* channelData=r.take(channelBytes);
            result.channels.push_back({format,std::vector<uint8_t>(channelData,channelData+size_t(channelBytes))});
        }
        std::vector<std::vector<uint32_t>> nodes(static_cast<size_t>(levelCount)),children(static_cast<size_t>(levelCount));
        VolumeSource source;
        for(size_t level=0;level<disk.size();++level) {
            const auto& d=disk[level];
            nodes[level].resize(size_t(d.nodes)*16);
            const uint64_t childGrid=uint64_t(d.resolution)*d.resolution*d.resolution;
            require(!d.lists||uint64_t(d.lists)*childGrid*8<=uint64_t(UINT32_MAX)+1,"expanded VBX child pool exceeds 4 GiB");
            if(d.lists)children[level].resize(size_t(d.lists)*size_t(childGrid)*2,UINT32_MAX);
            for(int32_t index=0;index<d.nodes;++index) {
                auto* node=nodes[level].data()+size_t(index)*16;
                const auto* original=d.nodeData+size_t(index)*size_t(d.nodeStride);
                for(unsigned w=0;w<16;++w)node[w]=u32(original+4*w);
                require((node[0]&255u)==level,"VBX serialized node is in the wrong level");
                setReference(node+10,normalizeReference(getReference(node+10)));
                setReference(node+12,normalizeReference(getReference(node+12)));
                if(masks) {node[14]=node[15]=0;if(level==0)node[0]=(node[0]&~0xff00u)|0x100u;}
            }
            if(level>0&&d.lists) {
                if(masks) {
                    require(uint64_t(d.nodeStride-56)*8>=childGrid,"VBX node bitmask is shorter than its child grid");
                    std::vector<bool> seen(size_t(d.lists),false);
                    for(int32_t index=0;index<d.nodes;++index) {
                        const auto* node=nodes[level].data()+size_t(index)*16;
                        const uint64_t list=getReference(node+12);
                        if(list==UINT64_MAX)continue;
                        require((list&255u)==1&&((list>>8)&255u)==level&&(list>>16)<uint64_t(d.lists),"VBX bitmask node references an invalid child list");
                        const size_t listIndex=size_t(list>>16);
                        require(!seen[listIndex],"VBX bitmask child list is shared by multiple nodes");seen[listIndex]=true;
                        const auto* mask=d.nodeData+size_t(index)*size_t(d.nodeStride)+56;
                        const auto* packed=d.listData+listIndex*size_t(d.listStride);
                        uint64_t rank=0;
                        for(uint64_t bit=0;bit<childGrid;++bit)if((mask[bit/8]>>(bit%8))&1u) {
                            require((rank+1)*8<=uint32_t(d.listStride),"VBX packed child list is shorter than its bitmask");
                            setReference(children[level].data()+(listIndex*size_t(childGrid)+size_t(bit))*2,normalizeReference(u64(packed+rank*8)));
                            ++rank;
                        }
                    }
                } else {
                    require(uint64_t(d.listStride)>=childGrid*8,"VBX indexed child list is truncated");
                    for(int32_t index=0;index<d.lists;++index)for(uint64_t child=0;child<childGrid;++child)
                        setReference(children[level].data()+(size_t(index)*size_t(childGrid)+size_t(child))*2,normalizeReference(u64(d.listData+size_t(index)*size_t(d.listStride)+child*8)));
                }
            }
            source.levels.push_back({{nodes[level].data(),uint32_t(d.nodes),64},{children[level].data(),uint32_t(d.lists),d.lists?childGrid*8:0},uint32_t(d.logDimension)});
        }
        const uint64_t slots=uint64_t(grid.x)*grid.y*grid.z;
        require(slots<=UINT32_MAX,"VBX atlas slot count exceeds the portable range");
        std::vector<AtlasNode> atlasMap(size_t(slots),AtlasNode{-1,-1,-1,-1});
        int64_t minimum[3]={INT32_MAX,INT32_MAX,INT32_MAX},maximum[3]={INT32_MIN,INT32_MIN,INT32_MIN};
        bool active=false;
        for(int32_t index=0;index<disk[0].nodes;++index) {
            auto* node=nodes[0].data()+size_t(index)*16;
            if(((node[0]>>8)&255u)==0)continue;
            uint32_t brick[3];
            for(unsigned axis=0;axis<3;++axis) {
                const int64_t atlas=int32_t(node[4+axis]);
                require(atlas>=apron&&(atlas-apron)%int64_t(brickWidth)==0,"VBX leaf has an invalid atlas position");
                brick[axis]=uint32_t((atlas-apron)/int64_t(brickWidth));
                const int64_t position=int32_t(node[1+axis]);
                minimum[axis]=std::min(minimum[axis],position);maximum[axis]=std::max(maximum[axis],position+leafDimension.x);
                require(maximum[axis]<=INT32_MAX,"VBX active bounds exceed the portable range");
            }
            require(brick[0]<uint32_t(grid.x)&&brick[1]<uint32_t(grid.y)&&brick[2]<uint32_t(grid.z),"VBX leaf atlas slot is out of bounds");
            const size_t slot=(size_t(brick[2])*size_t(grid.y)+brick[1])*size_t(grid.x)+brick[0];
            require(atlasMap[slot].leaf<0,"VBX active leaves share an atlas slot");
            atlasMap[slot]={int32_t(node[1]),int32_t(node[2]),int32_t(node[3]),index};active=true;
        }
        require(active,"VBX has no active bricks");
        source.volume.atlasResolutionAndApron={uint32_t(resolution.x),uint32_t(resolution.y),uint32_t(resolution.z),uint32_t(apron)};
        source.volume.atlasBrickCountAndWidth={uint32_t(grid.x),uint32_t(grid.y),uint32_t(grid.z),uint32_t(brickWidth)};
        require((root&255u)==0&&(root>>16)<=UINT32_MAX,"VBX root reference is invalid");
        source.volume.hierarchy={uint32_t((root>>8)&255u),uint32_t(root>>16),uint32_t(levelCount),0};
        source.volume.boundsMin={float(minimum[0]),float(minimum[1]),float(minimum[2]),0};
        source.volume.boundsMax={float(maximum[0]),float(maximum[1]),float(maximum[2]),0};
        source.atlasMap=atlasMap.data();source.atlasMapCount=atlasMap.size();
        if(!packVolume(source,result.packed,error))throw std::runtime_error(error);
        output=std::move(result);error.clear();return true;
    } catch(const std::exception& exception) {error=exception.what();return false;}
}
bool saveRawVBX(const VbxRawVolume& volume,std::vector<uint8_t>& output,std::string& error) {
    try {
        checkTransform(volume.transform);checkVoxelSize(volume.voxelSize);
        const auto source=sourceFromPacked(volume.packed);
        PackedVolume validated;
        if(!packVolume(source,validated,error))throw std::runtime_error(error);
        const auto& info=validated.volume;const auto res=info.atlasResolutionAndApron,grid=info.atlasBrickCountAndWidth;
        const uint64_t voxelCount=uint64_t(res.x)*res.y*res.z;
        require(!volume.channels.empty()&&volume.channels.size()<=INT32_MAX,"VBX channel count is invalid");
        for(const auto& channel:volume.channels) {
            const uint32_t stride=atlasStride(channel.format);
            require(stride&&voxelCount*stride==channel.bytes.size(),"VBX channel data does not match its format and dimensions");
            require(channel.bytes.size()<=uint64_t(UINT32_MAX)+1,"VBX channel exceeds portable raw-buffer addressing");
        }
        require(validated.levels[0].nodes.z<=INT32_MAX&&res.x<=INT32_MAX&&res.y<=INT32_MAX&&res.z<=INT32_MAX,"VBX dimensions exceed signed file fields");
        Writer w;w.byte(1);w.byte(11);
        w.vector(volume.transform.preTranslation);w.vector(volume.transform.angles);w.vector(volume.transform.scale);w.vector(volume.transform.translation);
        w.integer(1);w.ulong(62); // fixed header: 2 + 48 transform + 4 grid count + 8 offset
        w.append(volume.gridName.data(),volume.gridName.size());w.byte('f');w.byte(1);w.byte(0);w.vector(volume.voxelSize);
        w.uint(validated.levels[0].nodes.z);const uint32_t leafWidth=1u<<validated.levels[0].nodes.w;w.intVector(leafWidth,leafWidth,leafWidth);
        w.uint(res.w);w.integer(int32_t(volume.channels.size()));w.ulong(volume.channels.front().bytes.size());w.byte(2);w.integer(0);w.byte(0);
        w.intVector(grid.x,grid.y,grid.z);w.intVector(res.x,res.y,res.z);w.integer(int32_t(validated.levels.size()));
        w.ulong((uint64_t(info.hierarchy.y)<<16)|(uint64_t(info.hierarchy.x)<<8));
        for(const auto& level:validated.levels) {
            require(level.nodes.y<=INT32_MAX&&level.nodes.z<=INT32_MAX&&level.children.y<=INT32_MAX&&level.children.z<=INT32_MAX,"VBX pool exceeds signed file fields");
            w.uint(level.nodes.w);w.uint(1u<<level.nodes.w);w.intVector(uint32_t(level.nodeExtent.x),uint32_t(level.nodeExtent.y),uint32_t(level.nodeExtent.z));
            w.uint(level.nodes.z);w.uint(level.nodes.y);w.uint(level.children.z);w.uint(level.children.y);
        }
        for(const auto& level:validated.levels)for(uint32_t index=0;index<level.nodes.z;++index) {
            const auto* node=validated.nodes.data()+(level.nodes.x+uint64_t(index)*level.nodes.y)/4;
            for(uint32_t word=0;word<level.nodes.y/4;++word) {
                uint32_t value=node[word];
                if((word==11||word==13)&&node[word-1]==UINT32_MAX&&node[word]==UINT32_MAX)value=0;
                w.uint(value);
            }
        }
        for(const auto& level:validated.levels)for(uint64_t word=0;word<uint64_t(level.children.y)*level.children.z/4;++word)
            w.uint(validated.children[size_t(level.children.x/4+word)]);
        for(const auto& channel:volume.channels) {
            const int32_t types[]={0,2,3,4,5};
            w.integer(types[uint32_t(channel.format)]);w.uint(atlasStride(channel.format));
            w.append(channel.bytes.data(),channel.bytes.size());
        }
        output=std::move(w.bytes);error.clear();return true;
    } catch(const std::exception& exception) {error=exception.what();return false;}
}
bool loadVBX(const void* data,size_t bytes,VbxVolume& output,std::string& error) {
    try {
    VbxRawVolume raw;if(!loadRawVBX(data,bytes,raw,error))return false;
    if(raw.channels.size()!=1||raw.channels[0].format!=AtlasFormat::R32_FLOAT) {
        error="scalar VBX API requires exactly one R32_FLOAT channel; use loadRawVBX for other channels";return false;
    }
    VbxVolume result;result.packed=std::move(raw.packed);result.transform=raw.transform;result.gridName=raw.gridName;result.voxelSize=raw.voxelSize;
    result.atlas.resize(raw.channels[0].bytes.size()/4);
    for(size_t i=0;i<result.atlas.size();++i){const uint32_t bits=u32(raw.channels[0].bytes.data()+i*4);std::memcpy(&result.atlas[i],&bits,4);}
    output=std::move(result);error.clear();return true;
    }catch(const std::exception& exception){error=exception.what();return false;}
}
bool saveVBX(const VbxVolume& volume,std::vector<uint8_t>& output,std::string& error) {
    try {
    VbxRawVolume raw;raw.packed=volume.packed;raw.transform=volume.transform;raw.gridName=volume.gridName;raw.voxelSize=volume.voxelSize;
    VbxChannel channel;channel.format=AtlasFormat::R32_FLOAT;channel.bytes.resize(volume.atlas.size()*4);
    for(size_t i=0;i<volume.atlas.size();++i){uint32_t bits;std::memcpy(&bits,&volume.atlas[i],4);for(unsigned b=0;b<4;++b)channel.bytes[i*4+b]=uint8_t(bits>>(b*8));}
    raw.channels.push_back(std::move(channel));return saveRawVBX(raw,output,error);
    }catch(const std::exception& exception){error=exception.what();return false;}
}
bool loadRawVBXFile(const std::filesystem::path& path,VbxRawVolume& output,std::string& error) {
    try {
        std::ifstream file(path,std::ios::binary|std::ios::ate);require(bool(file),"cannot open VBX file");
        const auto size=file.tellg();require(size>=0&&uint64_t(size)<=std::numeric_limits<size_t>::max(),"VBX file size is invalid");
        std::vector<uint8_t> bytes(static_cast<size_t>(size));file.seekg(0);file.read(reinterpret_cast<char*>(bytes.data()),std::streamsize(bytes.size()));require(bool(file),"cannot read complete VBX file");
        return loadRawVBX(bytes.data(),bytes.size(),output,error);
    }catch(const std::exception& exception){error=exception.what();return false;}
}
bool saveRawVBXFile(const std::filesystem::path& path,const VbxRawVolume& volume,std::string& error) {
    std::vector<uint8_t> bytes;if(!saveRawVBX(volume,bytes,error))return false;
    try {
        std::ofstream file(path,std::ios::binary|std::ios::trunc);require(bool(file),"cannot create VBX file");
        file.write(reinterpret_cast<const char*>(bytes.data()),std::streamsize(bytes.size()));file.close();require(bool(file),"cannot write complete VBX file");error.clear();return true;
    }catch(const std::exception& exception){error=exception.what();return false;}
}
bool loadVBXFile(const std::filesystem::path& path,VbxVolume& output,std::string& error) {
    try {
        std::ifstream file(path,std::ios::binary|std::ios::ate);require(bool(file),"cannot open VBX file");
        const auto size=file.tellg();require(size>=0&&uint64_t(size)<=std::numeric_limits<size_t>::max(),"VBX file size is invalid");
        std::vector<uint8_t> bytes(static_cast<size_t>(size));file.seekg(0);file.read(reinterpret_cast<char*>(bytes.data()),std::streamsize(bytes.size()));require(bool(file),"cannot read complete VBX file");
        return loadVBX(bytes.data(),bytes.size(),output,error);
    } catch(const std::exception& exception) {error=exception.what();return false;}
}
bool saveVBXFile(const std::filesystem::path& path,const VbxVolume& volume,std::string& error) {
    std::vector<uint8_t> bytes;if(!saveVBX(volume,bytes,error))return false;
    try {
        std::ofstream file(path,std::ios::binary|std::ios::trunc);require(bool(file),"cannot create VBX file");
        file.write(reinterpret_cast<const char*>(bytes.data()),std::streamsize(bytes.size()));file.close();require(bool(file),"cannot write complete VBX file");error.clear();return true;
    } catch(const std::exception& exception) {error=exception.what();return false;}
}
} // namespace gvdb::slang
