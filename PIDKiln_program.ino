/*
** Pidkiln program routines - main program functions and all PID magic
**
*/
#include <PID_v1.h>
#include <SPI.h>
#include "src/program_step.h"   // step parser and ramp math - unit tested on the host (tests/)

// Other variables
//
hw_timer_t *timer = NULL;
volatile SemaphoreHandle_t timerSemaphore;


/*
** Core/main program functions
**
*/

// Setup timer function
//
void IRAM_ATTR onTimer(){
  // Increment the counter and set the time of ISR
  // Give a semaphore that we can check in the loop
  xSemaphoreGiveFromISR(timerSemaphore, NULL);
  // It is safe to use digitalRead/Write here if you want to toggle an output
}


// Validate one program step "temp:minutes_to_reach:minutes_dwell" and append it to Program[].
// Returns 0 on success, otherwise a PROGRAM_ERROR_STATE value + 1.
//
uint8_t add_program_line(String& linia){
uint16_t prg[3];

  DBG dbgLog(LOG_DEBUG,"[PRG] Sanitizing line: '%s'\n",linia.c_str());
  switch(parse_program_step(linia.c_str(),prg)){
    case STEP_OK: break;
    case STEP_BAD_CHAR: return PR_ERR_BAD_CHAR+1;
    default: return PR_ERR_BAD_FORMAT+1;
  }
  if(prg[0]>Prefs[PRF_MAX_TEMP].value.uint16) return PR_ERR_TOO_HOT+1;
  if(Program_size>=MAX_PRG_LENGTH) return PR_ERR_TOO_MANY_STEPS+1;
  Program[Program_size].temp=prg[0];
  Program[Program_size].togo=prg[1];
  Program[Program_size].dwell=prg[2];
  DBG dbgLog(LOG_DEBUG,"[PRG] Program_pos: %d, Temp: %dC Time to: %ds Dwell: %ds\n",Program_size,Program[Program_size].temp,Program[Program_size].togo,Program[Program_size].dwell);
  Program_size++;
  return 0;
}


// Load program from file to memory - return 0 if success
//
uint8_t Load_program(char *file){
Recursive_lock fs_lock(FS_mutex);
String line;
char file_path[32];
uint8_t err=0;
int pos=0;
int sel;
File prg;

  if(file){  // if function got an argument - this can happen if you want to validate new program uploaded by http
    snprintf(file_path,sizeof(file_path),"%s/%s",PRG_Directory,file);
    DBG dbgLog(LOG_DEBUG,"[PRG] Got pointer to load:'%s'\n",file);
    Program_name=String(file);
  }else{
    if((sel=Find_selected_program())<0) return Cleanup_program(1);
    snprintf(file_path,sizeof(file_path),"%s/%s",PRG_Directory,Programs_DIR[sel].filename);
    Program_name=String(Programs_DIR[sel].filename);
  }
  DBG dbgLog(LOG_INFO,"[PRG] Load program name: '%s'\n",file_path);
  
  if(prg = LittleFS.open(file_path,"r")){
    Program_desc="";  // erase current program
    Program_size=0;
    while(prg.available()){
      line=prg.readStringUntil('\n');
      line.trim();
      if(!line.length()) continue; // empty line - skip it
      
      DBG dbgLog(LOG_DEBUG,"[PRG] Raw line: '%s'\n",line.c_str());
      if(line.startsWith("#")){ // skip every comment line
        DBG dbgLog(LOG_DEBUG,"[PRG]  comment");
        if(!Program_desc.length()){
          line=line.substring(1);  // If it's the first line with comment - copy it without trailing #
          line.trim();
          Program_desc=line;
        }
      }else{
        // Sanitize every line - if it's have a comment - strip ip, then check if this are only numbers and ":" - otherwise fail
        if((pos=line.indexOf("#"))>0){
          line=line.substring(0,pos);
          line.trim(); // trim again after removing comment
        }
        
        if(line.length()>15) return Cleanup_program(2);  // program line too long
        else if(err=add_program_line(line)) return Cleanup_program(err); // line adding failed!!
        
        DBG dbgLog(LOG_DEBUG,"[PRG] San line: '%s'\n",line.c_str());
      }
    }
    if(!Program_size) return Cleanup_program(PR_ERR_BAD_FORMAT+1);  // comments only - nothing to run
    DBG dbgLog(LOG_DEBUG,"[PRG] Found description: %s\n",Program_desc.c_str());
    if(!Program_desc.length()) Program_desc="No description";   // if after reading file program still has no description - add it
    
    return 0;
  }return Cleanup_program(1);
}


