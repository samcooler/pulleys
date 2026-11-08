"""Exercise the bridge's actual chunk parser with loss, corruption and 4 KiB lines."""
import base64
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'src/bridge/main.cpp').read_text()
functions = source[source.index('static bool requestFirmwareChunkOnce'):source.index('static void serveFirmware()')]
prefix = r'''
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <deque>
#include <cstdint>
#include "pulleys_fw_serial.h"
static uint32_t ticks;
uint32_t millis(){return ticks++;}
void delay(int n){ticks+=n;}
void drainQueue(){}
namespace pulleys { void mesh_poll(){} }
static char fwSha[65] = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
static char fwChunkLine[pulleys::FW_LINE_BYTES];
static const char* largeBody = LARGE_BODY;
struct Port {
 std::deque<char> bytes; int requests=0, mode=0;
 void enqueue(unsigned long offset, bool corrupt=false){
  const bool large=mode==4;
  std::string raw = large ? std::string(4096,'x') : "abc";
  char crc[9]; std::snprintf(crc,sizeof(crc),"%08x",pulleys::fw_chunk_crc32((const uint8_t*)raw.data(),raw.size()) ^ (corrupt ? 1u : 0u));
  auto line=std::string("FW-DATA sha256=")+fwSha+" offset="+std::to_string(offset)+" crc32="+crc+" b64="+(large ? largeBody : "YWJj")+"\n";
  for(char c:line) bytes.push_back(c);
 }
 void printf(const char* format,...){
  if(std::strncmp(format,"FW-READ ",8)) return;
  va_list args; va_start(args,format); (void)va_arg(args,const char*);
  auto offset=va_arg(args,unsigned long); va_end(args); ++requests;
  if(mode==2 || (mode==1 && requests==1)) return;
  if(mode==0) enqueue(offset-3);
  enqueue(offset,mode==3 && requests==1);
 }
 int available(){return bytes.size();}
 char read(){char c=bytes.front(); bytes.pop_front(); return c;}
} pi, Serial;
#define PI_LINK pi
int mbedtls_base64_decode(uint8_t* out,size_t cap,size_t* n,const unsigned char* in,size_t size){
 if(size==std::strlen(largeBody) && !std::memcmp(in,largeBody,size) && cap>=4096){std::memset(out,'x',4096);*n=4096;return 0;}
 if(cap<3 || size!=4 || std::memcmp(in,"YWJj",4)) return -1;
 std::memcpy(out,"abc",3); *n=3; return 0;
}
'''.replace('LARGE_BODY', json.dumps(base64.b64encode(b'x' * 4096).decode()))
suffix = r'''
int main(){
 assert(pulleys::fw_chunk_crc32((const uint8_t*)"123456789",9)==0xcbf43926u);
 uint8_t data[4096]; size_t got;
 pi.mode=0; assert(requestFirmwareChunk(12,data,3,got)); assert(got==3 && !std::memcmp(data,"abc",3)); assert(pi.requests==1);
 pi=Port(); pi.mode=1; ticks=0; assert(requestFirmwareChunk(12,data,3,got)); assert(pi.requests==2);
 pi=Port(); pi.mode=2; ticks=0; assert(!requestFirmwareChunk(12,data,3,got)); assert(pi.requests==3 && ticks<15100);
 pi=Port(); pi.mode=3; ticks=0; assert(requestFirmwareChunk(12,data,3,got)); assert(pi.requests==2);
 pi=Port(); pi.mode=4; ticks=0; assert(requestFirmwareChunk(0,data,4096,got)); assert(got==4096 && data[4095]=='x'); assert(pi.requests==1);
 puts("PASS stale/lost/corrupt chunks, bounded retries and 4 KiB framing");
}
'''


class ChunkTests(unittest.TestCase):
    def test_stale_lost_corrupt_and_large_responses(self):
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / 'chunks.cpp'
            binary_path = Path(directory) / 'chunks'
            source_path.write_text(prefix + functions + suffix)
            subprocess.run(['clang++', '-std=c++11', '-I', str(ROOT / 'lib/pulleys_fw_serial'), str(source_path), '-o', str(binary_path)], check=True)
            subprocess.run([str(binary_path)], check=True, capture_output=True)
