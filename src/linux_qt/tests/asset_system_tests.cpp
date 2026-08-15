#include "DetailObjects.hpp"
#include "GameFileSystem.hpp"
#include "MaterialSystem.hpp"
#include "VmfDocument.hpp"
#include "VmfScene.hpp"
#include "StudioModelSystem.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {
// The rest of this file checks with assert(), which this project's Release
// build (-DNDEBUG) compiles away. Detail-object coverage uses a check that
// survives the optimizer.
void detailCheckImpl(bool condition, const char* expression, int line)
{
    if (!condition) {
        std::cerr << "FAILED (line " << line << "): " << expression << '\n';
        std::exit(EXIT_FAILURE);
    }
}
#define detailCheck(condition) detailCheckImpl((condition), #condition, __LINE__)

template<typename T>
void appendLe(std::vector<std::uint8_t>& out, T value)
{
    for (std::size_t i=0;i<sizeof(T);++i) out.push_back(static_cast<std::uint8_t>((value>>(8*i))&0xff));
}

template<typename T>
void setLe(std::vector<std::uint8_t>& out, std::size_t offset, T value)
{
    for (std::size_t i=0;i<sizeof(T);++i) out[offset+i]=static_cast<std::uint8_t>((value>>(8*i))&0xff);
}

void appendString(std::vector<std::uint8_t>& out, const std::string& text)
{
    out.insert(out.end(),text.begin(),text.end()); out.push_back(0);
}

void write(const fs::path& path, const std::vector<std::uint8_t>& bytes)
{
    fs::create_directories(path.parent_path());
    std::ofstream stream(path,std::ios::binary); assert(stream);
    stream.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
}

void writeText(const fs::path& path, const std::string& text)
{
    fs::create_directories(path.parent_path());
    std::ofstream stream(path); assert(stream); stream<<text;
}

std::vector<std::uint8_t> makeVpkFiles(
    const std::vector<std::pair<std::string, std::vector<std::uint8_t>>>& files)
{
    struct FilePart { std::string name; std::vector<std::uint8_t> bytes; };
    std::map<std::string, std::map<std::string, std::vector<FilePart>>> treeFiles;
    for (const auto& [resource, bytes] : files) {
        const fs::path path(resource);
        std::string extension = path.extension().string();
        if (!extension.empty() && extension.front() == '.') extension.erase(extension.begin());
        std::string directory = path.parent_path().generic_string();
        if (directory.empty()) directory = " ";
        treeFiles[extension][directory].push_back({path.stem().string(), bytes});
    }
    std::vector<std::uint8_t> tree;
    for (const auto& [extension, paths] : treeFiles) {
        appendString(tree, extension);
        for (const auto& [directory, entries] : paths) {
            appendString(tree, directory);
            for (const auto& entry : entries) {
                appendString(tree, entry.name);
                appendLe<std::uint32_t>(tree,0);
                appendLe<std::uint16_t>(tree,static_cast<std::uint16_t>(entry.bytes.size()));
                appendLe<std::uint16_t>(tree,0x7fff);
                appendLe<std::uint32_t>(tree,0);
                appendLe<std::uint32_t>(tree,0);
                appendLe<std::uint16_t>(tree,0xffff);
                tree.insert(tree.end(),entry.bytes.begin(),entry.bytes.end());
            }
            tree.push_back(0);
        }
        tree.push_back(0);
    }
    tree.push_back(0);
    std::vector<std::uint8_t> file;
    appendLe<std::uint32_t>(file,0x55aa1234); appendLe<std::uint32_t>(file,1);
    appendLe<std::uint32_t>(file,static_cast<std::uint32_t>(tree.size()));
    file.insert(file.end(),tree.begin(),tree.end());
    return file;
}

std::vector<std::uint8_t> makeVpk()
{
    return makeVpkFiles({{"materials/hello.txt", {'h','e','l','l','o'}}});
}

std::vector<std::uint8_t> makeVtf()
{
    std::vector<std::uint8_t> bytes(80+16,0);
    bytes[0]='V';bytes[1]='T';bytes[2]='F';bytes[3]=0;
    setLe<std::uint32_t>(bytes,4,7); setLe<std::uint32_t>(bytes,8,2);
    setLe<std::uint32_t>(bytes,12,80); setLe<std::uint16_t>(bytes,16,2);
    setLe<std::uint16_t>(bytes,18,2); setLe<std::uint16_t>(bytes,24,1);
    setLe<std::uint32_t>(bytes,52,12); bytes[56]=1;
    setLe<std::uint32_t>(bytes,57,0xffffffffu); bytes[61]=0; bytes[62]=0;
    setLe<std::uint16_t>(bytes,63,1);
    const std::uint8_t pixels[16]={0,0,255,255, 0,255,0,255, 255,0,0,255, 255,255,255,255};
    std::copy(std::begin(pixels),std::end(pixels),bytes.begin()+80);
    return bytes;
}

std::vector<std::uint8_t> makeAnimatedVtf()
{
    // Two BGRA8888 frames at the top mip. Frame zero and frame one differ so
    // the material test covers AnimatedTexture frame decoding, not only timing.
    std::vector<std::uint8_t> bytes(80+32,0);
    bytes[0]='V';bytes[1]='T';bytes[2]='F';bytes[3]=0;
    setLe<std::uint32_t>(bytes,4,7); setLe<std::uint32_t>(bytes,8,2);
    setLe<std::uint32_t>(bytes,12,80); setLe<std::uint16_t>(bytes,16,2);
    setLe<std::uint16_t>(bytes,18,2); setLe<std::uint16_t>(bytes,24,2);
    setLe<std::uint32_t>(bytes,52,12); bytes[56]=1;
    setLe<std::uint32_t>(bytes,57,0xffffffffu); bytes[61]=0; bytes[62]=0;
    setLe<std::uint16_t>(bytes,63,1);
    const std::uint8_t pixels[32]={
        0,0,255,255, 0,255,0,255, 255,0,0,255, 255,255,255,255,
        255,255,0,255, 255,0,255,255, 0,255,255,255, 32,64,128,255};
    std::copy(std::begin(pixels),std::end(pixels),bytes.begin()+80);
    return bytes;
}

std::vector<std::uint8_t> makeCubemapVtf()
{
    constexpr std::array<std::array<std::uint8_t,4>,6> FaceBgra{{
        {{0,0,255,255}},       // right: red
        {{0,255,0,255}},       // left: green
        {{255,0,0,255}},       // back: blue
        {{0,255,255,255}},     // front: yellow
        {{255,0,255,255}},     // up: magenta
        {{255,255,0,255}}}};   // down: cyan
    std::vector<std::uint8_t> bytes(80 + FaceBgra.size() * 4, 0);
    bytes[0]='V'; bytes[1]='T'; bytes[2]='F'; bytes[3]=0;
    setLe<std::uint32_t>(bytes,4,7); setLe<std::uint32_t>(bytes,8,5);
    setLe<std::uint32_t>(bytes,12,80); setLe<std::uint16_t>(bytes,16,1);
    setLe<std::uint16_t>(bytes,18,1); setLe<std::uint32_t>(bytes,20,0x00004000u);
    setLe<std::uint16_t>(bytes,24,1); setLe<std::uint32_t>(bytes,52,12);
    bytes[56]=1; setLe<std::uint32_t>(bytes,57,0xffffffffu);
    bytes[61]=0; bytes[62]=0; setLe<std::uint16_t>(bytes,63,1);
    std::size_t offset=80;
    for(const auto& face:FaceBgra) {
        std::copy(face.begin(),face.end(),bytes.begin()+static_cast<std::ptrdiff_t>(offset));
        offset+=face.size();
    }
    return bytes;
}

std::vector<std::uint8_t> makeIa88Vtf()
{
    std::vector<std::uint8_t> bytes(80+8,0);
    bytes[0]='V';bytes[1]='T';bytes[2]='F';bytes[3]=0;
    setLe<std::uint32_t>(bytes,4,7); setLe<std::uint32_t>(bytes,8,2);
    setLe<std::uint32_t>(bytes,12,80); setLe<std::uint16_t>(bytes,16,2);
    setLe<std::uint16_t>(bytes,18,2); setLe<std::uint16_t>(bytes,24,1);
    setLe<std::uint32_t>(bytes,52,6); bytes[56]=1;
    setLe<std::uint32_t>(bytes,57,0xffffffffu); bytes[61]=0; bytes[62]=0;
    setLe<std::uint16_t>(bytes,63,1);
    const std::uint8_t pixels[8]={32,255, 64,192, 128,128, 255,64};
    std::copy(std::begin(pixels),std::end(pixels),bytes.begin()+80);
    return bytes;
}
void setFloat(std::vector<std::uint8_t>& bytes, std::size_t offset, float value)
{
    std::uint32_t bits=0; static_assert(sizeof(bits)==sizeof(value));
    std::memcpy(&bits,&value,sizeof(bits)); setLe<std::uint32_t>(bytes,offset,bits);
}

void setMatrix3x4(std::vector<std::uint8_t>& bytes, std::size_t offset,
                  const float values[3][4])
{
    for(int row=0;row<3;++row)
        for(int column=0;column<4;++column)
            setFloat(bytes,offset+static_cast<std::size_t>(row*4+column)*4u,
                     values[row][column]);
}

void setBone(std::vector<std::uint8_t>& bytes, std::size_t base, int parent,
             const float position[3], const float quaternion[4],
             const float poseToBone[3][4])
{
    setLe<std::uint32_t>(bytes,base+4,static_cast<std::uint32_t>(parent));
    for(int i=0;i<3;++i) setFloat(bytes,base+32+static_cast<std::size_t>(i)*4u,position[i]);
    for(int i=0;i<4;++i) setFloat(bytes,base+44+static_cast<std::size_t>(i)*4u,quaternion[i]);
    setMatrix3x4(bytes,base+96,poseToBone);
}

void setBoneName(std::vector<std::uint8_t>& bytes, std::size_t base,
                 const std::string& name)
{
    constexpr std::size_t NameOffset=196;
    assert(name.size()+1<=216-NameOffset);
    setLe<std::uint32_t>(bytes,base,NameOffset);
    std::copy(name.begin(),name.end(),bytes.begin()+base+NameOffset);
    bytes[base+NameOffset+name.size()]=0;
}

std::vector<std::uint8_t> makeTinyMdl()
{
    std::vector<std::uint8_t> bytes(1800, 0);
    bytes[0]='I'; bytes[1]='D'; bytes[2]='S'; bytes[3]='T';
    setLe<std::uint32_t>(bytes,4,48); setLe<std::uint32_t>(bytes,8,1234);
    setLe<std::uint32_t>(bytes,76,static_cast<std::uint32_t>(bytes.size()));

    // Two-level bind hierarchy. VVD positions are authored in model/pose
    // space, so boneToPose * poseToBone must be identity for both bones.
    setLe<std::uint32_t>(bytes,156,2); setLe<std::uint32_t>(bytes,160,640);
    constexpr float S=0.7071067811865475f;
    const float rootPosition[3]={4.0f,0.0f,0.0f};
    const float rootQuaternion[4]={0.0f,0.0f,S,S};
    const float rootPoseToBone[3][4]={
        {0.0f,1.0f,0.0f,0.0f},
        {-1.0f,0.0f,0.0f,4.0f},
        {0.0f,0.0f,1.0f,0.0f}};
    setBone(bytes,640,-1,rootPosition,rootQuaternion,rootPoseToBone);

    const float childPosition[3]={0.0f,8.0f,0.0f};
    const float childQuaternion[4]={S,0.0f,0.0f,S};
    const float childPoseToBone[3][4]={
        {0.0f,1.0f,0.0f,0.0f},
        {0.0f,0.0f,1.0f,0.0f},
        {1.0f,0.0f,0.0f,4.0f}};
    setBone(bytes,640+216,0,childPosition,childQuaternion,childPoseToBone);

    setLe<std::uint32_t>(bytes,204,1); setLe<std::uint32_t>(bytes,208,256);
    setLe<std::uint32_t>(bytes,232,1); setLe<std::uint32_t>(bytes,236,352);
    setLe<std::uint32_t>(bytes,256,64);
    const std::string texture="test/material";
    std::copy(texture.begin(),texture.end(),bytes.begin()+320); bytes[320+texture.size()]=0;
    setLe<std::uint32_t>(bytes,352+4,1); setLe<std::uint32_t>(bytes,352+12,16);
    const std::size_t model=368;
    setLe<std::uint32_t>(bytes,model+72,1); setLe<std::uint32_t>(bytes,model+76,148);
    setLe<std::uint32_t>(bytes,model+80,3); setLe<std::uint32_t>(bytes,model+84,0);
    const std::size_t mesh=model+148;
    setLe<std::uint32_t>(bytes,mesh,0); setLe<std::uint32_t>(bytes,mesh+8,3);
    setLe<std::uint32_t>(bytes,mesh+12,0);
    return bytes;
}

std::vector<std::uint8_t> makeTinySkinnedMdl()
{
    std::vector<std::uint8_t> bytes = makeTinyMdl();

    // Two texture records and two one-column skin families. Source indexes the
    // table as family * numskinref + mesh material slot.
    setLe<std::uint32_t>(bytes, 204, 2);
    setLe<std::uint32_t>(bytes, 208, 256);
    setLe<std::uint32_t>(bytes, 256, 128); // name at 384
    setLe<std::uint32_t>(bytes, 320, 96);  // name at 416
    const std::string first = "test/material";
    const std::string second = "test/material_skin1";
    std::copy(first.begin(), first.end(), bytes.begin() + 384);
    bytes[384 + first.size()] = 0;
    std::copy(second.begin(), second.end(), bytes.begin() + 416);
    bytes[416 + second.size()] = 0;

    setLe<std::uint32_t>(bytes, 220, 1); // numskinref
    setLe<std::uint32_t>(bytes, 224, 2); // numskinfamilies
    setLe<std::uint32_t>(bytes, 228, 448);
    setLe<std::uint16_t>(bytes, 448, 0);
    setLe<std::uint16_t>(bytes, 450, 1);

    // Move the bodypart/model/mesh records away from the second texture record.
    setLe<std::uint32_t>(bytes, 236, 1080);
    setLe<std::uint32_t>(bytes, 1080 + 4, 1);
    setLe<std::uint32_t>(bytes, 1080 + 12, 16);
    const std::size_t model = 1096;
    setLe<std::uint32_t>(bytes, model + 72, 1);
    setLe<std::uint32_t>(bytes, model + 76, 148);
    setLe<std::uint32_t>(bytes, model + 80, 3);
    setLe<std::uint32_t>(bytes, model + 84, 0);
    const std::size_t mesh = model + 148;
    setLe<std::uint32_t>(bytes, mesh, 0);
    setLe<std::uint32_t>(bytes, mesh + 8, 3);
    setLe<std::uint32_t>(bytes, mesh + 12, 0);
    return bytes;
}

void setQuaternion48(std::vector<std::uint8_t>& bytes, std::size_t offset,
                     const float quaternion[4])
{
    const auto clampInt=[](int value,int minimum,int maximum){
        return std::max(minimum,std::min(value,maximum));
    };
    const std::uint16_t x=static_cast<std::uint16_t>(clampInt(
        static_cast<int>(quaternion[0]*32768.0f)+32768,0,65535));
    const std::uint16_t y=static_cast<std::uint16_t>(clampInt(
        static_cast<int>(quaternion[1]*32768.0f)+32768,0,65535));
    std::uint16_t z=static_cast<std::uint16_t>(clampInt(
        static_cast<int>(quaternion[2]*16384.0f)+16384,0,32767));
    if(quaternion[3]<0.0f) z|=0x8000u;
    setLe<std::uint16_t>(bytes,offset,x);
    setLe<std::uint16_t>(bytes,offset+2,y);
    setLe<std::uint16_t>(bytes,offset+4,z);
}