// Load programs directory into memory - to sort it etc for easier processing
//
uint8_t Load_programs_dir(){
Recursive_lock fs_lock(FS_mutex);
uint16_t count=0;
File dir,file;

  dir = LittleFS.open(PRG_Directory);
  if(!dir) return 1;  // directory open failed
  DBG dbgLog(LOG_INFO,"[PRG] Loading directory...\n");
  while(dir.openNextFile()) count++;  // not the prettiest - but we count files first to do proper malloc without fragmenting memory
  DBG dbgLog(LOG_DEBUG,"[PRG]\tcounted %d files\n",count);
  if(Programs_DIR){
    free(Programs_DIR);
    Programs_DIR=NULL;
  }
  Programs_DIR=(DIRECTORY*)MALLOC(sizeof(DIRECTORY)*count);
  Programs_DIR_size=0;
  dir.rewindDirectory();
  while((file=dir.openNextFile()) && Programs_DIR_size<count){    // now we do actual loading into memory
    const char *fname=file.name();
    DBG dbgLog(LOG_DEBUG,"[PRG] Processing filename: %s\n",fname);
    if(file.isDirectory() || !strcmp(fname,"index.html")) continue;   // skip subdirectories and the index file
    if(strlen(fname)>MAX_PROGNAME){   // could only get here by a data image upload - the web UI rejects such names
      DBG dbgLog(LOG_ERR,"[PRG] Skipping program with a name longer than %d chars: %s\n",MAX_PROGNAME,fname);
      continue;
    }
    strlcpy(Programs_DIR[Programs_DIR_size].filename,fname,sizeof(Programs_DIR[Programs_DIR_size].filename));
    Programs_DIR[Programs_DIR_size].filesize=file.size();
    Programs_DIR[Programs_DIR_size].sel=0;
    
    DBG dbgLog(LOG_DEBUG,"[PRG] FName: %s\t FSize:%d\tSel:%d\n",Programs_DIR[Programs_DIR_size].filename,Programs_DIR[Programs_DIR_size].filesize,Programs_DIR[Programs_DIR_size].sel);
    
    Programs_DIR_size++;
  }
  dir.close();

  // sorting... slooow but easy
  bool nok=true;
  while(nok){
    nok=false;
    if(Programs_DIR_size>1) // if we have at least 2 progs
      for(int a=0; a<Programs_DIR_size-1; a++){
        DIRECTORY tmp;
        if(strcmp(Programs_DIR[a].filename,Programs_DIR[a+1].filename)>0){
          tmp=Programs_DIR[a];
          Programs_DIR[a]=Programs_DIR[a+1];
          Programs_DIR[a+1]=tmp;
          nok=true;
        }
      }    
   }

  if(Programs_DIR_size) Programs_DIR[0].sel=1; // make first program selected if we have at least one
  return 0;
}


// Edit program step
//
void Update_program_step(uint8_t sstep, uint16_t stemp, uint16_t stime, uint16_t sdwell){
Recursive_lock state_lock(State_mutex);
  if(Program_run_size<=sstep)
    if(Program_run_size==sstep){ // we are out of the program - but this is just NEXT step, we can add
      Program_run_size++;
      Program_run=(PROGRAM *)REALLOC(Program_run,sizeof(PROGRAM)*Program_run_size);
    }else return;   // we are out of the program - we can edit it

  Program_run[sstep].temp=stemp;
  Program_run[sstep].togo=stime;
  Program_run[sstep].dwell=sdwell;
}


// Initizalize program - clear memory
//
void Initialize_program_to_run(){
Recursive_lock state_lock(State_mutex);
  if(Program_run!=NULL){
    free(Program_run);
    Program_run=NULL;
  }
  if(Program_run_desc!=NULL){
    free(Program_run_desc);
    Program_run_desc=NULL;
  }
  if(Program_run_name!=NULL){
    free(Program_run_name);
    Program_run_name=NULL;
  }
  Program_run_size=0;
  DBG dbgLog(LOG_INFO,"[PRG] Initialized new in-memory program\n");
}


// True while a program controls the heater: running, paused, or waiting for the temperature
// threshold. Use this instead of comparing states by hand - missing PR_THRESHOLD is an easy bug.
//
boolean Program_is_active(){
  return Program_run_state==PR_RUNNING || Program_run_state==PR_PAUSED || Program_run_state==PR_THRESHOLD;
}


