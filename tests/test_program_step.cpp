// Host-side unit tests for src/program_step.h. Build and run:
//   g++ -std=c++17 -Wall -Wextra -Werror -Isrc tests/test_program_step.cpp -o test_program_step && ./test_program_step
#include "program_step.h"
#include <cmath>
#include <cstdio>

static int failures=0;

#define CHECK(cond) do{ if(!(cond)){ std::printf("FAIL %s:%d: %s\n",__FILE__,__LINE__,#cond); failures++; } }while(0)

static Step_parse_result parse(const char *line, uint16_t out[3]){
  out[0]=out[1]=out[2]=0xBEEF;
  return parse_program_step(line,out);
}

static void test_valid_steps(){
  uint16_t s[3];
  CHECK(parse("500:120:30",s)==STEP_OK);
  CHECK(s[0]==500 && s[1]==120 && s[2]==30);
  CHECK(parse("0:0:0",s)==STEP_OK);
  CHECK(s[0]==0 && s[1]==0 && s[2]==0);
  CHECK(parse("1300:0:15",s)==STEP_OK);   // 0 minutes to reach is allowed (jump step)
  CHECK(s[1]==0);
  CHECK(parse("65535:65535:65535",s)==STEP_OK);
  CHECK(s[0]==65535 && s[1]==65535 && s[2]==65535);
  CHECK(parse("007:08:9",s)==STEP_OK);
  CHECK(s[0]==7 && s[1]==8 && s[2]==9);
}

static void test_bad_format(){
  uint16_t s[3];
  CHECK(parse("",s)==STEP_BAD_FORMAT);
  CHECK(parse("500",s)==STEP_BAD_FORMAT);
  CHECK(parse("500:30",s)==STEP_BAD_FORMAT);          // used to load as temp 0
  CHECK(parse("500:30:10:5",s)==STEP_BAD_FORMAT);     // used to write prg[255]
  CHECK(parse("500::10",s)==STEP_BAD_FORMAT);
  CHECK(parse(":30:10",s)==STEP_BAD_FORMAT);
  CHECK(parse("500:30:",s)==STEP_BAD_FORMAT);
  CHECK(parse("65536:1:1",s)==STEP_BAD_FORMAT);       // used to wrap to 0 and pass the MAX check
  CHECK(parse("1:99999999999:1",s)==STEP_BAD_FORMAT);
}

static void test_bad_chars(){
  uint16_t s[3];
  CHECK(parse("500:30:1a",s)==STEP_BAD_CHAR);
  CHECK(parse("500 :30:10",s)==STEP_BAD_CHAR);
  CHECK(parse("-5:30:10",s)==STEP_BAD_CHAR);
  CHECK(parse("500;30;10",s)==STEP_BAD_CHAR);
}

static void test_ramp(){
  CHECK(step_ramp_per_second(20,620,10)==1.0f);       // 600 C in 600 s
  CHECK(step_ramp_per_second(620,20,10)==-1.0f);
  CHECK(step_ramp_per_second(500,500,0)==0);          // used to be 0/0 = NaN
  CHECK(step_ramp_per_second(20,500,0)==0);           // used to be inf
  CHECK(std::isfinite(step_ramp_per_second(20,65535,1)));
}

int main(){
  test_valid_steps();
  test_bad_format();
  test_bad_chars();
  test_ramp();
  if(failures){ std::printf("%d check(s) failed\n",failures); return 1; }
  std::printf("all program_step tests passed\n");
  return 0;
}
