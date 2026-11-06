#include "pak_replay.h"
#include <libdeflate.h>
#include <filesystem>
#include <fstream>
#include <vector>
#include <thread>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <windows.h>
using namespace G3O;
void check(bool value) { if (!value) throw std::runtime_error("check failed"); }
int main() {
 auto dir=std::filesystem::current_path()/ ("fixture_"+std::to_string(GetCurrentProcessId()));
 std::filesystem::create_directories(dir);
 try {
  std::vector<char> plain(100000, 'x'), packed(110000), output(100000, '?');
  auto c=libdeflate_alloc_compressor(1); check(c!=nullptr);
  size_t n=libdeflate_zlib_compress(c,plain.data(),plain.size(),packed.data(),packed.size());
  libdeflate_free_compressor(c); check(n>0); packed.resize(n);
  {std::ofstream f(dir/"a.pak",std::ios::binary);f.write(packed.data(),packed.size());}
  {std::ofstream f(dir/"trace.csv");f<<"seq,pak,offset,in_bytes,out_bytes,decomp_us,load_time_ms\n1,a.pak,0x0,"<<n<<",100000,1,1\n";}
  PakReplay cache;
  cache.Start((dir/"trace.csv").string(),dir.string());
  for(int i=0;i<1000 && cache.Snapshot().ready==0;i++) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  check(cache.Snapshot().ready==1);
  size_t written=0;
  check(cache.TryCopy("a.pak",0,packed.data(),packed.size(),output.data(),output.size(),written));
  check(written==plain.size() && output==plain);
  auto stats=cache.Stop();check(stats.hits==1);
  std::filesystem::remove_all(dir);
  std::cout<<"PASS: independent archive read, exact hit, clean stop\n";
 } catch(...) {std::filesystem::remove_all(dir);throw;}
}