// Copy selected/loaded program to RUN program memory. Refused (returns false) while a program is
// active: replacing it would stop the PID loop and leave the SSR in its last state.
//
boolean Load_program_to_run(){
Recursive_lock state_lock(State_mutex);   // State before FS - see PIDKiln.h
Recursive_lock fs_lock(FS_mutex);

  if(Program_is_active()){
    DBG dbgLog(LOG_ERR,"[PRG] Refusing to replace the program - a program is active\n");
    return false;
  }
  Initialize_program_to_run();
  Program_run=(PROGRAM *)MALLOC(sizeof(PROGRAM)*Program_size);
  for(uint8_t a=0;a<Program_size;a++) Program_run[a]=Program[a];
  
  Program_run_desc=(char *)MALLOC((Program_desc.length()+1)*sizeof(char));
  strcpy(Program_run_desc,Program_desc.c_str());
  Program_run_name=(char *)MALLOC((Program_name.length()+1)*sizeof(char));
  strcpy(Program_run_name,Program_name.c_str());
  Program_run_size=Program_size;
  Program_run_state=PR_READY;
  return true;
}


/*
** Helping functions
**
*/

// Find selected program
//
int Find_selected_program(){
Recursive_lock fs_lock(FS_mutex);
  for(uint16_t a=0; a<Programs_DIR_size; a++) if(Programs_DIR[a].sel>0) return a;
  return -1;  // in case there is NO selected program or there can be no programs at all
}


// Move program selection up/down
//
void rotate_selected_program(int dir){
Recursive_lock fs_lock(FS_mutex);
int a = Find_selected_program();

  if(a<0) return;   // no programs
  DBG dbgLog(LOG_INFO,"[PRG] Rotating programs. For a:%d, dir: %d, selected?:%d, dir_size:%d\n",a,dir,Programs_DIR[a].sel,Programs_DIR_size);
  if(dir<0 && a>0){   // if we are DOWN down and we can a>0 - do it, if we can't - do nothing
    Programs_DIR[a].sel=0;  // delete old selection
    Programs_DIR[a-1].sel=1;
  }else if(dir>0 && a<(Programs_DIR_size-1)){   // if we are going UP and there is more programs - do it, if it's the end of program list, do nothing
    Programs_DIR[a].sel=0;  // delete old selection
    Programs_DIR[a+1].sel=1;
  }
}


// Short function to cleanup program def. after failed load
//
byte Cleanup_program(byte err){
Recursive_lock fs_lock(FS_mutex);
  Program_size=0;
  Program_desc="";
  Program_name="";
  for(byte a=0;a<MAX_PRG_LENGTH;a++) Program[a].temp=Program[a].togo=Program[a].dwell=0;
  DBG dbgLog(LOG_INFO,"[PRG] Cleaning up program with error %d\n",err);
  return err;
}


// Erase program from disk
//
boolean Erase_program_file(){
Recursive_lock fs_lock(FS_mutex);
char file[32];
int sel=Find_selected_program();

  if(sel<0) return false;
  snprintf(file,sizeof(file),"%s/%s",PRG_Directory,Programs_DIR[sel].filename);
  DBG dbgLog(LOG_INFO,"[PRG] !!! Erasing file from disk: %s",file);
  return LittleFS.remove(file);
}


// Cleanly end program
//
void END_Program(){
Recursive_lock state_lock(State_mutex);

  DBG dbgLog(LOG_INFO,"[PRG] Ending program cleanly\n");
  Program_run_state=PR_ENDED;
  KilnPID.SetMode(MANUAL);
  Disable_SSR();
  Disable_EMR();
  Program_run_end=time(NULL);
  Close_log_file();
  set_temp=0;
  pid_out=0;
  Program_run_step=-1;
}


// Abort program if something goes wrong (safety check, sensor error) or the user aborts.
// Must work in every active state - including PR_THRESHOLD, which it used to ignore.
//
void ABORT_Program(uint8_t error){
Recursive_lock state_lock(State_mutex);

  if(Program_is_active()){
    Program_error=error;
    DBG dbgLog(LOG_INFO,"[PRG] Aborting program with error: %d\n",Program_error);
    END_Program();
    Program_run_state=PR_ABORTED;
    //Program_run_start=0;
    START_Alarm();
  }
}