std::vector<std::uint8_t> makeTinyReferencePoseMdl()
{
    std::vector<std::uint8_t> bytes=makeTinyMdl();

    // The stored base root is deliberately neutral. Hammer's StudioModel calls
    // InitPose and then evaluates sequence 0 at cycle zero; that animation is
    // what supplies the root transform used to generate poseToBone. A loader
    // that only reads mstudiobone_t::pos/quat leaves the root and every child
    // with a visible residual offset.
    const float neutralPosition[3]={0.0f,0.0f,0.0f};
    const float neutralQuaternion[4]={0.0f,0.0f,0.0f,1.0f};
    const float rootPoseToBone[3][4]={
        {0.0f,1.0f,0.0f,0.0f},
        {-1.0f,0.0f,0.0f,4.0f},
        {0.0f,0.0f,1.0f,0.0f}};
    setBone(bytes,640,-1,neutralPosition,neutralQuaternion,rootPoseToBone);

    constexpr std::size_t Sequence=1100;
    constexpr std::size_t AnimationDescription=1330;
    constexpr std::size_t AnimationData=1450;
    setLe<std::uint32_t>(bytes,180,1); // numlocalanim
    setLe<std::uint32_t>(bytes,184,AnimationDescription);
    setLe<std::uint32_t>(bytes,188,1); // numlocalseq
    setLe<std::uint32_t>(bytes,192,Sequence);

    setLe<std::uint32_t>(bytes,Sequence+56,1); // numblends
    setLe<std::uint32_t>(bytes,Sequence+60,212); // animindexindex
    setLe<std::uint32_t>(bytes,Sequence+68,1); // groupsize[0]
    setLe<std::uint32_t>(bytes,Sequence+72,1); // groupsize[1]
    setLe<std::uint32_t>(bytes,Sequence+156,214); // weightlistindex
    setLe<std::uint16_t>(bytes,Sequence+212,0); // first local animation
    setFloat(bytes,Sequence+214,1.0f);
    setFloat(bytes,Sequence+218,1.0f);

    setFloat(bytes,AnimationDescription+8,30.0f);
    setLe<std::uint32_t>(bytes,AnimationDescription+16,1); // numframes
    setLe<std::uint32_t>(bytes,AnimationDescription+52,0); // inline anim block
    setLe<std::uint32_t>(bytes,AnimationDescription+56,
                         static_cast<std::uint32_t>(AnimationData-AnimationDescription));

    constexpr float S=0.7071067811865475f;
    const float referenceQuaternion[4]={0.0f,0.0f,S,S};
    bytes[AnimationData]=0; // root bone
    bytes[AnimationData+1]=0x03; // STUDIO_ANIM_RAWPOS | STUDIO_ANIM_RAWROT
    setLe<std::uint16_t>(bytes,AnimationData+2,0); // final animation record
    setQuaternion48(bytes,AnimationData+4,referenceQuaternion);
    // Vector48 values for {4,0,0}: IEEE half 4.0 is 0x4400.
    setLe<std::uint16_t>(bytes,AnimationData+10,0x4400u);
    setLe<std::uint16_t>(bytes,AnimationData+12,0u);
    setLe<std::uint16_t>(bytes,AnimationData+14,0u);
    return bytes;
}

std::vector<std::uint8_t> makeTinyAnimatedMdl()
{
    std::vector<std::uint8_t> bytes=makeTinyMdl();
    constexpr std::size_t Sequence=1100;
    constexpr std::size_t AnimationDescription=1360;
    constexpr std::size_t AnimationData=1500;
    constexpr std::size_t SequenceLabel=1324;

    setLe<std::uint32_t>(bytes,180,1);
    setLe<std::uint32_t>(bytes,184,AnimationDescription);
    setLe<std::uint32_t>(bytes,188,1);
    setLe<std::uint32_t>(bytes,192,Sequence);

    setLe<std::uint32_t>(bytes,Sequence+4,SequenceLabel-Sequence);
    const std::string label="root_move";
    std::copy(label.begin(),label.end(),bytes.begin()+SequenceLabel);
    bytes[SequenceLabel+label.size()]=0;
    setLe<std::uint32_t>(bytes,Sequence+12,1); // STUDIO_LOOPING
    setLe<std::uint32_t>(bytes,Sequence+56,1);
    setLe<std::uint32_t>(bytes,Sequence+60,212);
    setLe<std::uint32_t>(bytes,Sequence+68,1);
    setLe<std::uint32_t>(bytes,Sequence+72,1);
    setLe<std::uint32_t>(bytes,Sequence+156,214);
    setLe<std::uint16_t>(bytes,Sequence+212,0);
    setFloat(bytes,Sequence+214,1.0f);
    setFloat(bytes,Sequence+218,1.0f);

    setFloat(bytes,AnimationDescription+8,10.0f);
    setLe<std::uint32_t>(bytes,AnimationDescription+16,2);
    setLe<std::uint32_t>(bytes,AnimationDescription+52,0);
    setLe<std::uint32_t>(bytes,AnimationDescription+56,
                         static_cast<std::uint32_t>(AnimationData-AnimationDescription));

    // Root X uses two compressed samples: bind pose at frame zero and +10 at frame one.
    setFloat(bytes,640+72,1.0f); // mstudiobone_t::posscale.x
    bytes[AnimationData]=0;
    bytes[AnimationData+1]=0x04; // STUDIO_ANIM_ANIMPOS
    setLe<std::uint16_t>(bytes,AnimationData+2,0);
    setLe<std::uint16_t>(bytes,AnimationData+4,6); // x stream follows value pointers
    setLe<std::uint16_t>(bytes,AnimationData+6,0);
    setLe<std::uint16_t>(bytes,AnimationData+8,0);
    bytes[AnimationData+10]=2; // valid
    bytes[AnimationData+11]=2; // total
    setLe<std::uint16_t>(bytes,AnimationData+12,0);
    setLe<std::uint16_t>(bytes,AnimationData+14,10);
    return bytes;
}

std::vector<std::uint8_t> makeTinyIncludedAnimationRootMdl()
{
    std::vector<std::uint8_t> bytes=makeTinyMdl();
    setBoneName(bytes,640,"root");
    setBoneName(bytes,640+216,"child");
    constexpr std::size_t IncludeTable=1100;
    constexpr std::size_t IncludeName=1120;
    const std::string name="editor/test_helper_included_animations.mdl";
    setLe<std::uint32_t>(bytes,336,1); // numincludemodels
    setLe<std::uint32_t>(bytes,340,IncludeTable);
    setLe<std::uint32_t>(bytes,IncludeTable+4,IncludeName-IncludeTable);
    std::copy(name.begin(),name.end(),bytes.begin()+IncludeName);
    bytes[IncludeName+name.size()]=0;
    return bytes;
}

std::vector<std::uint8_t> makeTinyIncludedAnimationMdl()
{
    std::vector<std::uint8_t> bytes=makeTinyAnimatedMdl();
    constexpr std::size_t RootBone=640;
    constexpr std::size_t ChildBone=640+216;
    constexpr std::size_t BoneSize=216;
    constexpr std::size_t Sequence=1100;
    constexpr std::size_t SequenceLabel=1324;
    constexpr std::size_t AnimationData=1500;
    setBoneName(bytes,RootBone,"root");
    setBoneName(bytes,ChildBone,"child");

    // Animation-only includes are allowed to use a different local bone order.
    // Swap root and child records, then target the remapped root at local index
    // one. Index-only mapping would animate the wrong render bone.
    std::array<std::uint8_t,BoneSize> first{};
    std::copy_n(bytes.begin()+RootBone,BoneSize,first.begin());
    std::copy_n(bytes.begin()+ChildBone,BoneSize,bytes.begin()+RootBone);
    std::copy(first.begin(),first.end(),bytes.begin()+ChildBone);
    bytes[AnimationData]=1;

    const std::string label="included_player_move";
    std::fill(bytes.begin()+SequenceLabel,bytes.begin()+SequenceLabel+40,0);
    std::copy(label.begin(),label.end(),bytes.begin()+SequenceLabel);
    setLe<std::uint32_t>(bytes,Sequence+4,SequenceLabel-Sequence);
    return bytes;
}

std::vector<std::uint8_t> makeTinyTf2VirtualRootMdl()
{
    std::vector<std::uint8_t> bytes=makeTinyMdl();
    setBoneName(bytes,640,"root");
    setBoneName(bytes,640+216,"child");
    constexpr std::size_t IncludeTable=1100;
    constexpr std::size_t SequenceGroupName=1140;
    constexpr std::size_t ChannelGroupName=1200;
    const std::string sequenceGroup="player/test_tf2_sequence_group.mdl";
    const std::string channelGroup="player/test_tf2_channel_group.mdl";
    setLe<std::uint32_t>(bytes,336,2);
    setLe<std::uint32_t>(bytes,340,IncludeTable);
    setLe<std::uint32_t>(bytes,IncludeTable+4,SequenceGroupName-IncludeTable);
    setLe<std::uint32_t>(bytes,IncludeTable+8+4,ChannelGroupName-(IncludeTable+8));
    std::copy(sequenceGroup.begin(),sequenceGroup.end(),bytes.begin()+SequenceGroupName);
    bytes[SequenceGroupName+sequenceGroup.size()]=0;
    std::copy(channelGroup.begin(),channelGroup.end(),bytes.begin()+ChannelGroupName);
    bytes[ChannelGroupName+channelGroup.size()]=0;
    return bytes;
}

std::vector<std::uint8_t> makeTinyTf2HumanRootMdl()
{
    std::vector<std::uint8_t> bytes=makeTinyMdl();
    bytes.resize(2300,0);
    setLe<std::uint32_t>(bytes,76,static_cast<std::uint32_t>(bytes.size()));
    setBoneName(bytes,640,"root");
    setBoneName(bytes,640+216,"child");

    // Human player render skeletons may contain extra class-specific bones
    // after the common animation skeleton. The included animation MDL below
    // deliberately omits bone names, matching the case where Source's virtual
    // masterBone table is required instead of equal-size index fallback.
    constexpr std::size_t ExtraBone=640+2*216;
    const float extraPosition[3]={0.0f,0.0f,0.0f};
    const float identityQuaternion[4]={0.0f,0.0f,0.0f,1.0f};
    const float identityPoseToBone[3][4]={
        {1.0f,0.0f,0.0f,0.0f},
        {0.0f,1.0f,0.0f,0.0f},
        {0.0f,0.0f,1.0f,0.0f}};
    setLe<std::uint32_t>(bytes,156,3);
    setBone(bytes,ExtraBone,1,extraPosition,identityQuaternion,identityPoseToBone);
    setBoneName(bytes,ExtraBone,"human_extra");

    constexpr std::size_t IncludeTable=1500;
    constexpr std::size_t SequenceGroupName=1540;
    constexpr std::size_t ChannelGroupName=1600;
    const std::string sequenceGroup="player/test_tf2_sequence_group.mdl";
    const std::string channelGroup="player/test_tf2_channel_group.mdl";
    setLe<std::uint32_t>(bytes,336,2);
    setLe<std::uint32_t>(bytes,340,IncludeTable);
    setLe<std::uint32_t>(bytes,IncludeTable+4,SequenceGroupName-IncludeTable);
    setLe<std::uint32_t>(bytes,IncludeTable+8+4,ChannelGroupName-(IncludeTable+8));
    std::copy(sequenceGroup.begin(),sequenceGroup.end(),bytes.begin()+SequenceGroupName);
    bytes[SequenceGroupName+sequenceGroup.size()]=0;
    std::copy(channelGroup.begin(),channelGroup.end(),bytes.begin()+ChannelGroupName);
    bytes[ChannelGroupName+channelGroup.size()]=0;

    // TF2 human class models use $declaresequence entries in the mesh MDL.
    // These compile as same-named, empty STUDIO_OVERRIDE descriptors and must
    // be replaced in-place by the included animation model's real sequence.
    constexpr std::size_t ForwardSequence=1750;
    constexpr std::size_t ForwardLabel=2000;
    const std::string forwardName="tf2_player_virtual_move";
    setLe<std::uint32_t>(bytes,188,1);
    setLe<std::uint32_t>(bytes,192,ForwardSequence);
    setLe<std::uint32_t>(bytes,ForwardSequence+4,ForwardLabel-ForwardSequence);
    setLe<std::uint32_t>(bytes,ForwardSequence+12,0x0800u); // STUDIO_OVERRIDE
    setLe<std::uint32_t>(bytes,ForwardSequence+68,1);
    setLe<std::uint32_t>(bytes,ForwardSequence+72,1);
    std::copy(forwardName.begin(),forwardName.end(),bytes.begin()+ForwardLabel);
    bytes[ForwardLabel+forwardName.size()]=0;
    return bytes;
}

std::vector<std::uint8_t> makeTinyTf2SequenceGroupMdl()
{
    std::vector<std::uint8_t> bytes=makeTinyAnimatedMdl();
    bytes.resize(1900,0);
    setLe<std::uint32_t>(bytes,76,static_cast<std::uint32_t>(bytes.size()));
    constexpr std::size_t Sequence=1100;
    constexpr std::size_t SequenceLabel=1324;
    constexpr std::size_t AnimationDescription=1360;
    constexpr std::size_t AnimationName=1490;
    const std::string sequenceName="tf2_player_virtual_move";
    const std::string animationName="shared_tf2_player_move";
    std::fill(bytes.begin()+SequenceLabel,bytes.begin()+SequenceLabel+64,0);
    std::copy(sequenceName.begin(),sequenceName.end(),bytes.begin()+SequenceLabel);
    setLe<std::uint32_t>(bytes,Sequence+4,SequenceLabel-Sequence);
    setLe<std::uint32_t>(bytes,AnimationDescription+4,AnimationName-AnimationDescription);
    std::copy(animationName.begin(),animationName.end(),bytes.begin()+AnimationName);
    bytes[AnimationName+animationName.size()]=0;
    // The sequence-owning group has only the local alias descriptor. Its
    // actual compressed channels are supplied by another virtual-model group.
    setLe<std::uint32_t>(bytes,AnimationDescription+52,0);
    setLe<std::uint32_t>(bytes,AnimationDescription+56,0);
    return bytes;
}

std::vector<std::uint8_t> makeTinyTf2ChannelGroupMdl()
{
    std::vector<std::uint8_t> bytes=makeTinyAnimatedMdl();
    bytes.resize(1900,0);
    setLe<std::uint32_t>(bytes,76,static_cast<std::uint32_t>(bytes.size()));
    constexpr std::size_t AnimationDescription=1360;
    constexpr std::size_t AnimationBlockTable=1560;
    constexpr std::size_t AnimationBlockName=1600;
    constexpr std::size_t AnimationName=1660;
    const std::string blockName="test_tf2_channel_group.ani"; // TF2-style basename
    const std::string animationName="shared_tf2_player_move";

    setLe<std::uint32_t>(bytes,188,0); // animation group contributes no sequences
    setLe<std::uint32_t>(bytes,192,0);
    setLe<std::uint32_t>(bytes,AnimationDescription+4,AnimationName-AnimationDescription);
    std::copy(animationName.begin(),animationName.end(),bytes.begin()+AnimationName);
    bytes[AnimationName+animationName.size()]=0;
    setLe<std::uint32_t>(bytes,348,AnimationBlockName);
    setLe<std::uint32_t>(bytes,352,2);
    setLe<std::uint32_t>(bytes,356,AnimationBlockTable);
    setLe<std::uint32_t>(bytes,AnimationBlockTable+8,32);
    setLe<std::uint32_t>(bytes,AnimationBlockTable+12,64);
    std::copy(blockName.begin(),blockName.end(),bytes.begin()+AnimationBlockName);
    bytes[AnimationBlockName+blockName.size()]=0;
    setLe<std::uint32_t>(bytes,AnimationDescription+52,1);
    setLe<std::uint32_t>(bytes,AnimationDescription+56,0); // valid external offset zero
    return bytes;
}

