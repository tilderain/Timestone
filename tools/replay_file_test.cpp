#include "../UMVC3Hook/code/replay_file.h"
#include <cassert>
#include <iostream>
#include <iterator>
int main(int argc,char** argv) {
    assert(argc==4);
    const replay::fs::path temp=argv[3];
    std::error_code ec; replay::fs::create_directories(temp,ec); assert(!ec);
    const size_t expected[]{6571,6066};
    for(int i=1;i<=2;++i) {
        replay::Game g,r; std::string err;
        assert(replay::read(argv[i],g,err)); assert(g.frames()==expected[i-1]);
        assert(g.names[0][0]==0 && g.names[1][0]==0);
        auto out=temp/(std::to_string(i)+".tsr");
        assert(replay::write(out,g,err)); assert(replay::read(out,r,err));
        assert(g.config==r.config && g.inputs==r.inputs && g.id==r.id && g.stamp==r.stamp);
        std::ifstream a(argv[i],std::ios::binary), b(out,std::ios::binary);
        std::vector<char> aa((std::istreambuf_iterator<char>(a)),{}),bb((std::istreambuf_iterator<char>(b)),{});
        assert(aa==bb); assert(!replay::write(out,g,err));
        auto named=temp/(std::to_string(i)+"-named.tsr");
        std::strcpy(g.names[0].data(),"Test P1"); std::strcpy(g.names[1].data(),"Test P2");
        assert(replay::write(named,g,err)); assert(replay::read(named,r,err)); assert(r.names==g.names);
        auto bad=temp/(std::to_string(i)+"-bad.tsr"); aa[0]='X';
        std::ofstream f(bad,std::ios::binary); f.write(aa.data(),aa.size());f.close(); assert(!replay::read(bad,r,err));
        replay::fs::resize_file(bad,141); assert(!replay::read(bad,r,err));
        aa[0]='T'; aa[24]=3;
        auto invalid=[&](){std::ofstream z(bad,std::ios::binary|std::ios::trunc);z.write(aa.data(),aa.size());z.close();assert(!replay::read(bad,r,err));};
        invalid();aa[24]=0;aa[48]=0;invalid();aa[48]=25;aa[40]=4;invalid();
        std::cout<<"PASS fixture "<<i<<": "<<g.frames()<<" frames, byte-identical roundtrip, names, malformed input and overwrite checks\n";
    }
}