// Pause program. We are keeping current temperature and wait until it's resumed
//
void PAUSE_Program(){
Recursive_lock state_lock(State_mutex);
  if(Program_run_state==PR_RUNNING) Program_run_state=PR_PAUSED;
}


// Resume pasued program.
//
void RESUME_Program(){
Recursive_lock state_lock(State_mutex);
  if(Program_run_state==PR_PAUSED) Program_run_state=PR_RUNNING;
}


// Recalculate ETA based on current step
//
void Program_recalculate_ETA(boolean dwell=false){
Recursive_lock state_lock(State_mutex);

  Program_run_end=time(NULL);
  for(uint16_t a=Program_run_step; a<Program_run_size;a++){
    if(!dwell) Program_run_end+=Program_run[a].togo*60;
    else dwell=false;   // If we start counting with dwell=true - next step shoud start as a normal step, so dwell=fasle;
    Program_run_end+=Program_run[a].dwell*60;
  }
}


// This is functional called every second - if program is running. It calculates temperature grow, sets targets etc.
//
void Program_calculate_steps(boolean prg_start=false){
Recursive_lock state_lock(State_mutex);
static time_t step_start,next_step_end;
static uint16_t cnt1=1,cnt2=1;
static boolean is_it_dwell=false;

  if(prg_start){  // starting program - do some startup stuff
    Program_run_step=-1; cnt1=0;
    next_step_end=0;         // there was no previous step - so it has ended NOW
    cnt2=999;                // make sure we will parse first step
    is_it_dwell=false;        // last step was dwell
  }

  // Logging stuff - cnt1 - counter for this. If Prefs[PRF_LOG_WINDOW].value.uint16 == 0 - no logging
  cnt1++;
  if(Prefs[PRF_LOG_WINDOW].value.uint16 && cnt1>Prefs[PRF_LOG_WINDOW].value.uint16){
    cnt1=1;
    Add_log_line();
  }
    
  // Program recalc stuff - cnt2 - counter for it
  cnt2++;
  if(cnt2>10){
    cnt2=1;

    if(Program_run_state==PR_PAUSED){   // if we are paused - calculate new ETA end exit
      Program_recalculate_ETA(false);
      next_step_end+=10;                // this is not precise but should be enough
      return;
    }

    if(Prefs[PRF_PID_TEMP_THRESHOLD].value.int16>-1 && set_temp){       // check if we are in threshold window and there is temperature set already - if not, pause
      //DBG Serial.printf("[PRG] Temperature in TEMP_THRESHOLD. Kiln_temp:%.1f Set_temp:%.1f Window:%d\n",kiln_temp,set_temp,Prefs[PRF_PID_TEMP_THRESHOLD].value.int16);
      if(kiln_temp+Prefs[PRF_PID_TEMP_THRESHOLD].value.int16 < set_temp || kiln_temp-Prefs[PRF_PID_TEMP_THRESHOLD].value.int16 > set_temp){   // set_temp must be between kiln_temp +/- temp_threshold 
        DBG dbgLog(LOG_INFO,"[PRG] Temperature in TEMP_THRESHOLD. Kiln_temp:%.1f Set_temp:%.1f Window:%d\n",kiln_temp,set_temp,(int)Prefs[PRF_PID_TEMP_THRESHOLD].value.int16);
        //PAUSE_Program();
        Program_run_state=PR_THRESHOLD;
        Program_recalculate_ETA(false);
        next_step_end+=10; 
        return;
      }else if(Program_run_state==PR_THRESHOLD) Program_run_state=PR_RUNNING;     
    }

    // calculate next step
    if(time(NULL)>next_step_end){
      DBG dbgLog(LOG_DEBUG,"[DBG] Calculating new step!\n");
      if(!is_it_dwell){  // we have finished full step togo+dwell (or this is first step)
        Program_run_step++; // lets icrement step
        
        if(Program_run_step>=Program_run_size){  //we have passed the last step - end program
          END_Program();
          return;
        }
        DBG dbgLog(LOG_DEBUG,"[PRG] Calculating new NORMAL step!\n");
        is_it_dwell=true;   // next step will be dwell
        step_start=time(NULL);
        next_step_end=step_start+Program_run[Program_run_step].togo*60;
        DBG dbgLog(LOG_DEBUG,"[PRG] Next step:%d Start step:%d Togo:%d Run_step:%d/%d Set_temp:%.3f\n",next_step_end,step_start,Program_run[Program_run_step].togo,Program_run_step,Program_run_size,set_temp);
        
        if(!Program_run[Program_run_step].togo){  // 0 minutes to reach: jump the set point to the target
          temp_incr=0;                             //  (dividing by togo would give inf, or NaN that sticks in the PID)
          set_temp=Program_run[Program_run_step].temp;
        }else if(Program_run_step>0){ // is this a next step?
          temp_incr=step_ramp_per_second(Program_run[Program_run_step-1].temp,Program_run[Program_run_step].temp,Program_run[Program_run_step].togo);
          DBG dbgLog(LOG_DEBUG,"[PRG] temp_inc:%f Step_temp:%d Step-1_temp:%d\n",temp_incr,Program_run[Program_run_step].temp,Program_run[Program_run_step-1].temp);
        }else{      // or this is teh first one?
          DBG dbgLog(LOG_DEBUG,"[DBG] First step.\n");
          set_temp=kiln_temp;
          temp_incr=step_ramp_per_second(kiln_temp,Program_run[Program_run_step].temp,Program_run[Program_run_step].togo);
        }
        Program_recalculate_ETA(false);   // recalculate ETA for normal step
      }else{
        DBG dbgLog(LOG_DEBUG,"[DBG] Calculating new DWELL step!");
        is_it_dwell=false;      // next step will be normal
        temp_incr=0;
        step_start=time(NULL);
        next_step_end=step_start+Program_run[Program_run_step].dwell*60;
        set_temp=Program_run[Program_run_step].temp;
        DBG dbgLog(LOG_DEBUG,"[PRG] Next step:%d Start step:%d Togo:%d Run_step:%d/%d Set_temp:%f\n",next_step_end,step_start,Program_run[Program_run_step].dwell,Program_run_step,Program_run_size,set_temp);
        Program_recalculate_ETA(true);  // recalculate ETA for dwell
      }
    }
    DBG dbgLog(LOG_INFO,"[PRG] Curr temp: %.0f, Set_temp: %.0f, Incr: %.2f Pid_out:%.2f Step: %d\n", kiln_temp, set_temp, temp_incr, pid_out*PID_WINDOW_DIVIDER, Program_run_step);
  }

  if(Program_run_state!=PR_PAUSED && Program_run_state!=PR_THRESHOLD) set_temp+=temp_incr;  // increase desire temperature...
}


