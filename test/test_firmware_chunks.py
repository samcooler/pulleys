"""Exercise the bridge's actual chunk parser with loss, corruption and 4 KiB lines."""
import base64
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'src/bridge/main.cpp').read_text()
functions = source[source.index('// Ask the Pi for a chunk. Does not wait'):source.index('static void serveFirmware()')]
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
static bool fwRefused = false;
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
  if(mode==5 || mode==6){   // the Overlord refuses: unavailable, or cancelled for good
   auto line=std::string("FW-ERROR sha256=")+fwSha+" offset="+std::to_string(offset)+(mode==6 ? " reason=cancelled\n" : " reason=unavailable\n");
   for(char c:line) bytes.push_back(c);
   return;
  }
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
 // The Overlord says it cannot serve the chunk: stop waiting at once (no 5 s timeout), retry a few times, give up.
 pi=Port(); pi.mode=5; ticks=0; fwRefused=false; assert(!requestFirmwareChunk(12,data,3,got)); assert(pi.requests==3 && ticks<1000 && !fwRefused);
 // A cancel is final: one try, no retries, and nothing more is asked until the next transfer clears it.
 pi=Port(); pi.mode=6; ticks=0; fwRefused=false; assert(!requestFirmwareChunk(12,data,3,got)); assert(pi.requests==1 && ticks<1000 && fwRefused);
 assert(!requestFirmwareChunk(12,data,3,got)); assert(pi.requests==1);
 fwRefused=false;
 // An error for some other chunk is not ours: ignore it and keep waiting for our own answer.
 pi=Port(); pi.mode=0; ticks=0; { std::string stray=std::string("FW-ERROR sha256=")+fwSha+" offset=999 reason=cancelled\n"; for(char c:stray) pi.bytes.push_back(c); }
 assert(requestFirmwareChunk(12,data,3,got)); assert(!fwRefused);
 // Pipelining: the request goes out first and the answer is already waiting, whole, when it is read.
 pi=Port(); pi.mode=7; ticks=0; pi.mode=0; sendFirmwareChunkRequest(12,3);
 assert(pi.requests==1 && !pi.bytes.empty());
 { size_t n=0; uint8_t out[8]; assert(receiveFirmwareChunk(12,out,3,n) && n==3 && !std::memcmp(out,"abc",3)); }
 // The same through the pipe the bridge uses: image of 7 bytes in 3-byte chunks.
 pi=Port(); pi.mode=1; ticks=0;   // first request of the run is lost, so fetch() has to retry it
 { FirmwareLink link; uint8_t out[8]; size_t n=0;
   link.request(12,3); assert(pi.requests==1);        // lost
   assert(!link.receive(12,out,3));                   // nothing comes: times out, false
   assert(link.fetch(12,out,3) && !std::memcmp(out,"abc",3)); }
 puts("PASS stale/lost/corrupt chunks, bounded retries, 4 KiB framing, refusal and prefetched answers");
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


class SelfUpdateTests(unittest.TestCase):
    """The bridge's own flash update must never overlap a serial answer with a flash write."""

    def body(self, name):
        start = source.index(f'static void {name}(')
        return source[start:source.index('\nstatic void ', start + 1)]

    def test_self_update_is_never_pipelined_but_the_wifi_stream_is(self):
        # Update.write erases/writes flash with the UART interrupt blocked (CONFIG_UART_ISR_IN_IRAM is off),
        # so an answer arriving mid-write is lost: 0.6 KB/s, a 5 s timeout per chunk. Hardware found it, not the
        # host tests, so this guards the one argument that matters.
        self.assertRegex(self.body('updateBridgeSelf'), r'FW_CHUNK_BYTES,\s*size,\s*false\)')
        self.assertIn('Update.write', self.body('updateBridgeSelf'))
        self.assertRegex(self.body('serveFirmware'), r'FW_CHUNK_BYTES,\s*fwSize,\s*BRIDGE_PIPELINE\)')
