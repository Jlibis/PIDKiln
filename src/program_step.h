/*
** Program step parsing and ramp math - plain C++ with no Arduino dependencies, so it can be unit
** tested on a PC (tests/test_program_step.cpp, run by CI). The firmware includes it from
** PIDKiln_program.ino.
*/
#pragma once
#include <stdint.h>

enum Step_parse_result : uint8_t {
  STEP_OK=0,
  STEP_BAD_CHAR,     // something other than digits and ':'
  STEP_BAD_FORMAT,   // not exactly three fields, an empty field, or a value above 65535
};

// Parse one program step "temp:minutes_to_reach:minutes_dwell" (already trimmed, comment removed).
// On STEP_OK, out[] holds temp, togo, dwell. out[] is undefined otherwise.
inline Step_parse_result parse_program_step(const char *line, uint16_t out[3]){
  uint32_t val[3]={0,0,0};
  uint8_t field=0;
  bool digits=false;

  for(const char *c=line; *c; c++){
    if(*c==':'){
      if(!digits || ++field>2) return STEP_BAD_FORMAT;   // empty field or more than three fields
      digits=false;
    }else if(*c>='0' && *c<='9'){
      val[field]=val[field]*10+(uint32_t)(*c-'0');
      if(val[field]>UINT16_MAX) return STEP_BAD_FORMAT;  // would wrap in the uint16_t step fields
      digits=true;
    }else return STEP_BAD_CHAR;
  }
  if(field!=2 || !digits) return STEP_BAD_FORMAT;       // fewer than three fields, or the last one is empty
  for(uint8_t a=0; a<3; a++) out[a]=(uint16_t)val[a];
  return STEP_OK;
}

// Set point change per second while ramping from from_temp to to_temp in togo_minutes.
// 0 minutes means "jump": the caller sets the set point to to_temp directly, so this returns 0
// instead of dividing by zero.
inline float step_ramp_per_second(double from_temp, uint16_t to_temp, uint16_t togo_minutes){
  if(!togo_minutes) return 0;
  return (float)((to_temp-from_temp)/(togo_minutes*60.0));
}