// Start running the in memory program
//
void START_Program(){
Recursive_lock state_lock(State_mutex);

  // Start only a loaded program that isn't active (ready, ended, aborted). Starting an active one
  // used to restart it from step 0; a paused program is resumed with RESUME_Program() instead.
  if(Program_run_state==PR_NONE || Program_is_active()) return;
  
  DBG dbgLog(LOG_INFO,"[PRG] Starting new program!");
  Program_run_state=PR_RUNNING;
  Program_start_temp=kiln_temp;
  Energy_Usage=0;
  Program_error=0;
  TempA_errors=TempB_errors=0;  // Reset temperature errors
  
  Enable_EMR();

  KilnPID.SetTunings(Prefs[PRF_PID_KP].value.vfloat,Prefs[PRF_PID_KI].value.vfloat,Prefs[PRF_PID_KD].value.vfloat,Prefs[PRF_PID_POE].value.uint8); // set actual PID parameters
  Program_run_start=time(NULL);
  Program_calculate_steps(true);
  windowStartTime=millis();

  //tell the PID to range between 0 and the full window size/PID_WINDOW_DIVIDER. It's divided by 100 (default value of PID_WINDOW_DIVIDER) to have minimal window size faster above 1/25s - so SSR will be able to switch (zero switch)
  KilnPID.SetOutputLimits(0, Prefs[PRF_PID_WINDOW].value.uint16/PID_WINDOW_DIVIDER);
  KilnPID.SetMode(AUTOMATIC);

  DBG dbgLog(LOG_INFO,"[PRG] Trying to start log - window size:%d\n",Prefs[PRF_LOG_WINDOW].value.uint16);
  if(Prefs[PRF_LOG_WINDOW].value.uint16){ // if we should create log file
    DBG dbgLog(LOG_INFO,"[PRG] Trying to create logs");
    Init_log_file();
    Add_log_line();
  }
}


