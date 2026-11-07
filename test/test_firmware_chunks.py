"""Exercise the bridge chunk parser with deterministic serial loss and delays."""
from pathlib import Path
import subprocess
import tempfile
import unittest
source=(Path(__file__).resolve().parents[1] / 'src/bridge/main.cpp').read_text()
functions=source[source.index('static bool requestFirmwareChunkOnce'):source.index('static void serveFirmware()')]
prefix=r'''
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <deque>
#include <cstdint>
static uint32_t ticks;
uint32_t millis(){return ticks++;}
void delay(int n){ticks+=n;}
void drainQueue(){}
namespace pulleys { void mesh_poll(){} }
static char fwSha[65] = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
struct Port {
 std::deque<char> bytes; int requests=0, mode=0;
 void enqueue(unsigned long offset){
  auto line=std::string("FW-DATA sha256=")+fwSha+" offset="+std::to_string(offset)+" b64=YWJj\n";
  for(char c:line) bytes.push_back(c);
 }
 void printf(const char* format,...){
  if(std::strncmp(format,"FW-READ ",8)) return;
  va_list args; va_start(args,format); (void)va_arg(args,const char*);
  auto offset=va_arg(args,unsigned long); va_end(args); ++requests;
  if(mode==2 || (mode==1 && requests==1)) return;
  if(mode==0) enqueue(offset-3);
  enqueue(offset);
 }
 int available(){return bytes.size();}
 char read(){char c=bytes.front(); bytes.pop_front(); return c;}
} pi, Serial;
#define PI_LINK pi
int mbedtls_base64_decode(uint8_t* out,size_t cap,size_t* n,const unsigned char* in,size_t size){
 if(cap<3 || size!=4 || std::memcmp(in,"YWJj",4)) return -1;
 std::memcpy(out,"abc",3); *n=3; return 0;
}
'''
suffix=r'''
int main(){
 uint8_t data[3]; size_t got;
 pi.mode=0; assert(requestFirmwareChunk(12,data,3,got)); assert(got==3 && !std::memcmp(data,"abc",3)); assert(pi.requests==1);
 pi=Port(); pi.mode=1; ticks=0; assert(requestFirmwareChunk(12,data,3,got)); assert(pi.requests==2);
 pi=Port(); pi.mode=2; ticks=0; assert(!requestFirmwareChunk(12,data,3,got)); assert(pi.requests==3 && ticks<15100);
 puts("PASS stale response ignored, lost chunk retried, timeout bounded");
}
'''
class ChunkTests(unittest.TestCase):
    def test_stale_lost_and_missing_responses(self):
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "chunks.cpp"
            binary_path = Path(directory) / "chunks"
            source_path.write_text(prefix + functions + suffix)
            subprocess.run(["clang++", "-std=c++11", str(source_path), "-o", str(binary_path)], check=True)
            subprocess.run([str(binary_path)], check=True, capture_output=True)


if __name__ == "__main__":
    unittest.main()