std::vector<std::uint8_t> makeTinyTf2ChannelAni()
{
    std::vector<std::uint8_t> bytes(64,0);
    constexpr std::size_t AnimationData=32; // datastart 32 + external animindex 0
    bytes[AnimationData]=0;
    bytes[AnimationData+1]=0x04;
    setLe<std::uint16_t>(bytes,AnimationData+2,0);
    setLe<std::uint16_t>(bytes,AnimationData+4,6);
    setLe<std::uint16_t>(bytes,AnimationData+6,0);
    setLe<std::uint16_t>(bytes,AnimationData+8,0);
    bytes[AnimationData+10]=2;
    bytes[AnimationData+11]=2;
    setLe<std::uint16_t>(bytes,AnimationData+12,0);
    setLe<std::uint16_t>(bytes,AnimationData+14,10);
    return bytes;
}

std::vector<std::uint8_t> makeTinyNeutralBlendMdl()
{
    std::vector<std::uint8_t> bytes=makeTinyMdl();
    bytes.resize(1900,0);
    setLe<std::uint32_t>(bytes,76,static_cast<std::uint32_t>(bytes.size()));
    constexpr std::size_t Sequence=1100;
    constexpr std::size_t BlendTable=1312;
    constexpr std::size_t WeightList=1340;
    constexpr std::size_t SequenceLabel=1360;
    constexpr std::size_t Animation0=1400;
    constexpr std::size_t Animation1=1500;
    constexpr std::size_t AnimationData=1650;

    setLe<std::uint32_t>(bytes,180,2);
    setLe<std::uint32_t>(bytes,184,Animation0);
    setLe<std::uint32_t>(bytes,188,1);
    setLe<std::uint32_t>(bytes,192,Sequence);
    setLe<std::uint32_t>(bytes,Sequence+4,SequenceLabel-Sequence);
    const std::string label="neutral_blend";
    std::copy(label.begin(),label.end(),bytes.begin()+SequenceLabel);
    bytes[SequenceLabel+label.size()]=0;
    setLe<std::uint32_t>(bytes,Sequence+12,1);
    setLe<std::uint32_t>(bytes,Sequence+56,3);
    setLe<std::uint32_t>(bytes,Sequence+60,BlendTable-Sequence);
    setLe<std::uint32_t>(bytes,Sequence+68,3);
    setLe<std::uint32_t>(bytes,Sequence+72,1);
    setFloat(bytes,Sequence+84,-1.0f);
    setFloat(bytes,Sequence+92,1.0f);
    setLe<std::uint32_t>(bytes,Sequence+156,WeightList-Sequence);
    setLe<std::uint16_t>(bytes,BlendTable,0);
    setLe<std::uint16_t>(bytes,BlendTable+2,1);
    setLe<std::uint16_t>(bytes,BlendTable+4,0);
    setFloat(bytes,WeightList,1.0f);
    setFloat(bytes,WeightList+4,1.0f);

    // Corner animation: one-frame reference pose and no channel records.
    setFloat(bytes,Animation0+8,30.0f);
    setLe<std::uint32_t>(bytes,Animation0+16,1);

    // Center animation: the same two-frame root translation as the regular fixture.
    setFloat(bytes,Animation1+8,10.0f);
    setLe<std::uint32_t>(bytes,Animation1+16,2);
    setLe<std::uint32_t>(bytes,Animation1+52,0);
    setLe<std::uint32_t>(bytes,Animation1+56,AnimationData-Animation1);
    setFloat(bytes,640+72,1.0f);
    bytes[AnimationData]=0;
    bytes[AnimationData+1]=0x04;
    setLe<std::uint16_t>(bytes,AnimationData+2,0);
    setLe<std::uint16_t>(bytes,AnimationData+4,6);
    setLe<std::uint16_t>(bytes,AnimationData+6,0);
    setLe<std::uint16_t>(bytes,AnimationData+8,0);
    bytes[AnimationData+10]=2;
    bytes[AnimationData+11]=2;
    setLe<std::uint16_t>(bytes,AnimationData+12,0);
    setLe<std::uint16_t>(bytes,AnimationData+14,10);
    return bytes;
}

std::vector<std::uint8_t> makeTinyRotatingAnimatedMdl()
{
    std::vector<std::uint8_t> bytes=makeTinyAnimatedMdl();
    constexpr std::size_t RootBone=640;
    constexpr std::size_t AnimationData=1500;
    constexpr float HalfPi=1.5707963267948966f;

    // Keep the Euler reference channel consistent with the authored reference
    // quaternion, then animate Z from 90 to 180 degrees while root X moves 0..10.
    setFloat(bytes,RootBone+68,HalfPi); // mstudiobone_t::rot.z
    setFloat(bytes,RootBone+92,HalfPi); // mstudiobone_t::rotscale.z
    bytes[AnimationData]=0;
    bytes[AnimationData+1]=0x0c; // STUDIO_ANIM_ANIMPOS | STUDIO_ANIM_ANIMROT
    setLe<std::uint16_t>(bytes,AnimationData+2,0);

    // Rotation value pointers begin at payload. The Z stream follows both
    // pointer triplets, at payload + 12 bytes.
    setLe<std::uint16_t>(bytes,AnimationData+4,0);
    setLe<std::uint16_t>(bytes,AnimationData+6,0);
    setLe<std::uint16_t>(bytes,AnimationData+8,12);
    // Position value pointers begin at payload + 6. X follows the rotation
    // stream and is therefore 12 bytes from this second pointer triplet.
    setLe<std::uint16_t>(bytes,AnimationData+10,12);
    setLe<std::uint16_t>(bytes,AnimationData+12,0);
    setLe<std::uint16_t>(bytes,AnimationData+14,0);

    bytes[AnimationData+16]=2;
    bytes[AnimationData+17]=2;
    setLe<std::uint16_t>(bytes,AnimationData+18,0);
    setLe<std::uint16_t>(bytes,AnimationData+20,1);
    bytes[AnimationData+22]=2;
    bytes[AnimationData+23]=2;
    setLe<std::uint16_t>(bytes,AnimationData+24,0);
    setLe<std::uint16_t>(bytes,AnimationData+26,10);
    return bytes;
}

std::vector<std::uint8_t> makeTinyExternalAnimatedMdl()
{
    std::vector<std::uint8_t> bytes=makeTinyAnimatedMdl();
    constexpr std::size_t AnimationDescription=1360;
    constexpr std::size_t AnimationBlockTable=1560;
    constexpr std::size_t AnimationBlockName=1600;
    const std::string name="models/editor/test_helper_external.ani";

    setLe<std::uint32_t>(bytes,348,AnimationBlockName);
    setLe<std::uint32_t>(bytes,352,2); // block zero plus one external block
    setLe<std::uint32_t>(bytes,356,AnimationBlockTable);
    setLe<std::uint32_t>(bytes,AnimationBlockTable+8,32); // block 1 data start
    setLe<std::uint32_t>(bytes,AnimationBlockTable+12,64);
    std::copy(name.begin(),name.end(),bytes.begin()+AnimationBlockName);
    bytes[AnimationBlockName+name.size()]=0;

    setLe<std::uint32_t>(bytes,AnimationDescription+52,1);
    setLe<std::uint32_t>(bytes,AnimationDescription+56,4);
    return bytes;
}

std::vector<std::uint8_t> makeTinyExternalAni()
{
    std::vector<std::uint8_t> bytes(64,0);
    constexpr std::size_t AnimationData=36; // block datastart 32 + animindex 4
    bytes[AnimationData]=0;
    bytes[AnimationData+1]=0x04; // STUDIO_ANIM_ANIMPOS
    setLe<std::uint16_t>(bytes,AnimationData+2,0);
    setLe<std::uint16_t>(bytes,AnimationData+4,6);
    setLe<std::uint16_t>(bytes,AnimationData+6,0);
    setLe<std::uint16_t>(bytes,AnimationData+8,0);
    bytes[AnimationData+10]=2;
    bytes[AnimationData+11]=2;
    setLe<std::uint16_t>(bytes,AnimationData+12,0);
    setLe<std::uint16_t>(bytes,AnimationData+14,10);
    return bytes;
}

std::vector<std::uint8_t> makeTinyLinearBoneMdl()
{
    std::vector<std::uint8_t> bytes=makeTinyMdl();
    bytes.resize(2600,0);
    setLe<std::uint32_t>(bytes,76,static_cast<std::uint32_t>(bytes.size()));

    // Make the legacy bone records deliberately wrong. Source uses the
    // studiohdr2 linear-bone arrays when present, so a loader that ignores them
    // will visibly move the child-weighted vertices.
    const float badPosition[3]={100.0f,50.0f,-25.0f};
    const float identityQuaternion[4]={0.0f,0.0f,0.0f,1.0f};
    const float identity[3][4]={
        {1.0f,0.0f,0.0f,0.0f},
        {0.0f,1.0f,0.0f,0.0f},
        {0.0f,0.0f,1.0f,0.0f}};
    setBone(bytes,640,-1,badPosition,identityQuaternion,identity);
    setBone(bytes,640+216,0,badPosition,identityQuaternion,identity);

    const std::size_t header2=1200;
    const std::size_t linear=1456;
    setLe<std::uint32_t>(bytes,400,static_cast<std::uint32_t>(header2));
    setLe<std::uint32_t>(bytes,header2+16,static_cast<std::uint32_t>(linear-header2));
    setLe<std::uint32_t>(bytes,linear,2);
    constexpr std::uint32_t ParentOffset=64;
    constexpr std::uint32_t PositionOffset=72;
    constexpr std::uint32_t QuaternionOffset=96;
    constexpr std::uint32_t RotationOffset=128;
    constexpr std::uint32_t PoseToBoneOffset=152;
    setLe<std::uint32_t>(bytes,linear+8,ParentOffset);
    setLe<std::uint32_t>(bytes,linear+12,PositionOffset);
    setLe<std::uint32_t>(bytes,linear+16,QuaternionOffset);
    setLe<std::uint32_t>(bytes,linear+20,RotationOffset);
    setLe<std::uint32_t>(bytes,linear+24,PoseToBoneOffset);

    setLe<std::uint32_t>(bytes,linear+ParentOffset,0xffffffffu);
    setLe<std::uint32_t>(bytes,linear+ParentOffset+4,0);
    constexpr float S=0.7071067811865475f;
    const float positions[2][3]={{4.0f,0.0f,0.0f},{0.0f,8.0f,0.0f}};
    const float quaternions[2][4]={{0.0f,0.0f,S,S},{S,0.0f,0.0f,S}};
    const float poseToBone[2][3][4]={
        {{0.0f,1.0f,0.0f,0.0f},{-1.0f,0.0f,0.0f,4.0f},{0.0f,0.0f,1.0f,0.0f}},
        {{0.0f,1.0f,0.0f,0.0f},{0.0f,0.0f,1.0f,0.0f},{1.0f,0.0f,0.0f,4.0f}}};
    for(int bone=0;bone<2;++bone){
        for(int component=0;component<3;++component)
            setFloat(bytes,linear+PositionOffset+static_cast<std::size_t>(bone*3+component)*4u,
                     positions[bone][component]);
        for(int component=0;component<4;++component)
            setFloat(bytes,linear+QuaternionOffset+static_cast<std::size_t>(bone*4+component)*4u,
                     quaternions[bone][component]);
        setMatrix3x4(bytes,linear+PoseToBoneOffset+static_cast<std::size_t>(bone)*48u,
                     poseToBone[bone]);
    }
    return bytes;
}

std::vector<std::uint8_t> makeTinyVvd()
{
    std::vector<std::uint8_t> bytes(64+3*48+3*16,0);
    bytes[0]='I'; bytes[1]='D'; bytes[2]='S'; bytes[3]='V';
    setLe<std::uint32_t>(bytes,4,4); setLe<std::uint32_t>(bytes,8,1234);
    setLe<std::uint32_t>(bytes,12,1); setLe<std::uint32_t>(bytes,16,3);
    // vertexFileHeader_t::vertexDataStart is at byte 56; tangentDataStart is 60.
    setLe<std::uint32_t>(bytes,56,64);
    setLe<std::uint32_t>(bytes,60,64+3*48);
    const float positions[3][3]={{0,0,0},{16,0,0},{0,16,0}};
    const float uvs[3][2]={{0,0},{1,0},{0,1}};
    for(int i=0;i<3;++i){
        const std::size_t base=64+static_cast<std::size_t>(i)*48;
        if(i==0){
            setFloat(bytes,base,1.0f); bytes[base+12]=0; bytes[base+15]=1;
        } else if(i==1){
            setFloat(bytes,base,1.0f); bytes[base+12]=1; bytes[base+15]=1;
        } else {
            setFloat(bytes,base,0.25f); setFloat(bytes,base+4,0.75f);
            bytes[base+12]=0; bytes[base+13]=1; bytes[base+15]=2;
        }
        setFloat(bytes,base+16,positions[i][0]); setFloat(bytes,base+20,positions[i][1]);
        setFloat(bytes,base+24,positions[i][2]); setFloat(bytes,base+28,0);
        setFloat(bytes,base+32,0); setFloat(bytes,base+36,1);
        setFloat(bytes,base+40,uvs[i][0]); setFloat(bytes,base+44,uvs[i][1]);
        const std::size_t tangentBase=64+3*48+static_cast<std::size_t>(i)*16;
        setFloat(bytes,tangentBase,1.0f);
        setFloat(bytes,tangentBase+4,0.0f);
        setFloat(bytes,tangentBase+8,0.0f);
        setFloat(bytes,tangentBase+12,1.0f);
    }
    return bytes;
}

std::vector<std::uint8_t> makeTinyVtx()
{
    std::vector<std::uint8_t> bytes(158,0);
    setLe<std::uint32_t>(bytes,0,7); setLe<std::uint32_t>(bytes,16,1234);
    setLe<std::uint32_t>(bytes,24,1); setLe<std::uint32_t>(bytes,28,1);
    setLe<std::uint32_t>(bytes,32,36);
    const std::size_t body=36; setLe<std::uint32_t>(bytes,body,1); setLe<std::uint32_t>(bytes,body+4,8);
    const std::size_t model=44; setLe<std::uint32_t>(bytes,model,1); setLe<std::uint32_t>(bytes,model+4,8);
    const std::size_t lod=52; setLe<std::uint32_t>(bytes,lod,1); setLe<std::uint32_t>(bytes,lod+4,12);
    const std::size_t mesh=64; setLe<std::uint32_t>(bytes,mesh,1); setLe<std::uint32_t>(bytes,mesh+4,9);
    const std::size_t group=73;
    setLe<std::uint32_t>(bytes,group,3); setLe<std::uint32_t>(bytes,group+4,25);
    setLe<std::uint32_t>(bytes,group+8,3); setLe<std::uint32_t>(bytes,group+12,52);
    setLe<std::uint32_t>(bytes,group+16,1); setLe<std::uint32_t>(bytes,group+20,58);
    for(int i=0;i<3;++i) setLe<std::uint16_t>(bytes,group+25+static_cast<std::size_t>(i)*9+4,static_cast<std::uint16_t>(i));
    for(int i=0;i<3;++i) setLe<std::uint16_t>(bytes,group+52+static_cast<std::size_t>(i)*2,static_cast<std::uint16_t>(i));
    const std::size_t strip=group+58; setLe<std::uint32_t>(bytes,strip,3);
    setLe<std::uint32_t>(bytes,strip+8,3); bytes[strip+18]=1;
    return bytes;
}

 }