// Check the temperature limits - called every second, with or without a program.
// With a program active, a failed check aborts it. Without one the heater should already be off, so
// over-temperature means a stuck relay or a heat source PIDKiln doesn't control: switch both relays
// off (again), raise the alarm and log - once, until the temperatures are back in range.
//
void SAFETY_Check(){
static boolean idle_alarm=false;
uint8_t err=PR_ERR_end;   // = no error

  if(kiln_temp>Prefs[PRF_MAX_TEMP].value.uint16) err=PR_ERR_TOO_HOT;
  else if(case_temp>Prefs[PRF_MAX_HOUSING_TEMP].value.uint16) err=PR_ERR_TOO_HOT_HOUSING;
  else if(Program_is_active() && kiln_temp<Prefs[PRF_MIN_TEMP].value.int16) err=PR_ERR_TOO_COLD;

  if(err==PR_ERR_end){
    idle_alarm=false;
    return;
  }
  if(Program_is_active()){
    DBG dbgLog(LOG_ERR,"[PRG] Safety check failed - error %d (kiln %.1fC, housing %.1fC, limits %d..%dC, housing %dC)\n",err,kiln_temp,case_temp,(int)Prefs[PRF_MIN_TEMP].value.int16,Prefs[PRF_MAX_TEMP].value.uint16,Prefs[PRF_MAX_HOUSING_TEMP].value.uint16);
    ABORT_Program(err);
  }else if(!idle_alarm){
    idle_alarm=true;
    DBG dbgLog(LOG_ERR,"[PRG] Safety check failed with no program running - error %d (kiln %.1fC, housing %.1fC). Relays off, alarm on\n",err,kiln_temp,case_temp);
    Disable_SSR();
    Disable_EMR();
    START_Alarm();
  }
}


// Heater checks - called every second, right after the kiln temperature readout.
// - Heating stall (Thermal_Runaway minutes, 0 = off): while a program is active and the set point is
//   HEATING_STALL_DELTA or more above the kiln, the kiln must gain HEATING_STALL_DELTA within the window.
//   Catches a dead element or relay, or a thermocouple that is no longer in the kiln.
// - Stuck SSR: once the heater has been used, the kiln must not warm up by SSR_STUCK_DELTA above its
//   lowest temperature since the SSR was last commanded on. With a program active this aborts, which
//   opens the EMR. Without one, only the alarm and the log can tell anyone.
// Uses millis(), not time(): an NTP sync during a firing would make the wall clock jump.
//
void Heater_safety_check(){
static uint32_t stall_start=0;
static double stall_ref=0, off_low=0;
static boolean stall_running=false, heater_used=false, off_tracking=false, stuck_reported=false;
uint32_t now=millis();
uint16_t stall_minutes=Prefs[PRF_THERMAL_RUN].value.uint16;

  // Heating stall
  if(Program_is_active() && stall_minutes && set_temp>=kiln_temp+HEATING_STALL_DELTA){
    if(!stall_running || kiln_temp>=stall_ref+HEATING_STALL_DELTA){   // start a new window - or the kiln warms up as it should
      stall_running=true;
      stall_start=now;
      stall_ref=kiln_temp;
    }else if(now-stall_start>=(uint32_t)stall_minutes*60000){
      DBG dbgLog(LOG_ERR,"[PRG] Safety check failed - kiln did not gain %.0fC in %d minutes (%.1fC -> %.1fC, set point %.1fC)\n",HEATING_STALL_DELTA,stall_minutes,stall_ref,kiln_temp,set_temp);
      stall_running=false;
      ABORT_Program(PR_ERR_HEATING_STALL);
    }
  }else stall_running=false;

  // Stuck SSR. pid_out is clamped to 0 at the bottom, and then the SSR stays off for the whole window
  if(Program_is_active() && pid_out>0){
    heater_used=true;
    off_tracking=false;
    stuck_reported=false;
  }else if(heater_used){
    if(!off_tracking || kiln_temp<off_low){   // lowest temperature since the SSR was last commanded on
      off_low=kiln_temp;
      off_tracking=true;
    }
    if(!stuck_reported && kiln_temp>=off_low+SSR_STUCK_DELTA){
      stuck_reported=true;
      DBG dbgLog(LOG_ERR,"[PRG] Safety check failed - kiln warmed from %.1fC to %.1fC with the SSR switched off. Stuck SSR?\n",off_low,kiln_temp);
      if(Program_is_active()) ABORT_Program(PR_ERR_SSR_STUCK);
      else{
        Disable_SSR();
        Disable_EMR();
        START_Alarm();
      }
    }
  }
}


