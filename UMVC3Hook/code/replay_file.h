#pragma once
// Local-only codec for the experimental TSARES2 format. All integers are LE.
// The 92-byte configuration is the packed WireConfig from spectate.cpp.
#include <array>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace replay {
namespace fs = std::filesystem;
constexpr size_t kHeader = 140, kConfig = 92, kStride = 8;
constexpr size_t kMaxFrames = 65536;
constexpr char kMagic[8] = {'T','S','A','R','E','S','2','\0'};
using Names = std::array<std::array<char, 64>, 2>;
struct Game {
    uint32_t id = 0, seed = 0, openingP1 = 0, openingP2 = 0;
    uint64_t stamp = 0;
    std::array<uint8_t,kConfig> config{};
    std::vector<uint32_t> inputs;
    Names names{};
    size_t frames() const { return inputs.size()/2; }
};
inline uint32_t get32(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1])<<8 | uint32_t(p[2])<<16 | uint32_t(p[3])<<24;
}
inline void put32(uint8_t* p, uint32_t v) {
    for (unsigned i=0;i<4;++i) p[i]=uint8_t(v>>(8*i));
}
inline bool validConfig(const std::array<uint8_t,kConfig>& c) {
    for (size_t i=0;i<6;++i) {
        const uint32_t ch=get32(c.data()+4*i), color=get32(c.data()+24+4*i),
                       assist=get32(c.data()+48+4*i);
        if (ch<1 || ch>50 || color>8 || assist>2) return false;
    }
    for(size_t i=72;i<82;++i) if(c[i]>1) return false;
    const uint32_t stage=get32(c.data()+82), timer=get32(c.data()+86);
    constexpr uint32_t stages[]={0,2,3,4,5,6,7,9,10,11,100,102,103,104,105,106,107,109,110};
    return std::find(std::begin(stages),std::end(stages),stage)!=std::end(stages) && timer>=1 && timer<=99 && c[90]<=2 && c[91]<=1;
}
inline bool valid(const Game& g) {
    return g.id!=0 && g.openingP1<=2 && g.openingP2<=2 && !g.inputs.empty() &&
           !(g.inputs.size()%2) && g.frames()<=kMaxFrames && validConfig(g.config);
}
inline fs::path sidecar(fs::path p) { p += ".names"; return p; }
inline void readNames(const fs::path& p, Names& out) {
    out={}; std::ifstream f(sidecar(p),std::ios::binary); Names candidate{};
    if (!f.read(candidate[0].data(),64) || !f.read(candidate[1].data(),64) ||
        f.peek()!=std::char_traits<char>::eof() || candidate[0][63] || candidate[1][63]) return;
    out=candidate;
}
inline bool read(const fs::path& p, Game& out, std::string& error) {
    std::error_code ec; auto n=fs::file_size(p,ec);
    if(ec || n<kHeader+kStride || n>kHeader+kMaxFrames*kStride || (n-kHeader)%kStride) {
        error="Invalid replay length"; return false;
    }
    std::ifstream f(p,std::ios::binary); std::array<uint8_t,kHeader> h{};
    if(!f.read(reinterpret_cast<char*>(h.data()),h.size()) ||
       std::memcmp(h.data(),kMagic,8)!=0 || get32(h.data()+8)!=2 ||
       get32(h.data()+12)!=kHeader || get32(h.data()+40)!=kStride || get32(h.data()+44)!=0) {
        error="Unsupported or truncated TSARES2 header"; return false;
    }
    Game g; g.id=get32(h.data()+16); g.seed=get32(h.data()+20);
    g.openingP1=get32(h.data()+24); g.openingP2=get32(h.data()+28);
    g.stamp=uint64_t(get32(h.data()+32)) | uint64_t(get32(h.data()+36))<<32;
    std::copy_n(h.data()+48,kConfig,g.config.data());
    g.inputs.resize(size_t(n-kHeader)/4);
    std::array<uint8_t,4> v{};
    for(auto& input:g.inputs) {
        if(!f.read(reinterpret_cast<char*>(v.data()),v.size())) { error="Truncated inputs"; return false; }
        input=get32(v.data());
    }
    if(f.peek()!=std::char_traits<char>::eof() || !valid(g)) { error="Invalid replay data"; return false; }
    readNames(p,g.names); out=std::move(g); error.clear(); return true;
}
inline bool write(const fs::path& p, const Game& g, std::string& error) {
    if(!valid(g)) { error="Invalid replay data"; return false; }
    std::error_code ec;
    if(fs::exists(p,ec) || ec) { error="Replay destination unavailable"; return false; }
    auto temp=p; temp += ".part";
    if(fs::exists(temp,ec) || ec) { error="Temporary destination unavailable"; return false; }
    std::array<uint8_t,kHeader> h{};
    std::copy_n(reinterpret_cast<const uint8_t*>(kMagic),8,h.data());
    put32(h.data()+8,2); put32(h.data()+12,kHeader); put32(h.data()+16,g.id);
    put32(h.data()+20,g.seed); put32(h.data()+24,g.openingP1); put32(h.data()+28,g.openingP2);
    put32(h.data()+32,uint32_t(g.stamp)); put32(h.data()+36,uint32_t(g.stamp>>32));
    put32(h.data()+40,kStride); std::copy(g.config.begin(),g.config.end(),h.begin()+48);
    std::ofstream f(temp,std::ios::binary|std::ios::trunc);
    f.write(reinterpret_cast<const char*>(h.data()),h.size());
    std::array<uint8_t,4> v{};
    for(uint32_t input:g.inputs) { put32(v.data(),input); f.write(reinterpret_cast<const char*>(v.data()),v.size()); }
    f.close();
    if(!f) { fs::remove(temp,ec); error="Replay write failed"; return false; }
    fs::rename(temp,p,ec);
    if(ec) { fs::remove(temp,ec); error="Replay commit failed"; return false; }
    error.clear();
    if(g.names[0][0] || g.names[1][0]) {
        auto names=g.names; names[0][63]=names[1][63]=0;
        std::ofstream nf(sidecar(p),std::ios::binary|std::ios::trunc);
        nf.write(names[0].data(),64); nf.write(names[1].data(),64); nf.close();
        if(!nf) { fs::remove(sidecar(p),ec); error="Replay saved; name metadata write failed"; }
    }
    return true;
}
} // namespace replay