int main()
{
    const fs::path temp=fs::temp_directory_path()/("hammer-assets-test-"+std::to_string(
        std::chrono::high_resolution_clock::now().time_since_epoch().count()));
    fs::create_directories(temp);

    const fs::path vpkPath=temp/"pak01_dir.vpk";
    write(vpkPath,makeVpk());
    hammer::assets::VpkArchive vpk;
    hammer::assets::AssetError error;
    assert(vpk.open(vpkPath,&error));
    const auto hello=vpk.read("materials/hello.txt");
    assert(hello && std::string(hello->begin(),hello->end())=="hello");

    const fs::path steam=temp/"Steam";
    const fs::path app=steam/"steamapps/common/Test Game";
    writeText(steam/"steamapps/libraryfolders.vdf","\"libraryfolders\" { \"0\" { \"path\" \""+steam.string()+"\" \"apps\" { \"123\" \"1\" } } }");
    writeText(steam/"steamapps/appmanifest_123.acf","\"AppState\" { \"appid\" \"123\" \"installdir\" \"Test Game\" }");
    writeText(app/"mod/gameinfo.txt",
        "GameInfo\n"
        "{\n"
        "  FileSystem\n"
        "  {\n"
        "    SteamAppId 123\n"
        "    SearchPaths\n"
        "    {\n"
        "      // Wildcard paths are normal Source syntax.\n"
        "      game+mod+custom_mod+vgui |gameinfo_path|custom/*\n"
        "      Game |gameinfo_path|.\n"
        "      Game |appid_123|base\n"
        "    }\n"
        "  }\n"
        "}\n");
    writeText(app/"mod/custom/example/materials/custom_marker.txt","mounted");
    writeText(app/"base/materials/test/material.vmt","\"LightmappedGeneric\" { \"$basetexture\" \"test/brick\" }");
    writeText(app/"base/materials/test/alpha_blend.vmt",
              "LightmappedGeneric { $basetexture test/brick $translucent 1 $alpha 0.35 }");
    writeText(app/"base/materials/test/alpha_cutout.vmt",
              "LightmappedGeneric { $basetexture test/brick $alphatest 1 $alphatestreference 0.3 }");
    write(app/"base/materials/test/brick.vtf",makeVtf());
    auto phongMaskVtf=makeVtf();
    phongMaskVtf[83]=64; // first BGRA texel alpha: normal-map Phong mask
    write(app/"base/materials/test/phong_mask.vtf",phongMaskVtf);
    write(app/"base/materials/test/phong_exponent.vtf",makeVtf());
    write(app/"base/materials/test/selfillum_mask.vtf",makeVtf());
    writeText(app/"base/materials/test/material_effects.vmt",
              "VertexLitGeneric { $basetexture test/brick $bumpmap test/phong_mask "
              "$phong 1 $phongboost 2 "
              "$phongfresnelranges \"[ 0.1 0.45 1.0 ]\" "
              "$phongtint \"[ 0 0 0 ]\" $phongalbedotint 1 "
              "$phongexponenttexture test/phong_exponent "
              "$selfillum 1 $selfillumtint \"[ 0.4 0.8 1.2 ]\" "
              "$selfillummask test/selfillum_mask "
              "$rimlight 1 $rimlightexponent 6 $rimlightboost 2.5 $rimmask 1 "
              "$envmap env_cubemap $normalmapalphaenvmapmask 1 $invertphongmask 1 $envmaptint \"[ 0.5 0.4 0.3 ]\" }");
    write(app/"base/materials/cubemaps/custom_env.vtf",makeCubemapVtf());
    writeText(app/"base/materials/test/custom_envmap.vmt",
              "VertexLitGeneric { $basetexture test/brick "
              "$envmap cubemaps/custom_env $envmaptint \"[ 0.8 0.9 1.0 ]\" }");
    writeText(app/"base/materials/test/missing_envmap.vmt",
              "VertexLitGeneric { $basetexture test/brick "
              "$envmap cubemaps/does_not_exist }");
    // The mounted/project path may contain the word "uber". That must never
    // classify an unrelated logical VMT as the TF2 invulnerability override.
    writeText(app/"base/materials/test/uber_directory/ordinary_material.vmt",
              R"VMT("VertexLitGeneric"
{
    "$basetexture" "test/red"
}
)VMT");
    writeText(app/"base/materials/models/effects/invulnfx_red.vmt",
              "VertexLitGeneric { $basetexture test/brick $phong 1 $selfillum 1 "
              "$color2 \"[ 0.25 0.25 0.25 ]\" $blendtintbybasealpha 1 "
              "$blendtintcoloroverbase 0.85 Proxies { "
              "ModelGlowColor { resultVar $glowcolor } "
              "Equals { srcVar1 $glowcolor resultVar $color2 } "
              "Equals { srcVar1 $glowcolor resultVar $selfillumtint } } }");
    writeText(app/"base/materials/models/bots/engineer/robot_invulnerability.vmt",
              "VertexLitGeneric { $basetexture test/brick $phong 1 $selfillum 1 "
              "$color2 \"[ 1 1 1 ]\" $selfillumtint \"[ 1 1 1 ]\" "
              "$blendtintbybasealpha 1 $blendtintcoloroverbase 1 Proxies { "
              "ModelGlowColor { resultVar $glowcolor } "
              "Equals { srcVar1 $glowcolor resultVar $color2 } "
              "Equals { srcVar1 $glowcolor resultVar $selfillumtint } } }");
    write(app/"base/materials/effects/red.vtf",makeVtf());
    write(app/"base/materials/effects/invun_red.vtf",makeCubemapVtf());
    write(app/"base/materials/water/tfwater001_normal.vtf",makeAnimatedVtf());
    write(app/"base/materials/models/player/pyro/pyro_lightwarp.vtf",makeVtf());
    // Stock Heavy player material: YellowLevel is a runtime entity proxy. Its
    // authored $yellow value is only an initializer and must not be copied into
    // $color2 when the editor cannot evaluate YellowLevel.
    writeText(app/"base/materials/models/player/hvyweapon/hvyweapon_red.vmt", R"VMT(
"VertexLitGeneric"
{
    "$basetexture" "test/brick"
    "$bumpmap" "test/phong_mask"
    "$detail" "test/brick"
    "$detailscale" "5"
    "$detailblendfactor" ".01"
    "$detailblendmode" "6"
    "$yellow" "0"
    "$one" "1"
    "$phong" "1"
    "$phongexponent" "20"
    "$phongboost" ".3"
    "$lightwarptexture" "models/player/pyro/pyro_lightwarp"
    "$phongfresnelranges" "[.3 1 8]"
    "$halflambert" "0"
    "$rimlight" "1"
    "$rimlightexponent" "4"
    "$rimlightboost" "2"
    "360?$color2" "[ 0.8 0.8 0.8 ]"
    "$cloakPassEnabled" "1"
    "Proxies"
    {
        "spy_invis" { }
        "invis" { }
        "AnimatedTexture"
        {
            "animatedtexturevar" "$detail"
            "animatedtextureframenumvar" "$detailframe"
            "animatedtextureframerate" "30"
        }
        "BurnLevel" { "resultVar" "$detailblendfactor" }
        "YellowLevel" { "resultVar" "$yellow" }
        "Equals" { "srcVar1" "$yellow" "resultVar" "$color2" }
    }
}
)VMT");
    writeText(app/"base/materials/models/effects/invulnfx_stock_red.vmt", R"VMT(
"VertexLitGeneric"
{
    "$basetexture" "effects/red"
    "$bumpmap" "water/tfwater001_normal"
    "$envmap" "effects/invun_red"
    "$halflambert" "1"
    "$selfillum" "1"
    "$selfIllumFresnel" "1"
    "$selfIllumFresnelMinMaxExp" "[0 18 13]"
    "$invulnlevel" "0"
    "$invulnexponent" "1"
    "$invulnfmax" "18"
    "$invulnscale" "0"
    "$invulnphong" "1"
    "$half" "0.5"
    "$invulnphongfading" "0"
    "$invulnphongfull" "1"
    "$invulnphongoutput" "1"
    "$invulnexponentfading" "1"
    "$invulnexponentfull" "13"
    "$invulnexponentoutput" "1"
    "$invulnfmaxfading" "-31"
    "$invulnfmaxfull" "18"
    "$invulnfmaxoutput" "1"
    "$phong" "1"
    "$phongexponent" "35"
    "$phongboost" "1"
    "$lightwarptexture" "models/player/pyro/pyro_lightwarp"
    "$phongfresnelranges" "[11 1 8]"
    "$rimlight" "1"
    "$rimlightexponent" "11"
    "$rimlightboost" "5"
    "$glowcolor" "1"
    "Proxies"
    {
        "ModelGlowColor" { "resultVar" "$glowcolor" }
        "Equals" { "srcVar1" "$glowcolor" "resultVar" "$color2" }
        "AnimatedTexture"
        {
            "animatedtexturevar" "$bumpmap"
            "animatedtextureframenumvar" "$bumpframe"
            "animatedtextureframerate" "70"
        }
        "InvulnLevel" { "resultVar" "$invulnlevel" }
        "LessOrEqual"
        {
            "srcVar1" "$invulnlevel" "srcVar2" "$half"
            "lessEqualVar" "$invulnphongfading"
            "greaterVar" "$invulnphongfull"
            "resultVar" "$invulnphongoutput"
        }
        "LessOrEqual"
        {
            "srcVar1" "$invulnlevel" "srcVar2" "$half"
            "lessEqualVar" "$invulnfmaxfading"
            "greaterVar" "$invulnfmaxfull"
            "resultVar" "$invulnfmaxoutput"
        }
        "LessOrEqual"
        {
            "srcVar1" "$invulnlevel" "srcVar2" "$half"
            "lessEqualVar" "$invulnexponentfading"
            "greaterVar" "$invulnexponentfull"
            "resultVar" "$invulnexponentoutput"
        }
        "Sine"
        {
            "resultVar" "$invulnfmax" "sineperiod" ".3"
            "sinemin" "$invulnfmaxoutput" "sinemax" "18"
        }
        "Sine"
        {
            "resultVar" "$invulnphong" "sineperiod" ".3"
            "sinemin" "$invulnphongoutput" "sinemax" "1"
        }
        "Sine"
        {
            "resultVar" "$invulnexponent" "sineperiod" ".3"
            "sinemin" "$invulnexponentoutput" "sinemax" "13"
        }
        "Equals"
        {
            "srcVar1" "$invulnexponent"
            "resultVar" "$selfillumfresnelminmaxexp[2]"
        }
        "Equals"
        {
            "srcVar1" "$invulnfmax"
            "resultVar" "$selfillumfresnelminmaxexp[1]"
        }
        "Equals" { "srcVar1" "$invulnphong" "resultVar" "$phongboost" }
    }
}
)VMT");
    writeText(app/"base/materials/test/item_tint_proxy.vmt",
              "VertexLitGeneric { $basetexture test/brick $selfillum 1 "
              "$colortint_base \"[ 0.2 0.8 0.4 ]\" Proxies { "
              "ItemTintColor { resultVar $colortint_tmp } "
              "SelectFirstIfNonZero { srcVar1 $colortint_tmp srcVar2 $colortint_base resultVar $color2 } "
              "Multiply { srcVar1 $color2 srcVar2 \"[ 0.5 1 1 ]\" resultVar $selfillumtint } } }");
    writeText(app/"base/materials/test/character_runtime_tint.vmt",
              "VertexLitGeneric { $basetexture test/brick "
              "$color2 \"[ 0 0 0 ]\" $blendtintbybasealpha 1 "
              "$selfillum 1 $selfillumtint \"[ 0 0 0 ]\" Proxies { "
              "Equals { srcVar1 $runtime_character_tint resultVar $color2 } "
              "Equals { srcVar1 $runtime_character_glow resultVar $selfillumtint } } }");
    writeText(app/"base/materials/test/world_material_effects.vmt",
              "LightmappedGeneric { $basetexture test/brick $bumpmap test/brick "
              "$phong 1 $phongexponent 48 $phongboost 1.5 "
              "$phongtint \"[ 0.8 0.7 0.6 ]\" "
              "$envmap env_cubemap $envmaptint \"[ 0.35 0.35 0.35 ]\" }");
    writeText(app/"base/materials/models/props/test_skin.vmt",
              "LightmappedGeneric { $basetexture test/brick }");
    writeText(app/"base/materials/models/props/special_shader.vmt",
              "EyeRefract { $basetexture test/brick $bumpmap test/brick "
              "$phong 1 $envmap env_cubemap }");
    writeText(app/"base/materials/test/ssbump.vmt",
              "LightmappedGeneric { $basetexture test/brick $bumpmap test/brick $ssbump 1 }");
    writeText(app/"base/materials/vgui/hud/test_panel.vmt",
              "UnlitGeneric { $basetexture test/brick }");
    writeText(app/"base/materials/backpack/items/test_icon.vmt",
              "UnlitGeneric { $basetexture test/brick }");
    writeText(app/"base/materials/tools/trigger_volume.vmt",
              "LightmappedGeneric { $basetexture test/brick \"%compiletrigger\" \"1\" }");
    writeText(app/"base/materials/tools/ordinary_brush.vmt",
              "LightmappedGeneric { $basetexture test/brick }");
    writeText(app/"base/materials/decals/modulated_test.vmt",
              "DecalModulate { $basetexture test/brick $decalscale 0.25 }");
    writeText(app/"base/materials/nature/water_coast.vmt",
              "Water { $normalmap nature/water_normal $fogcolor \"{ 35 75 95 }\" "
              "$translucent 1 $reflectamount 0.65 $refractamount 0.12 "
              "$reflectblendfactor 0.75 $nofresnel 1 $scroll1 \"[0.01 0.02 0]\" "
              "$scroll2 \"[-0.03 0.04 0]\" $scale \"[2 3]\" $fogstart 32 $fogend 512 }");
    write(app/"base/materials/nature/water_normal.vtf",makeVtf());
    writeText(app/"base/materials/nature/water_procedural.vmt",
              "Water { $fogcolor \"[ 0.10 0.30 0.42 ]\" }");
    writeText(app/"base/materials/nature/water_flowing.vmt",
              "Water { $normalmap nature/water_normal $flowmap nature/water_flow "
              "$flow_timeintervalinseconds 2 $flow_uvscrolldistance 0.25 "
              "$flow_worlduvscale 1.5 $flow_normaluvscale 2 }");
    write(app/"base/materials/nature/water_flow.vtf",makeVtf());
    writeText(app/"base/materials/nature/displacement_blend.vmt",
              "WorldVertexTransition { $basetexture test/brick "
              "$basetexture2 nature/displacement_layer2 }");
    write(app/"base/materials/nature/displacement_layer2.vtf",makeIa88Vtf());
    write(app/"base/models/editor/test_helper.mdl",makeTinyMdl());
    write(app/"base/models/editor/test_helper_skins.mdl",makeTinySkinnedMdl());
    write(app/"base/models/editor/test_helper_skins.vvd",makeTinyVvd());
    write(app/"base/models/editor/test_helper_skins.dx90.vtx",makeTinyVtx());
    write(app/"base/models/editor/test_helper.vvd",makeTinyVvd());
    write(app/"base/models/editor/test_helper.dx90.vtx",makeTinyVtx());
    write(app/"base/models/editor/test_helper_linear.mdl",makeTinyLinearBoneMdl());
    write(app/"base/models/editor/test_helper_linear.vvd",makeTinyVvd());
    write(app/"base/models/editor/test_helper_linear.dx90.vtx",makeTinyVtx());
    write(app/"base/models/editor/test_helper_reference_pose.mdl",makeTinyReferencePoseMdl());
    write(app/"base/models/editor/test_helper_reference_pose.vvd",makeTinyVvd());
    write(app/"base/models/editor/test_helper_reference_pose.dx90.vtx",makeTinyVtx());
    write(app/"base/models/editor/test_helper_animated.mdl",makeTinyAnimatedMdl());
    write(app/"base/models/editor/test_helper_animated.vvd",makeTinyVvd());
    write(app/"base/models/editor/test_helper_animated.dx90.vtx",makeTinyVtx());
    write(app/"base/models/editor/test_helper_included_root.mdl",makeTinyIncludedAnimationRootMdl());
    write(app/"base/models/editor/test_helper_included_root.vvd",makeTinyVvd());
    write(app/"base/models/editor/test_helper_included_root.dx90.vtx",makeTinyVtx());
    write(app/"base/models/editor/test_helper_included_animations.mdl",makeTinyIncludedAnimationMdl());
    write(app/"base/models/editor/test_helper_tf2_virtual_root.mdl",makeTinyTf2VirtualRootMdl());
    write(app/"base/models/editor/test_helper_tf2_virtual_root.vvd",makeTinyVvd());
    write(app/"base/models/editor/test_helper_tf2_virtual_root.dx90.vtx",makeTinyVtx());
    write(app/"base/models/editor/test_helper_tf2_human_root.mdl",makeTinyTf2HumanRootMdl());
    write(app/"base/models/editor/test_helper_tf2_human_root.vvd",makeTinyVvd());
    write(app/"base/models/editor/test_helper_tf2_human_root.dx90.vtx",makeTinyVtx());
    write(app/"base/models/player/test_tf2_sequence_group.mdl",makeTinyTf2SequenceGroupMdl());
    write(app/"base/models/player/test_tf2_channel_group.mdl",makeTinyTf2ChannelGroupMdl());
    write(app/"base/models/player/test_tf2_channel_group.ani",makeTinyTf2ChannelAni());
    write(app/"base/models/editor/test_helper_neutral_blend.mdl",makeTinyNeutralBlendMdl());
    write(app/"base/models/editor/test_helper_neutral_blend.vvd",makeTinyVvd());
    write(app/"base/models/editor/test_helper_neutral_blend.dx90.vtx",makeTinyVtx());
    write(app/"base/models/editor/test_helper_rotating.mdl",makeTinyRotatingAnimatedMdl());
    write(app/"base/models/editor/test_helper_rotating.vvd",makeTinyVvd());
    write(app/"base/models/editor/test_helper_rotating.dx90.vtx",makeTinyVtx());
    write(app/"base/models/editor/test_helper_external.mdl",makeTinyExternalAnimatedMdl());
    write(app/"base/models/editor/test_helper_external.vvd",makeTinyVvd());
    write(app/"base/models/editor/test_helper_external.dx90.vtx",makeTinyVtx());
    write(app/"base/models/editor/test_helper_external.ani",makeTinyExternalAni());

    hammer::assets::SteamLibraries libraries({steam});
    assert(libraries.resolveApp(123)==fs::weakly_canonical(app));
    auto fsys=std::make_shared<hammer::assets::GameFileSystem>();
    assert(fsys->configure(app/"mod/gameinfo.txt",&error,&libraries));
    assert(fsys->exists("materials/custom_marker.txt"));
    assert(fsys->exists("materials/test/material.vmt"));
    hammer::assets::MaterialSystem materials(fsys);
    hammer::assets::StudioModelSystem studioModels(fsys);
    const auto helperModel=studioModels.model("models/editor/test_helper.mdl");
    assert(helperModel && helperModel->valid && helperModel->meshes.size()==1);
    assert(helperModel->meshes[0].vertices.size()==3);
    assert(helperModel->meshes[0].material=="test/material");
    const auto skinnedModel=studioModels.model("models/editor/test_helper_skins.mdl");
    assert(skinnedModel && skinnedModel->valid && skinnedModel->skinCount()==2);
    assert(skinnedModel->meshes[0].materialSlot==0);
    assert(skinnedModel->materialForSkin(0,0)=="test/material");
    assert(skinnedModel->materialForSkin(0,1)=="test/material_skin1");
    assert(skinnedModel->materialForSkin(0,99)=="test/material");
    // Bind-pose vertices are already in model/pose space. Root, child, and
    // blended weights must all remain unchanged after boneToPose * poseToBone.
    const auto& bindOrigin=helperModel->meshes[0].vertices[0];
    const auto& bindChild=helperModel->meshes[0].vertices[1];
    const auto& bindBlended=helperModel->meshes[0].vertices[2];
    assert(std::abs(bindOrigin.x-0.0f)<0.001f && std::abs(bindOrigin.y-0.0f)<0.001f);
    assert(std::abs(bindChild.x-16.0f)<0.001f && std::abs(bindChild.y-0.0f)<0.001f);
    assert(std::abs(bindBlended.x-0.0f)<0.001f && std::abs(bindBlended.y-16.0f)<0.001f);
    assert(std::abs(bindChild.nx)<0.001f && std::abs(bindChild.ny)<0.001f &&
           std::abs(bindChild.nz-1.0f)<0.001f);
    assert(bindOrigin.hasTangent && bindChild.hasTangent && bindBlended.hasTangent);
    assert(std::abs(bindOrigin.tx-1.0f)<0.001f && std::abs(bindOrigin.ty)<0.001f &&
           std::abs(bindOrigin.tz)<0.001f && bindOrigin.tangentSign>0.0f);

    const auto linearModel=studioModels.model("models/editor/test_helper_linear.mdl");
    assert(linearModel && linearModel->valid && linearModel->meshes.size()==1);
    assert(linearModel->meshes[0].vertices.size()==3);
    const auto& linearChild=linearModel->meshes[0].vertices[1];
    const auto& linearBlended=linearModel->meshes[0].vertices[2];
    assert(std::abs(linearChild.x-16.0f)<0.001f && std::abs(linearChild.y)<0.001f);
    assert(std::abs(linearBlended.x)<0.001f && std::abs(linearBlended.y-16.0f)<0.001f);

    const auto referencePoseModel=studioModels.model("models/editor/test_helper_reference_pose.mdl");
    assert(referencePoseModel && referencePoseModel->valid && referencePoseModel->meshes.size()==1);
    assert(referencePoseModel->meshes[0].vertices.size()==3);
    const auto& referenceRoot=referencePoseModel->meshes[0].vertices[0];
    const auto& referenceChild=referencePoseModel->meshes[0].vertices[1];
    const auto& referenceBlended=referencePoseModel->meshes[0].vertices[2];
    assert(std::abs(referenceRoot.x)<0.001f && std::abs(referenceRoot.y)<0.001f &&
           std::abs(referenceRoot.z)<0.001f);
    assert(std::abs(referenceChild.x-16.0f)<0.002f && std::abs(referenceChild.y)<0.002f &&
           std::abs(referenceChild.z)<0.002f);
    assert(std::abs(referenceBlended.x)<0.002f && std::abs(referenceBlended.y-16.0f)<0.002f &&
           std::abs(referenceBlended.z)<0.002f);
    const auto animatedModel=studioModels.model("models/editor/test_helper_animated.mdl");
    assert(animatedModel && animatedModel->valid && animatedModel->sequenceCount()==1);
    assert(animatedModel->sequences[0].label=="root_move");
    assert(animatedModel->sequences[0].looping && animatedModel->sequences[0].frameCount==2);
    assert(std::abs(animatedModel->sequences[0].fps-10.0f)<0.001f);
    assert(std::abs(animatedModel->sequences[0].duration-0.1f)<0.001f);
    assert(animatedModel->sequenceIndex("ROOT_MOVE")==0);
    assert(animatedModel->sequenceIndex("0")==0);
    std::vector<std::vector<hammer::assets::StudioVertex>> animationStart;
    std::vector<std::vector<hammer::assets::StudioVertex>> animationMiddle;
    assert(studioModels.sampleAnimation(*animatedModel,0,0.0,animationStart));
    assert(studioModels.sampleAnimation(*animatedModel,0,0.5,animationMiddle));
    assert(animationStart.size()==1 && animationMiddle.size()==1);
    assert(animationStart[0].size()==3 && animationMiddle[0].size()==3);
    assert(std::abs(animationStart[0][0].x-animatedModel->meshes[0].vertices[0].x)<0.002f);
    const float animatedDistance=std::abs(animationMiddle[0][0].x-animationStart[0][0].x)+
                                 std::abs(animationMiddle[0][0].y-animationStart[0][0].y)+
                                 std::abs(animationMiddle[0][0].z-animationStart[0][0].z);
    assert(animatedDistance>4.9f && animatedDistance<5.1f);
    std::vector<hammer::assets::StudioBoneMatrix> animationPalette;
    assert(studioModels.sampleAnimationMatrices(*animatedModel,0,0.5,animationPalette));
    assert(!animationPalette.empty());
    const auto& paletteMatrix=animationPalette.front().values;
    const auto& bindVertex=animatedModel->meshes[0].vertices[0];
    const float paletteX=paletteMatrix[0]*bindVertex.sourcePosition[0]+
                         paletteMatrix[1]*bindVertex.sourcePosition[1]+
                         paletteMatrix[2]*bindVertex.sourcePosition[2]+paletteMatrix[3];
    const float paletteY=paletteMatrix[4]*bindVertex.sourcePosition[0]+
                         paletteMatrix[5]*bindVertex.sourcePosition[1]+
                         paletteMatrix[6]*bindVertex.sourcePosition[2]+paletteMatrix[7];
    const float paletteZ=paletteMatrix[8]*bindVertex.sourcePosition[0]+
                         paletteMatrix[9]*bindVertex.sourcePosition[1]+
                         paletteMatrix[10]*bindVertex.sourcePosition[2]+paletteMatrix[11];
    assert(std::abs(paletteX-animationMiddle[0][0].x)<0.002f);
    assert(std::abs(paletteY-animationMiddle[0][0].y)<0.002f);
    assert(std::abs(paletteZ-animationMiddle[0][0].z)<0.002f);
    assert(animatedModel->referencePoseMatrices.size()==animationPalette.size());

    const auto includedModel=studioModels.model("models/editor/test_helper_included_root.mdl");
    assert(includedModel && includedModel->valid && includedModel->sequenceCount()==1);
    assert(includedModel->sequences[0].label=="included_player_move");
    assert(includedModel->sequenceIndex("INCLUDED_PLAYER_MOVE")==0);
    std::vector<std::vector<hammer::assets::StudioVertex>> includedStart;
    std::vector<std::vector<hammer::assets::StudioVertex>> includedMiddle;
    assert(studioModels.sampleAnimation(*includedModel,0,0.0,includedStart));
    assert(studioModels.sampleAnimation(*includedModel,0,0.5,includedMiddle));
    const float includedDistance=std::abs(includedMiddle[0][0].x-includedStart[0][0].x)+
                                 std::abs(includedMiddle[0][0].y-includedStart[0][0].y)+
                                 std::abs(includedMiddle[0][0].z-includedStart[0][0].z);
    assert(includedDistance>4.9f && includedDistance<5.1f);

    // TF2 player-style virtual model: the sequence group contains a local
    // animation alias, while a sibling include owns the same-named compressed
    // channels in a basename-relative ANI file at external offset zero.
    const auto tf2VirtualModel=studioModels.model("models/editor/test_helper_tf2_virtual_root.mdl");
    assert(tf2VirtualModel && tf2VirtualModel->valid && tf2VirtualModel->sequenceCount()==1);
    assert(tf2VirtualModel->sequences[0].label=="tf2_player_virtual_move");
    std::vector<std::vector<hammer::assets::StudioVertex>> tf2VirtualStart;
    std::vector<std::vector<hammer::assets::StudioVertex>> tf2VirtualMiddle;
    assert(studioModels.sampleAnimation(*tf2VirtualModel,0,0.0,tf2VirtualStart));
    assert(studioModels.sampleAnimation(*tf2VirtualModel,0,0.5,tf2VirtualMiddle));
    const float tf2VirtualDistance=
        std::abs(tf2VirtualMiddle[0][0].x-tf2VirtualStart[0][0].x)+
        std::abs(tf2VirtualMiddle[0][0].y-tf2VirtualStart[0][0].y)+
        std::abs(tf2VirtualMiddle[0][0].z-tf2VirtualStart[0][0].z);
    assert(tf2VirtualDistance>4.9f && tf2VirtualDistance<5.1f);

    // The same unnamed animation skeleton must also map onto a human-style
    // render skeleton that has additional bones. Equal bone counts must not be
    // a hidden requirement for virtual-model animation playback.
    const auto tf2HumanModel=studioModels.model("models/editor/test_helper_tf2_human_root.mdl");
    assert(tf2HumanModel && tf2HumanModel->valid && tf2HumanModel->sequenceCount()==1);
    std::vector<std::vector<hammer::assets::StudioVertex>> tf2HumanStart;
    std::vector<std::vector<hammer::assets::StudioVertex>> tf2HumanMiddle;
    assert(studioModels.sampleAnimation(*tf2HumanModel,0,0.0,tf2HumanStart));
    assert(studioModels.sampleAnimation(*tf2HumanModel,0,0.5,tf2HumanMiddle));
    const float tf2HumanDistance=
        std::abs(tf2HumanMiddle[0][0].x-tf2HumanStart[0][0].x)+
        std::abs(tf2HumanMiddle[0][0].y-tf2HumanStart[0][0].y)+
        std::abs(tf2HumanMiddle[0][0].z-tf2HumanStart[0][0].z);
    assert(tf2HumanDistance>4.9f && tf2HumanDistance<5.1f);

    const auto neutralBlendModel=studioModels.model("models/editor/test_helper_neutral_blend.mdl");
    assert(neutralBlendModel && neutralBlendModel->valid && neutralBlendModel->sequenceCount()==1);
    assert(neutralBlendModel->sequences[0].frameCount==2);
    std::vector<std::vector<hammer::assets::StudioVertex>> neutralStart;
    std::vector<std::vector<hammer::assets::StudioVertex>> neutralMiddle;
    assert(studioModels.sampleAnimation(*neutralBlendModel,0,0.0,neutralStart));
    assert(studioModels.sampleAnimation(*neutralBlendModel,0,0.5,neutralMiddle));
    const float neutralDistance=std::abs(neutralMiddle[0][0].x-neutralStart[0][0].x)+
                                std::abs(neutralMiddle[0][0].y-neutralStart[0][0].y)+
                                std::abs(neutralMiddle[0][0].z-neutralStart[0][0].z);
    assert(neutralDistance>4.9f && neutralDistance<5.1f);

    const auto rotatingModel=studioModels.model("models/editor/test_helper_rotating.mdl");
    assert(rotatingModel && rotatingModel->valid && rotatingModel->sequenceCount()==1);
    std::vector<std::vector<hammer::assets::StudioVertex>> rotatingStart;
    std::vector<std::vector<hammer::assets::StudioVertex>> rotatingMiddle;
    assert(studioModels.sampleAnimation(*rotatingModel,0,0.0,rotatingStart));
    assert(studioModels.sampleAnimation(*rotatingModel,0,0.5,rotatingMiddle));
    assert(rotatingStart.size()==1 && rotatingMiddle.size()==1);
    assert(std::abs(rotatingStart[0][0].x-rotatingModel->meshes[0].vertices[0].x)<0.002f);
    assert(std::abs(rotatingStart[0][0].y-rotatingModel->meshes[0].vertices[0].y)<0.002f);
    // T(9,0) * R(135deg) * inverse(T(4,0) * R(90deg)) maps the
    // root-bound origin to approximately (6.1716,-2.8284,0).
    assert(std::abs(rotatingMiddle[0][0].x-6.171573f)<0.01f);
    assert(std::abs(rotatingMiddle[0][0].y+2.828427f)<0.01f);
    const float rotatingNormalLength=std::sqrt(
        rotatingMiddle[0][0].nx*rotatingMiddle[0][0].nx+
        rotatingMiddle[0][0].ny*rotatingMiddle[0][0].ny+
        rotatingMiddle[0][0].nz*rotatingMiddle[0][0].nz);
    assert(std::abs(rotatingNormalLength-1.0f)<0.001f);

    const auto externalAnimatedModel=studioModels.model("models/editor/test_helper_external.mdl");
    assert(externalAnimatedModel && externalAnimatedModel->valid &&
           externalAnimatedModel->sequenceCount()==1);
    std::vector<std::vector<hammer::assets::StudioVertex>> externalStart;
    std::vector<std::vector<hammer::assets::StudioVertex>> externalMiddle;
    assert(studioModels.sampleAnimation(*externalAnimatedModel,0,0.0,externalStart));
    assert(studioModels.sampleAnimation(*externalAnimatedModel,0,0.5,externalMiddle));
    assert(externalStart.size()==1 && externalMiddle.size()==1);
    const float externalDistance=std::abs(externalMiddle[0][0].x-externalStart[0][0].x)+
                                 std::abs(externalMiddle[0][0].y-externalStart[0][0].y)+
                                 std::abs(externalMiddle[0][0].z-externalStart[0][0].z);
    assert(externalDistance>4.9f && externalDistance<5.1f);
    assert(std::abs(externalStart[0][0].x-externalAnimatedModel->meshes[0].vertices[0].x)<0.002f);

    if (std::getenv("HAMMER_ANIMATION_ONLY")) {
        std::cout << "studio animation tests passed\n";
        fs::remove_all(temp);
        return 0;
    }

    const auto material=materials.material("test/material");
    assert(material && !material->missing && material->image.width==2 && material->image.height==2);
    assert((material->image.pixels[0]&0x00ffffffu)==0x00ff0000u);
    const auto alphaBlend=materials.material("test/alpha_blend");
    assert(alphaBlend && alphaBlend->translucent && std::abs(alphaBlend->alpha-0.35f)<0.001f);
    assert(!alphaBlend->alphaTest);
    const auto alphaCutout=materials.material("test/alpha_cutout");
    assert(alphaCutout && alphaCutout->alphaTest && !alphaCutout->translucent);
    assert(std::abs(alphaCutout->alphaTestReference-0.3f)<0.001f);
    const auto effectsMaterial=materials.material("test/material_effects");
    assert(effectsMaterial && !effectsMaterial->missing);
    assert(effectsMaterial->bumpMapped && effectsMaterial->bumpImage.valid());
    assert(effectsMaterial->bumpMap=="test/phong_mask");
    assert(((effectsMaterial->bumpImage.pixels[0]>>24)&255u)==64u);
    assert(effectsMaterial->phong && effectsMaterial->specular);
    assert(effectsMaterial->editorBumpMapSupported && effectsMaterial->editorPhongSupported &&
           effectsMaterial->editorSpecularSupported &&
           effectsMaterial->editorSelfIllumSupported &&
           effectsMaterial->editorRimLightSupported);
    assert(!effectsMaterial->phongMaskFromBaseAlpha);
    assert(!effectsMaterial->envMapMaskFromBaseAlpha &&
           effectsMaterial->envMapMaskFromNormalAlpha &&
           effectsMaterial->invertPhongMask);
    assert(std::abs(effectsMaterial->phongExponent-150.0f)<0.001f &&
           !effectsMaterial->phongExponentOverride);
    assert(std::abs(effectsMaterial->phongBoost-2.0f)<0.001f);
    assert(std::abs(effectsMaterial->phongFresnelRanges[0]-0.1f)<0.001f);
    assert(std::abs(effectsMaterial->phongFresnelRanges[1]-0.45f)<0.001f);
    assert(std::abs(effectsMaterial->phongFresnelRanges[2]-1.0f)<0.001f);
    assert(effectsMaterial->phongTintDefined &&
           std::abs(effectsMaterial->phongTint[0])<0.001f &&
           effectsMaterial->phongAlbedoTint);
    assert(effectsMaterial->hasPhongExponentTexture &&
           effectsMaterial->phongExponentImage.valid());
    assert(effectsMaterial->selfIllum && effectsMaterial->rimLight);
    assert(effectsMaterial->hasSelfIllumMask &&
           effectsMaterial->selfIllumMaskImage.valid());
    assert(std::abs(effectsMaterial->selfIllumTint[0]-0.4f)<0.001f &&
           std::abs(effectsMaterial->selfIllumTint[1]-0.8f)<0.001f &&
           std::abs(effectsMaterial->selfIllumTint[2]-1.2f)<0.001f);
    assert(std::abs(effectsMaterial->rimLightExponent-6.0f)<0.001f &&
           std::abs(effectsMaterial->rimLightBoost-2.5f)<0.001f &&
           effectsMaterial->rimMaskFromExponentAlpha);
    assert(std::abs(effectsMaterial->envMapTint[0]-0.5f)<0.001f &&
           std::abs(effectsMaterial->envMapTint[2]-0.3f)<0.001f);
    assert(effectsMaterial->envMap=="env_cubemap");
    assert(effectsMaterial->envMapUsesMapCubemap && !effectsMaterial->hasEnvMapCube);
    const auto customEnvMaterial=materials.material("test/custom_envmap");
    assert(customEnvMaterial && !customEnvMaterial->missing && customEnvMaterial->specular);
    assert(customEnvMaterial->envMap=="cubemaps/custom_env");
    assert(!customEnvMaterial->envMapUsesMapCubemap && customEnvMaterial->hasEnvMapCube);
    assert(customEnvMaterial->envMapCube.valid() && customEnvMaterial->envMapError.empty());
    assert(!customEnvMaterial->envMapSource.empty());
    const auto missingEnvMaterial=materials.material("test/missing_envmap");
    assert(missingEnvMaterial && !missingEnvMaterial->missing && missingEnvMaterial->specular);
    assert(!missingEnvMaterial->envMapUsesMapCubemap && !missingEnvMaterial->hasEnvMapCube);
    assert(missingEnvMaterial->envMapError.find("not found")!=std::string::npos);
    const auto runtimeTintCharacter =
        materials.material("test/character_runtime_tint");
    assert(runtimeTintCharacter && !runtimeTintCharacter->missing);
    assert(!runtimeTintCharacter->color2Active);
    assert(!runtimeTintCharacter->highEnergyEffect && !runtimeTintCharacter->uberEffect);
    assert(std::abs(runtimeTintCharacter->color2[0]-1.0f)<0.001f &&
           std::abs(runtimeTintCharacter->color2[1]-1.0f)<0.001f &&
           std::abs(runtimeTintCharacter->color2[2]-1.0f)<0.001f);
    assert(std::abs(runtimeTintCharacter->selfIllumTint[0]-1.0f)<0.001f &&
           std::abs(runtimeTintCharacter->selfIllumTint[1]-1.0f)<0.001f &&
           std::abs(runtimeTintCharacter->selfIllumTint[2]-1.0f)<0.001f);
    const auto heavyPlayerMaterial =
        materials.material("models/player/hvyweapon/hvyweapon_red");
    assert(heavyPlayerMaterial && !heavyPlayerMaterial->missing);
    assert(heavyPlayerMaterial->phong && heavyPlayerMaterial->rimLight &&
           heavyPlayerMaterial->hasLightWarpTexture);
    assert(!heavyPlayerMaterial->color2Active && !heavyPlayerMaterial->uberEffect);
    assert(std::abs(heavyPlayerMaterial->color2[0]-1.0f)<0.001f &&
           std::abs(heavyPlayerMaterial->color2[1]-1.0f)<0.001f &&
           std::abs(heavyPlayerMaterial->color2[2]-1.0f)<0.001f);
    const auto uberMaterial=materials.material("models/effects/invulnfx_red");
    assert(uberMaterial && !uberMaterial->missing && uberMaterial->modelGlowProxy);
    assert(uberMaterial->uberEffect && uberMaterial->highEnergyEffect);
    assert(uberMaterial->blendTintByBaseAlpha &&
           std::abs(uberMaterial->blendTintColorOverBase-0.85f)<0.001f);
    assert(uberMaterial->highEnergyEffect);
    assert(uberMaterial->color2[0]>0.99f && uberMaterial->color2[1]<0.11f &&
           uberMaterial->color2[2]<0.07f);
    assert(std::abs(uberMaterial->selfIllumTint[0]-uberMaterial->color2[0])<0.001f &&
           std::abs(uberMaterial->selfIllumTint[1]-uberMaterial->color2[1])<0.001f &&
           std::abs(uberMaterial->selfIllumTint[2]-uberMaterial->color2[2])<0.001f);
    const auto robotUberMaterial =
        materials.material("models/bots/engineer/robot_invulnerability");
    assert(robotUberMaterial && !robotUberMaterial->missing);
    // Runtime ModelGlowColor/self-illumination metadata is not sufficient to
    // classify an ordinary character material as a forced invulnerability VMT.
    assert(robotUberMaterial->modelGlowProxy && !robotUberMaterial->uberEffect &&
           robotUberMaterial->highEnergyEffect);
    const auto robotRed = previewUberColor(*robotUberMaterial, 0);
    const auto robotBlue = previewUberColor(*robotUberMaterial, 1);
    assert(robotRed[0] > 0.99f && robotRed[1] < 0.11f && robotRed[2] < 0.07f);
    assert(robotBlue[0] < 0.13f && robotBlue[1] > 0.37f && robotBlue[2] > 0.99f);
    const auto stockUberMaterial =
        materials.material("models/effects/invulnfx_stock_red");
    assert(stockUberMaterial && !stockUberMaterial->missing);
    assert(stockUberMaterial->uberEffect && stockUberMaterial->highEnergyEffect);
    assert(stockUberMaterial->modelGlowProxy && stockUberMaterial->invulnLevelProxy);
    assert(stockUberMaterial->selfIllum && stockUberMaterial->selfIllumFresnel);
    assert(stockUberMaterial->halfLambert && stockUberMaterial->hasLightWarpTexture);
    assert(stockUberMaterial->envMap == "effects/invun_red" &&
           stockUberMaterial->hasEnvMapCube);
    assert(stockUberMaterial->bumpMapped && stockUberMaterial->bumpFrames.size() == 2);
    assert(std::abs(stockUberMaterial->bumpAnimationFrameRate - 70.0f) < 0.001f);
    assert(stockUberMaterial->previewAnimated);
    assert(std::abs(stockUberMaterial->selfIllumFresnelMinMaxExp[0]) < 0.001f &&
           std::abs(stockUberMaterial->selfIllumFresnelMinMaxExp[1] - 18.0f) < 0.001f &&
           std::abs(stockUberMaterial->selfIllumFresnelMinMaxExp[2] - 13.0f) < 0.001f);
    assert(std::abs(stockUberMaterial->phongBoost - 1.0f) < 0.001f);
    assert(stockUberMaterial->color2Active && stockUberMaterial->color2[0] > 0.99f &&
           stockUberMaterial->color2[1] < 0.11f && stockUberMaterial->color2[2] < 0.07f);
    const auto itemTintMaterial=materials.material("test/item_tint_proxy");
    assert(itemTintMaterial && itemTintMaterial->itemTintProxy);
    assert(std::abs(itemTintMaterial->color2[0]-0.2f)<0.001f &&
           std::abs(itemTintMaterial->color2[1]-0.8f)<0.001f &&
           std::abs(itemTintMaterial->color2[2]-0.4f)<0.001f);
    assert(std::abs(itemTintMaterial->selfIllumTint[0]-0.1f)<0.001f &&
           std::abs(itemTintMaterial->selfIllumTint[1]-0.8f)<0.001f &&
           std::abs(itemTintMaterial->selfIllumTint[2]-0.4f)<0.001f);
    assert(itemTintMaterial->color2Active);
    if (std::getenv("HAMMER_MATERIAL_PROXY_ONLY")) {
        std::cout << "material proxy tests passed\n";
        fs::remove_all(temp);
        return 0;
    }

    const auto ordinaryUberPathMaterial =
        materials.material("test/uber_directory/ordinary_material");
    assert(ordinaryUberPathMaterial && !ordinaryUberPathMaterial->missing);
    assert(!ordinaryUberPathMaterial->uberEffect);
    assert(!ordinaryUberPathMaterial->highEnergyEffect);
    const auto worldEffectsMaterial=materials.material("test/world_material_effects");
    assert(worldEffectsMaterial && !worldEffectsMaterial->missing);
    assert(worldEffectsMaterial->shader=="LightmappedGeneric");
    assert(worldEffectsMaterial->bumpMapped && worldEffectsMaterial->phong &&
           worldEffectsMaterial->specular);
    assert(worldEffectsMaterial->phongExponentOverride &&
           std::abs(worldEffectsMaterial->phongExponent-48.0f)<0.001f);
    assert(worldEffectsMaterial->phongTintDefined &&
           std::abs(worldEffectsMaterial->phongTint[0]-0.8f)<0.001f);
    assert(worldEffectsMaterial->editorBumpMapSupported &&
           worldEffectsMaterial->editorPhongSupported &&
           worldEffectsMaterial->editorSpecularSupported &&
           !worldEffectsMaterial->editorSelfIllumSupported &&
           !worldEffectsMaterial->editorRimLightSupported);
    const auto specialMaterial=materials.material("models/props/special_shader");
    assert(specialMaterial && !specialMaterial->missing);
    assert(!specialMaterial->editorBumpMapSupported && !specialMaterial->editorPhongSupported &&
           !specialMaterial->editorSpecularSupported &&
           !specialMaterial->editorSelfIllumSupported &&
           !specialMaterial->editorRimLightSupported);
    const auto ssBumpMaterial=materials.material("test/ssbump");
    assert(ssBumpMaterial && ssBumpMaterial->bumpMapped && ssBumpMaterial->ssBump);
    const auto triggerMaterial=materials.material("tools/trigger_volume");
    assert(triggerMaterial && !triggerMaterial->missing && triggerMaterial->compileTrigger);
    const auto ordinaryMaterial=materials.material("tools/ordinary_brush");
    assert(ordinaryMaterial && !ordinaryMaterial->missing && !ordinaryMaterial->compileTrigger);
    const auto modulatedDecal=materials.material("decals/modulated_test");
    assert(modulatedDecal && !modulatedDecal->missing && modulatedDecal->decalModulate);
    assert(std::abs(modulatedDecal->decalScale-0.25f)<0.001f);
    const auto waterCoast=materials.material("nature/water_coast");
    assert(waterCoast && !waterCoast->missing && waterCoast->water);
    assert(waterCoast->translucent && waterCoast->previewAnimated && waterCoast->image.valid());
    assert(waterCoast->waterNormalImage.valid());
    assert(waterCoast->shader=="Water" && waterCoast->baseTexture=="nature/water_normal");
    assert(!waterCoast->note.empty());
    assert(waterCoast->waterAlpha > 0.93f && waterCoast->waterAlpha < 0.95f);
    assert(std::abs(waterCoast->waterReflectAmount-0.65f)<0.0001f);
    assert(std::abs(waterCoast->waterRefractAmount-0.12f)<0.0001f);
    assert(std::abs(waterCoast->waterReflectBlendFactor-0.75f)<0.0001f);
    assert(std::abs(waterCoast->waterFogStart-32.0f)<0.0001f &&
           std::abs(waterCoast->waterFogEnd-512.0f)<0.0001f);
    assert(waterCoast->waterNoFresnel && waterCoast->waterMultiTexture);
    assert(std::abs(waterCoast->waterScale[0]-2.0f)<0.0001f &&
           std::abs(waterCoast->waterScale[1]-3.0f)<0.0001f);
    assert(std::abs(waterCoast->waterScroll1[0]-0.01f)<0.0001f &&
           std::abs(waterCoast->waterScroll2[0]+0.03f)<0.0001f);
    const std::uint32_t waterPreviewPixel=waterCoast->image.pixels[0];
    assert((waterPreviewPixel&0x00ffffffu)!=0x00eb3cebu);
    const int previewR=static_cast<int>((waterPreviewPixel>>16)&255u);
    const int previewG=static_cast<int>((waterPreviewPixel>>8)&255u);
    const int previewB=static_cast<int>(waterPreviewPixel&255u);
    const auto colorDistance=[](int r,int g,int b,int targetR,int targetG,int targetB) {
        const int dr=r-targetR, dg=g-targetG, db=b-targetB;
        return dr*dr+dg*dg+db*db;
    };
    assert(colorDistance(previewR,previewG,previewB,35,75,95) <
           colorDistance(previewR,previewG,previewB,210,232,255));
    const auto proceduralWater=materials.material("nature/water_procedural");
    assert(proceduralWater && !proceduralWater->missing && proceduralWater->water);
    assert(proceduralWater->image.valid() && proceduralWater->vtfSource.empty());
    assert(proceduralWater->waterNormalImage.valid());
    assert(proceduralWater->note.find("procedural ripples")!=std::string::npos);

    const auto flowingWater=materials.material("nature/water_flowing");
    assert(flowingWater && !flowingWater->missing && flowingWater->water);
    assert(flowingWater->waterHasFlowMap && flowingWater->waterFlowImage.valid());
    assert(flowingWater->waterFlowMap=="nature/water_flow");
    assert(!flowingWater->waterFlowSource.empty());
    // Flow maps stay raw. makeVtf()'s first BGRA texel decodes to R=255,
    // G=0: full leftward and downward flow under the shader convention.
    const std::uint32_t flowPixel=flowingWater->waterFlowImage.pixels[0];
    assert(((flowPixel>>16)&255u)==255u);
    assert(((flowPixel>>8)&255u)==0u);
    assert(std::abs(flowingWater->waterFlowCycleRate-0.5f)<0.0001f);
    assert(std::abs(flowingWater->waterFlowDistance-0.25f)<0.0001f);
    assert(std::abs(flowingWater->waterFlowMapScale-1.5f)<0.0001f);
    assert(std::abs(flowingWater->waterFlowNormalUvScale-2.0f)<0.0001f);
    assert(flowingWater->note.find("Flow-mapped")!=std::string::npos);

    const auto displacementBlend=materials.material("nature/displacement_blend");
    assert(displacementBlend && !displacementBlend->missing);
    assert(displacementBlend->shader=="WorldVertexTransition");
    assert(displacementBlend->baseTexture=="test/brick");
    assert(displacementBlend->baseTexture2=="nature/displacement_layer2");
    assert(displacementBlend->image.valid() && displacementBlend->image2.valid());
    assert(displacementBlend->blended);
    assert(displacementBlend->note.find("Displacement blend")!=std::string::npos);

    const auto browsableNames = materials.materialNames();
    assert(std::find(browsableNames.begin(), browsableNames.end(), "nature/water_coast") != browsableNames.end());
    assert(std::find(browsableNames.begin(), browsableNames.end(), "models/props/test_skin") == browsableNames.end());
    assert(std::find(browsableNames.begin(), browsableNames.end(), "vgui/hud/test_panel") == browsableNames.end());
    assert(std::find(browsableNames.begin(), browsableNames.end(), "backpack/items/test_icon") == browsableNames.end());

    const auto animatedFrames=hammer::assets::MaterialSystem::decodeVtfFrames(makeAnimatedVtf());
    assert(animatedFrames && animatedFrames->size()==2);
    assert((*animatedFrames)[0].valid() && (*animatedFrames)[1].valid());
    assert((*animatedFrames)[0].pixels[0] != (*animatedFrames)[1].pixels[0]);
    const auto decoded=hammer::assets::MaterialSystem::decodeVtf(makeVtf());
    assert(decoded && decoded->valid());
    const auto decodedCube=hammer::assets::MaterialSystem::decodeVtfCubemap(makeCubemapVtf());
    assert(decodedCube && decodedCube->valid());
    constexpr std::array<std::uint32_t,6> ExpectedCubeColors{{
        0x00ff0000u,0x0000ff00u,0x000000ffu,
        0x00ffff00u,0x00ff00ffu,0x0000ffffu}};
    for(std::size_t face=0;face<ExpectedCubeColors.size();++face)
        assert((decodedCube->faces[face].pixels[0]&0x00ffffffu)==ExpectedCubeColors[face]);
    const auto ia88=hammer::assets::MaterialSystem::decodeVtf(makeIa88Vtf());
    assert(ia88 && ia88->valid());
    assert(((ia88->pixels[0] >> 24) & 255) == 255);

    // Real Source gameinfo files refer to "name.vpk", while the directory
    // archive stored on disk is "name_dir.vpk". They can also depend on a
    // second Steam app through DependsOnAppID and |appid_N| paths.
    const fs::path sdkApp=steam/"steamapps/common/Source SDK Base 2013 Singleplayer";
    const fs::path tfApp=steam/"steamapps/common/Team Fortress 2";
    writeText(steam/"steamapps/appmanifest_243750.acf",
              "\"AppState\" { \"appid\" \"243750\" \"installdir\" \"Source SDK Base 2013 Singleplayer\" }");
    writeText(steam/"steamapps/appmanifest_440.acf",
              "\"AppState\" { \"appid\" \"440\" \"installdir\" \"Team Fortress 2\" }");
    write(tfApp/"tf/tf2_misc_dir.vpk",makeVpk());
    writeText(tfApp/"tf/materials/shared/source.txt", "external");
    writeText(sdkApp/"zmb/materials/local_root.txt", "game-root");
    writeText(sdkApp/"zmb/tf/materials/shared/source.txt", "local-override");
    writeText(sdkApp/"zmb/gameinfo.txt",
        "GameInfo\n"
        "{\n"
        "  DependsOnAppID 440\n"
        "  FileSystem\n"
        "  {\n"
        "    SteamAppId 243750\n"
        "    SearchPaths\n"
        "    {\n"
        "      game+mod+custom_mod+vgui |gameinfo_path|custom/*\n"
        "      game+mod+vgui |appid_440|tf/tf2_misc.vpk\n"
        "      game |gameinfo_path|.\n"
        "    }\n"
        "  }\n"
        "}\n");
    hammer::assets::SteamLibraries sourceLibraries({steam});
    auto sourceFs=std::make_shared<hammer::assets::GameFileSystem>();
    error.message.clear();
    assert(sourceFs->configure(sdkApp/"zmb/gameinfo.txt",&error,&sourceLibraries));
    assert(error.message.empty());
    assert(sourceFs->vpkCount()==1);
    assert(sourceFs->exists("materials/hello.txt"));
    assert(sourceFs->exists("materials/local_root.txt"));
    const auto localOverride = sourceFs->readFile("materials/shared/source.txt");
    assert(localOverride && std::string(localOverride->begin(), localOverride->end()) == "local-override");
    const auto localTf = fs::weakly_canonical(sdkApp/"zmb/tf");
    assert(std::any_of(sourceFs->locations().begin(), sourceFs->locations().end(),
        [&](const hammer::assets::SearchLocation& item) {
            return item.kind == hammer::assets::SearchLocation::Kind::Directory && item.path == localTf;
        }));

    // AppID content must be resolved against every Steam library, not only
    // the first manifest found. A stale/incomplete TF2 install can still have
    // tf/ while the complete hl2/ depot lives in another library.
    const fs::path staleSteam=temp/"a-stale-steam";
    const fs::path completeSteam=temp/"b-complete-steam";
    const fs::path staleTf=staleSteam/"steamapps/common/Team Fortress 2";
    const fs::path completeTf=completeSteam/"steamapps/common/Team Fortress 2";
    writeText(staleSteam/"steamapps/appmanifest_440.acf",
              "\"AppState\" { \"appid\" \"440\" \"installdir\" \"Team Fortress 2\" }");
    writeText(completeSteam/"steamapps/appmanifest_440.acf",
              "\"AppState\" { \"appid\" \"440\" \"installdir\" \"Team Fortress 2\" }");
    writeText(staleTf/"tf/materials/stale_only.txt", "stale");
    write(completeTf/"hl2/hl2_misc_dir.vpk", makeVpk());
    writeText(completeTf/"hl2/materials/hl2_loose.txt", "hl2-loose");
    writeText(sdkApp/"multi-library/gameinfo.txt",
        "GameInfo { DependsOnAppID 440 FileSystem { SteamAppId 243750 SearchPaths { "
        "game |appid_440|hl2/hl2_misc.vpk game |appid_440|hl2 "
        "game |gameinfo_path|. } } }");
    hammer::assets::SteamLibraries multiLibraries({staleSteam, completeSteam});
    const auto appCandidates = multiLibraries.resolveApps(440);
    assert(appCandidates.size() == 2);
    auto multiFs=std::make_shared<hammer::assets::GameFileSystem>();
    error.message.clear();
    assert(multiFs->configure(sdkApp/"multi-library/gameinfo.txt",&error,&multiLibraries));
    assert(error.message.empty());
    assert(multiFs->exists("materials/hello.txt"));
    assert(multiFs->exists("materials/hl2_loose.txt"));
    const auto completeHl2 = fs::weakly_canonical(completeTf/"hl2");
    assert(std::any_of(multiFs->locations().begin(), multiFs->locations().end(),
        [&](const hammer::assets::SearchLocation& item) {
            return item.path == completeHl2 || item.path == fs::weakly_canonical(completeTf/"hl2/hl2_misc_dir.vpk");
        }));

    // Missing search-path VPKs are nonfatal in Source. The game still loads
    // available paths and records a useful warning instead of showing a blank error.
    writeText(sdkApp/"partial/gameinfo.txt",
        "GameInfo { FileSystem { SteamAppId 243750 SearchPaths { "
        "game |appid_440|tf/missing.vpk game |gameinfo_path|. } } }");
    auto partialFs=std::make_shared<hammer::assets::GameFileSystem>();
    error.message.clear();
    assert(partialFs->configure(sdkApp/"partial/gameinfo.txt",&error,&sourceLibraries));
    assert(error.message.empty());
    assert(!partialFs->warnings().empty());

    // If AppID 440 is referenced but its explicit SearchPaths are absent or
    // incomplete, the compatibility pass must still discover TF2's paired
    // hl2_misc/hl2_textures archives and expose their materials.
    const fs::path fallbackSteam = temp / "fallback-steam";
    const fs::path fallbackTf = fallbackSteam / "steamapps/common/Team Fortress 2";
    const std::string brickVmt =
        "LightmappedGeneric { $basetexture brick/brickwall001a }";
    write(fallbackTf / "hl2/hl2_misc_dir.vpk",
          makeVpkFiles({{"materials/brick/brickwall001a.vmt",
                         std::vector<std::uint8_t>(brickVmt.begin(), brickVmt.end())}}));
    write(fallbackTf / "hl2/hl2_textures_dir.vpk",
          makeVpkFiles({{"materials/brick/brickwall001a.vtf", makeVtf()}}));
    writeText(fallbackSteam / "steamapps/libraryfolders.vdf",
              "\"libraryfolders\" { \"0\" { \"path\" \"" + fallbackSteam.string() + "\" } }");
    writeText(sdkApp / "fallback/gameinfo.txt",
        "GameInfo { DependsOnAppID 440 FileSystem { SteamAppId 243750 "
        "SearchPaths { game |gameinfo_path|. } } }");
    hammer::assets::SteamLibraries fallbackLibraries({fallbackSteam});
    auto fallbackFs = std::make_shared<hammer::assets::GameFileSystem>();
    error.message.clear();
    assert(fallbackFs->configure(sdkApp / "fallback/gameinfo.txt", &error, &fallbackLibraries));
    assert(error.message.empty());
    assert(fallbackFs->exists("materials/brick/brickwall001a.vmt"));
    assert(fallbackFs->exists("materials/brick/brickwall001a.vtf"));
    const auto indexedFiles = fallbackFs->listFiles("materials/", ".vmt");
    assert(std::find(indexedFiles.begin(), indexedFiles.end(),
                     "materials/brick/brickwall001a.vmt") != indexedFiles.end());
    hammer::assets::MaterialSystem fallbackMaterials(fallbackFs);
    const auto names = fallbackMaterials.materialNames();
    assert(std::find(names.begin(), names.end(), "brick/brickwall001a") != names.end());
    const auto brick = fallbackMaterials.material("brick/brickwall001a");
    assert(brick && !brick->missing && brick->image.valid());
    assert(!brick->vmtSource.empty() && !brick->vtfSource.empty());
    assert(brick->error.empty());
    const auto brickWithExtension = fallbackMaterials.material("materials/brick/brickwall001a.vmt");
    assert(brickWithExtension && !brickWithExtension->missing);
    const auto absent = fallbackMaterials.material("brick/definitely_missing");
    assert(absent && absent->missing && !absent->error.empty());


    // --- Detail objects (%detailtype / detail.vbsp / VBSP's emitter) --------
    {
        // A trimmed copy of TF2's detail.vbsp, keeping the structure that
        // matters: an empty low-alpha group, groups sorted by alpha, and both
        // sprite and model entries.
        const char* detailVbsp =
            "detail\n{\n"
            "tf_grass\n{\n"
            "  \"density\" \"8000\"\n"
            "  Group4\n  {\n    \"alpha\" \"1\"\n"
            "    Model1\n    {\n"
            "      \"sprite\" \"0 0 164 256 512\"\n"
            "      \"spritesize\" \"0.5 0.0 24 37\"\n"
            "      \"amount\" \"0.5\"\n"
            "      \"detailOrientation\" \"2\"\n"
            "    }\n"
            "    Model2\n    {\n"
            "      \"model\" \"models/props_foliage/grass_02_detailmodel.mdl\"\n"
            "      \"upright\" \"1\"\n"
            "      \"amount\" \"0.5\"\n"
            "    }\n  }\n"
            "  Group1\n  {\n    \"alpha\" \"0\"\n  }\n"
            "}\n}\n";
        const auto dictionaryDocument = hammer::vmf::Document::parse(detailVbsp);
        detailCheck(dictionaryDocument.has_value() && !dictionaryDocument->roots().empty());
        const auto dictionary =
            hammer::assets::parseDetailObjectDictionary(dictionaryDocument->roots().front());
        const hammer::assets::DetailObjectType* grass = dictionary.find("TF_Grass");
        detailCheck(grass != nullptr && grass->density == 8000.0f);
        // ParseDetailGroup keeps groups sorted by alpha regardless of file order.
        detailCheck(grass->groups.size() == 2);
        detailCheck(grass->groups[0].alpha == 0.0f && grass->groups[0].models.empty());
        detailCheck(grass->groups[1].alpha == 1.0f && grass->groups[1].models.size() == 2);
        const auto& sprite = grass->groups[1].models[0];
        detailCheck(sprite.type == hammer::assets::DetailPropType::Sprite);
        // "sprite" "0 0 164 256 512" -> half-texel-inset atlas rect.
        detailCheck(std::abs(sprite.texUpperLeft[0] - 0.5f / 512.0f) < 1e-6f);
        detailCheck(std::abs(sprite.texLowerRight[1] - 255.5f / 512.0f) < 1e-6f);
        // "spritesize" "0.5 0.0 24 37" -> 24x37 units, origin centred at the base.
        detailCheck(std::abs(sprite.positionUpperLeft[0] + 12.0f) < 1e-6f);
        detailCheck(std::abs(sprite.positionUpperLeft[1] - 37.0f) < 1e-6f);
        detailCheck(std::abs(sprite.positionLowerRight[0] - 12.0f) < 1e-6f);
        detailCheck(std::abs(sprite.positionLowerRight[1]) < 1e-6f);
        // Cumulative amounts, and the model entry.
        detailCheck(std::abs(sprite.amount - 0.5f) < 1e-6f);
        const auto& model = grass->groups[1].models[1];
        detailCheck(model.type == hammer::assets::DetailPropType::Model && model.upright);
        detailCheck(std::abs(model.amount - 1.0f) < 1e-6f);

        // A 1024x1024 floor. VBSP takes area * density * 0.000001 samples per
        // fan triangle - density is objects per million square units - so the
        // two 524288-unit triangles give 4194 each, truncated independently.
        const std::string floorVmf =
            "world\n{\n\"id\" \"1\"\n\"classname\" \"worldspawn\"\n"
            "\"detailmaterial\" \"detail/detailsprites\"\n"
            "\"detailvbsp\" \"detail.vbsp\"\n"
            "solid\n{\n\"id\" \"2\"\n"
            "side\n{\n\"id\" \"3\"\n\"plane\" \"(-512 -512 0) (-512 512 0) (512 512 0)\"\n\"material\" \"nature/grass\"\n\"uaxis\" \"[1 0 0 0] 0.25\"\n\"vaxis\" \"[0 -1 0 0] 0.25\"\n\"lightmapscale\" \"16\"\n}\n"
            "side\n{\n\"id\" \"4\"\n\"plane\" \"(-512 512 -32) (-512 -512 -32) (512 -512 -32)\"\n\"material\" \"tools/toolsnodraw\"\n\"uaxis\" \"[1 0 0 0] 0.25\"\n\"vaxis\" \"[0 -1 0 0] 0.25\"\n\"lightmapscale\" \"16\"\n}\n"
            "side\n{\n\"id\" \"5\"\n\"plane\" \"(-512 -512 0) (-512 -512 -32) (-512 512 -32)\"\n\"material\" \"tools/toolsnodraw\"\n\"uaxis\" \"[0 1 0 0] 0.25\"\n\"vaxis\" \"[0 0 -1 0] 0.25\"\n\"lightmapscale\" \"16\"\n}\n"
            "side\n{\n\"id\" \"6\"\n\"plane\" \"(512 512 0) (512 512 -32) (512 -512 -32)\"\n\"material\" \"tools/toolsnodraw\"\n\"uaxis\" \"[0 1 0 0] 0.25\"\n\"vaxis\" \"[0 0 -1 0] 0.25\"\n\"lightmapscale\" \"16\"\n}\n"
            "side\n{\n\"id\" \"7\"\n\"plane\" \"(512 -512 0) (512 -512 -32) (-512 -512 -32)\"\n\"material\" \"tools/toolsnodraw\"\n\"uaxis\" \"[1 0 0 0] 0.25\"\n\"vaxis\" \"[0 0 -1 0] 0.25\"\n\"lightmapscale\" \"16\"\n}\n"
            "side\n{\n\"id\" \"8\"\n\"plane\" \"(-512 512 0) (-512 512 -32) (512 512 -32)\"\n\"material\" \"tools/toolsnodraw\"\n\"uaxis\" \"[1 0 0 0] 0.25\"\n\"vaxis\" \"[0 0 -1 0] 0.25\"\n\"lightmapscale\" \"16\"\n}\n"
            "}\n}\n";
        const auto floorDocument = hammer::vmf::Document::parse(floorVmf);
        detailCheck(floorDocument.has_value());
        const hammer::vmf::Scene floorScene = hammer::vmf::buildScene(*floorDocument);
        detailCheck(floorScene.detailMaterial == "detail/detailsprites");
        detailCheck(floorScene.detailVbspName == "detail.vbsp");

        const auto detailTypeFor = [](std::string_view material) {
            return material == "nature/grass" ? std::string("tf_grass") : std::string();
        };
        const auto emission =
            hammer::assets::emitDetailProps(floorScene, dictionary, detailTypeFor);
        detailCheck(emission.overflowed == 0);
        detailCheck(emission.props.size() == 4194 * 2);
        for (const auto& prop : emission.props) {
            // Only the grass-textured top face emits, and every object lands on it.
            detailCheck(std::abs(prop.origin.z) < 1e-6);
            detailCheck(prop.origin.x >= -512.0 && prop.origin.x <= 512.0);
            detailCheck(prop.origin.y >= -512.0 && prop.origin.y <= 512.0);
        }
        // Placement is seeded off the Hammer face id, so it is reproducible.
        const auto again = hammer::assets::emitDetailProps(floorScene, dictionary, detailTypeFor);
        detailCheck(again.props.size() == emission.props.size());
        for (std::size_t i = 0; i < again.props.size(); ++i) {
            detailCheck(again.props[i].origin.x == emission.props[i].origin.x &&
                   again.props[i].origin.y == emission.props[i].origin.y);
        }
        // A material with no %detailtype scatters nothing.
        const auto none = hammer::assets::emitDetailProps(
            floorScene, dictionary, [](std::string_view) { return std::string(); });
        detailCheck(none.props.empty());

        // Displacements pick their group from the PAINTED alpha at the sample,
        // which is the direction that decides which blend layer grows grass.
        {
            const char* twoGroups =
                "detail\n{\n"
                "disp_grass\n{\n"
                "  \"density\" \"100\"\n"
                "  GroupLow\n  {\n    \"alpha\" \"0\"\n"
                "    ModelLow\n    {\n      \"model\" \"models/low.mdl\"\n      \"amount\" \"1\"\n    }\n  }\n"
                "  GroupHigh\n  {\n    \"alpha\" \"1\"\n"
                "    ModelHigh\n    {\n      \"model\" \"models/high.mdl\"\n      \"amount\" \"1\"\n    }\n  }\n"
                "}\n}\n";
            const auto twoGroupDocument = hammer::vmf::Document::parse(twoGroups);
            detailCheck(twoGroupDocument.has_value());
            const auto twoGroupDictionary =
                hammer::assets::parseDetailObjectDictionary(twoGroupDocument->roots().front());

            const auto dispScene = [&](const char* alphaRow) {
                std::string vmf =
                    "world\n{\n\"id\" \"1\"\n\"classname\" \"worldspawn\"\n"
                    "solid\n{\n\"id\" \"2\"\n"
                    "side\n{\n\"id\" \"3\"\n\"plane\" \"(-512 -512 0) (-512 512 0) (512 512 0)\"\n"
                    "\"material\" \"nature/grass\"\n\"uaxis\" \"[1 0 0 0] 0.25\"\n"
                    "\"vaxis\" \"[0 -1 0 0] 0.25\"\n\"lightmapscale\" \"16\"\n"
                    "dispinfo\n{\n\"power\" \"2\"\n\"startposition\" \"[-512 -512 0]\"\n"
                    "\"elevation\" \"0\"\n\"subdiv\" \"0\"\n"
                    "normals\n{\n";
                for (int row = 0; row < 5; ++row)
                    vmf += "\"row" + std::to_string(row) + "\" \"0 0 1  0 0 1  0 0 1  0 0 1  0 0 1\"\n";
                vmf += "}\ndistances\n{\n";
                for (int row = 0; row < 5; ++row)
                    vmf += "\"row" + std::to_string(row) + "\" \"0 0 0 0 0\"\n";
                vmf += "}\noffsets\n{\n";
                for (int row = 0; row < 5; ++row)
                    vmf += "\"row" + std::to_string(row) +
                           "\" \"0 0 0  0 0 0  0 0 0  0 0 0  0 0 0\"\n";
                vmf += "}\nalphas\n{\n";
                for (int row = 0; row < 5; ++row)
                    vmf += "\"row" + std::to_string(row) + "\" \"" + alphaRow + "\"\n";
                vmf += "}\n}\n}\n"
                    "side\n{\n\"id\" \"4\"\n\"plane\" \"(-512 512 -32) (-512 -512 -32) (512 -512 -32)\"\n\"material\" \"tools/toolsnodraw\"\n\"uaxis\" \"[1 0 0 0] 0.25\"\n\"vaxis\" \"[0 -1 0 0] 0.25\"\n\"lightmapscale\" \"16\"\n}\n"
                    "side\n{\n\"id\" \"5\"\n\"plane\" \"(-512 -512 0) (-512 -512 -32) (-512 512 -32)\"\n\"material\" \"tools/toolsnodraw\"\n\"uaxis\" \"[0 1 0 0] 0.25\"\n\"vaxis\" \"[0 0 -1 0] 0.25\"\n\"lightmapscale\" \"16\"\n}\n"
                    "side\n{\n\"id\" \"6\"\n\"plane\" \"(512 512 0) (512 512 -32) (512 -512 -32)\"\n\"material\" \"tools/toolsnodraw\"\n\"uaxis\" \"[0 1 0 0] 0.25\"\n\"vaxis\" \"[0 0 -1 0] 0.25\"\n\"lightmapscale\" \"16\"\n}\n"
                    "side\n{\n\"id\" \"7\"\n\"plane\" \"(512 -512 0) (512 -512 -32) (-512 -512 -32)\"\n\"material\" \"tools/toolsnodraw\"\n\"uaxis\" \"[1 0 0 0] 0.25\"\n\"vaxis\" \"[0 0 -1 0] 0.25\"\n\"lightmapscale\" \"16\"\n}\n"
                    "side\n{\n\"id\" \"8\"\n\"plane\" \"(-512 512 0) (-512 512 -32) (512 512 -32)\"\n\"material\" \"tools/toolsnodraw\"\n\"uaxis\" \"[1 0 0 0] 0.25\"\n\"vaxis\" \"[0 0 -1 0] 0.25\"\n\"lightmapscale\" \"16\"\n}\n"
                    "}\n}\n";
                return vmf;
            };

            // The reported bug: a displacement brush whose OTHER five sides
            // carry the same ground material must not grow anything, because
            // VBSP deletes the brush from the world and keeps only its
            // displaced side (map.cpp LoadMapBrush: b->numsides = 0).
            {
                std::string allSides = dispScene("255 255 255 255 255");
                std::string::size_type at = 0;
                while ((at = allSides.find("tools/toolsnodraw", at)) != std::string::npos) {
                    allSides.replace(at, std::strlen("tools/toolsnodraw"), "nature/grass");
                    at += std::strlen("nature/grass");
                }
                const auto coveredDocument = hammer::vmf::Document::parse(allSides);
                detailCheck(coveredDocument.has_value());
                const hammer::vmf::Scene coveredScene = hammer::vmf::buildScene(*coveredDocument);
                const auto coveredEmission = hammer::assets::emitDetailProps(
                    coveredScene, twoGroupDictionary,
                    [](std::string_view material) {
                        return material == "nature/grass" ? std::string("disp_grass")
                                                          : std::string();
                    });
                detailCheck(!coveredEmission.props.empty());
                for (const auto& prop : coveredEmission.props) {
                    // Everything is on the displaced top surface at z = 0;
                    // nothing hangs off the underside at z = -32 or the sides.
                    detailCheck(std::abs(prop.origin.z) < 1e-6);
                }
            }

            const auto painted = hammer::vmf::Document::parse(dispScene("255 255 255 255 255"));
            const auto unpainted = hammer::vmf::Document::parse(dispScene("0 0 0 0 0"));
            detailCheck(painted.has_value() && unpainted.has_value());
            const auto dispDetailTypeFor = [](std::string_view material) {
                return material == "nature/grass" ? std::string("disp_grass") : std::string();
            };
            const auto paintedEmission = hammer::assets::emitDetailProps(
                hammer::vmf::buildScene(*painted), twoGroupDictionary, dispDetailTypeFor);
            const auto unpaintedEmission = hammer::assets::emitDetailProps(
                hammer::vmf::buildScene(*unpainted), twoGroupDictionary, dispDetailTypeFor);
            detailCheck(!paintedEmission.props.empty());
            detailCheck(paintedEmission.props.size() == unpaintedEmission.props.size());
            for (const auto& prop : paintedEmission.props)
                detailCheck(prop.model == "models/high.mdl");
            for (const auto& prop : unpaintedEmission.props)
                detailCheck(prop.model == "models/low.mdl");
        }

        // The engine's distance fade: nothing past cl_detaildist, and a fade
        // band across the last cl_detailfade units that is linear in SQUARED
        // distance (CDetailObjectSystem::SortSpritesBackToFront).
        {
            const hammer::assets::DetailPropFade fade;
            detailCheck(fade.maxDistance == 1200.0f && fade.fadeWidth == 400.0f);
            const hammer::vmf::Vec3 eye{0.0, 0.0, 0.0};
            detailCheck(hammer::assets::detailPropAlpha({0.0, 400.0, 0.0}, eye, fade) == 1.0f);
            detailCheck(hammer::assets::detailPropAlpha({0.0, 800.0, 0.0}, eye, fade) == 1.0f);
            detailCheck(hammer::assets::detailPropAlpha({0.0, 1200.0, 0.0}, eye, fade) == 0.0f);
            detailCheck(hammer::assets::detailPropAlpha({0.0, 4000.0, 0.0}, eye, fade) == 0.0f);
            // 900 units: (1200^2 - 900^2) / (1200^2 - 800^2).
            const float banded =
                hammer::assets::detailPropAlpha({0.0, 900.0, 0.0}, eye, fade);
            detailCheck(std::abs(banded - 0.7875f) < 1e-4f);

            // An env_detail_controller can only pull the distances in.
            const char* controllerVmf =
                "world\n{\n\"id\" \"1\"\n\"classname\" \"worldspawn\"\n}\n"
                "entity\n{\n\"id\" \"9\"\n\"classname\" \"env_detail_controller\"\n"
                "\"fademindist\" \"100\"\n\"fademaxdist\" \"600\"\n\"origin\" \"0 0 0\"\n}\n";
            const auto controllerDocument = hammer::vmf::Document::parse(controllerVmf);
            detailCheck(controllerDocument.has_value());
            const auto narrowed = hammer::assets::detailPropFadeForScene(
                hammer::vmf::buildScene(*controllerDocument));
            detailCheck(narrowed.maxDistance == 600.0f && narrowed.fadeWidth == 100.0f);
            detailCheck(hammer::assets::detailPropAlpha({0.0, 700.0, 0.0}, eye, narrowed) == 0.0f);

            const char* wideVmf =
                "world\n{\n\"id\" \"1\"\n\"classname\" \"worldspawn\"\n}\n"
                "entity\n{\n\"id\" \"9\"\n\"classname\" \"env_detail_controller\"\n"
                "\"fademindist\" \"5000\"\n\"fademaxdist\" \"9000\"\n\"origin\" \"0 0 0\"\n}\n";
            const auto wideDocument = hammer::vmf::Document::parse(wideVmf);
            detailCheck(wideDocument.has_value());
            const auto clamped = hammer::assets::detailPropFadeForScene(
                hammer::vmf::buildScene(*wideDocument));
            detailCheck(clamped.maxDistance == 1200.0f && clamped.fadeWidth == 400.0f);
        }

        // An orientation-2 sprite always turns to face the viewer and stays
        // vertical, and its quad hangs off the emitted origin.
        hammer::assets::DetailPropInstance billboard;
        billboard.type = hammer::assets::DetailPropType::Sprite;
        billboard.orientation = 2;
        billboard.origin = {0.0, 0.0, 0.0};
        billboard.positionUpperLeft = {-12.0f, 37.0f};
        billboard.positionLowerRight = {12.0f, 0.0f};
        billboard.texUpperLeft = {0.0f, 0.0f};
        billboard.texLowerRight = {1.0f, 1.0f};
        billboard.scale = 1.0f;
        const auto quad = hammer::assets::detailSpriteQuad(billboard, {0.0, -256.0, 40.0});
        // Bottom edge sits on the surface, top edge 37 units up, 24 wide.
        detailCheck(std::abs(quad.corners[0].z - 37.0) < 1e-6);
        detailCheck(std::abs(quad.corners[1].z) < 1e-6);
        detailCheck(std::abs(quad.corners[3].z - 37.0) < 1e-6);
        const double width = std::hypot(quad.corners[3].x - quad.corners[0].x,
                                        quad.corners[3].y - quad.corners[0].y);
        detailCheck(std::abs(width - 24.0) < 1e-6);
        // Screen-aligned-vertical means the quad faces the viewer horizontally.
        detailCheck(std::abs(quad.normal.z) < 1e-6);
        detailCheck(quad.normal.y < -0.99);
    }

    fs::remove_all(temp);
    std::cout<<"hammer asset tests passed\n";
    return 0;
}