// Function that create task to handle program running
//
void Program_Loop(void * parameter){
static uint16_t cnt1=0;
uint32_t now;

 esp_task_wdt_add(NULL);   // this task drives the SSR - restart the ESP32 if it stops running (see setup())
 for(;;){
    esp_task_wdt_reset();

    now = millis();
 
    // Interrupts triggered ones per second
    // 
    if (xSemaphoreTake(timerSemaphore, 0) == pdTRUE){
      Recursive_lock state_lock(State_mutex);   // nothing may load, start or abort a program in the middle of this

      // Update temperature readout
      Update_TemperatureA();
      SAFETY_Check();
      Heater_safety_check();

      // Restart requested by a finished firmware update (web handlers must not block or restart)
      if(shouldReboot && !Program_is_active()) Restart_ESP();

      // Check if there is Alarm ON - if so, lower time and call STOP
      if(ALARM_countdown>0){
        if(ALARM_countdown<=1) STOP_Alarm();
        ALARM_countdown--;
      }

      // Do slow stuff every 10th second - cnt1=[0..9]
      //
      if(cnt1>9) cnt1=0;
      else cnt1++;
#ifdef MAXCS2
      if(cnt1==3){  // just to make it in other time then next if cnt1
        Update_TemperatureB();      // this does not have to be updated so often as kiln temp
      }
#endif
      // Energy is read by Power_Loop only - reading it here too counted it twice

      // Main view refresh - Input_Loop draws it (it owns the display), and only if it is still shown
      LCD_refresh_v1=true;

      if(Program_is_active()){
        // Do all the program recalc
        Program_calculate_steps();

        if(cnt1==9){
          cnt1=0;
          LCD_refresh_v2=true;
          DBG dbgLog(LOG_INFO,"[PRG] Pid_out RAW:%.2f Pid_out:%.2f Now-window:%d WindowSize:%d Prg_state:%d\n",pid_out,pid_out*PID_WINDOW_DIVIDER,(now - windowStartTime), Prefs[PRF_PID_WINDOW].value.uint16, (byte)Program_run_state);
        }
       }
    }

    {
      // Under the lock: otherwise an abort from another task could switch the SSR off between our
      // state check and Enable_SSR(), and the SSR would stay on with no program to switch it off
      Recursive_lock state_lock(State_mutex);

      // Fail safe on NaN/inf: PID_v1 never recovers from a NaN, and comparisons with it are false
      if(Program_is_active() && !(isfinite(kiln_temp) && isfinite(set_temp) && isfinite(pid_out))){
        DBG dbgLog(LOG_ERR,"[PRG] Invalid value - kiln:%f set:%f pid_out:%f. Aborting\n",kiln_temp,set_temp,pid_out);
        ABORT_Program(PR_ERR_BAD_VALUE);
      }

      // Do the PID stuff
      if(Program_is_active()){
        KilnPID.Compute();

        if (now - windowStartTime > Prefs[PRF_PID_WINDOW].value.uint16){ //time to shift the Relay Window
          windowStartTime += Prefs[PRF_PID_WINDOW].value.uint16;
        }
        if (pid_out*PID_WINDOW_DIVIDER > now - windowStartTime) Enable_SSR();
        else Disable_SSR();
      }
    }
    //yield();
    vTaskDelay(10); // This should enable to run other tasks on this core
  }
}


/*
** Main setup function for programs module
*/
void Program_Setup(){
 
  // Start interupt timer handler - 1s
  // Create semaphore to inform us when the timer has fired
  timerSemaphore = xSemaphoreCreateBinary();

  // has now only 1 parameter (frequency). There is an automatic calculation of the divider using different clock sources to achieve the selected frequency.
  timer = timerBegin(1000000);

  // Attach onTimer function to our timer.
  timerAttachInterrupt(timer, &onTimer);

  // Set alarm to call onTimer function every second (value in microseconds).
  // Repeat the alarm (third parameter)
  timerAlarm(timer, 1000000, true, 0);


// For testing!!!
//  Load_program("test_up_down.txt");
//  Load_program_to_run();

  xTaskCreatePinnedToCore(
//  xTaskCreate(
              Program_Loop,    /* Task function. */
              "Program_loop",  /* String with name of task. */
              8192,            /* Stack size in bytes. */
              NULL,            /* Parameter passed as input of the task */
              1,               /* Priority of the task. */
              NULL,0);         /* Task handle. */
                    
}
