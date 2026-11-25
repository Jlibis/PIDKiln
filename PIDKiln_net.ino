/*
** Network related functions
**
*/


// Other variables
//



// Set some default time - if there is no NTP
//
void Setup_start_date(){
struct timeval tv;
struct tm mytm={};   // zeroed - tm_isdst and the other fields used to be stack garbage
int y=2000,mo=1,d=1,h=0,mi=0,s=0;   // used for whatever the prefs don't give

  // Initial_Date "YYYY-MM-DD" and Initial_Time "HH:MM:SS" (separators . - : are all accepted)
  if(Prefs[PRF_INIT_DATE].value.str) sscanf(Prefs[PRF_INIT_DATE].value.str,"%d%*[.:-]%d%*[.:-]%d",&y,&mo,&d);
  if(Prefs[PRF_INIT_TIME].value.str) sscanf(Prefs[PRF_INIT_TIME].value.str,"%d%*[.:-]%d%*[.:-]%d",&h,&mi,&s);
  DBG dbgLog(LOG_INFO,"[NET] Initial date and time: %04d-%02d-%02d %02d:%02d:%02d\n",y,mo,d,h,mi,s);

  mytm.tm_year = y-1900;  // year after 1900
  mytm.tm_mon = mo-1;     // 0-11
  mytm.tm_mday = d;
  mytm.tm_hour = h;
  mytm.tm_min = mi;
  mytm.tm_sec = s;
  mytm.tm_isdst = -1;     // let mktime decide

  time_t t = mktime(&mytm);

  tv.tv_sec = t;
  tv.tv_usec = 0;

  settimeofday(&tv, NULL);
}


// Returns in lip current IP depending on WiFi mode
//
void Return_Current_IP(IPAddress &lip){
  if(WiFi.getMode()==WIFI_STA){
    lip=WiFi.localIP();
  }else if(WiFi.getMode()==WIFI_AP){
    lip=WiFi.softAPIP();
  }
}


// Turns off WiFi
//
void Disable_WiFi(){
  WiFi.disconnect(true);
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
}


// Starts WiFi in SoftAP mode
//
boolean Start_WiFi_AP(){
  if(!Prefs[PRF_WIFI_MODE].value.uint8) return 1;   // if WiFi disabled

  WiFi.mode(WIFI_AP);
  
  IPAddress local_IP(192,168,10,1);
  IPAddress gateway(192,168,10,1);
  IPAddress subnet(255,255,255,0);

  WiFi.softAPConfig(local_IP, gateway, subnet);
  
  // Name and password come from the preferences now (they were hard-coded). WPA2 needs 8-63 characters.
  const char *ap_name=Prefs[PRF_WIFI_AP_NAME].value.str;
  const char *ap_pass=Prefs[PRF_WIFI_AP_PASS].value.str;
  if(!ap_name || !strlen(ap_name)) ap_name=DEFAULT_AP_NAME;
  if(!ap_pass || strlen(ap_pass)<8 || strlen(ap_pass)>63){
    DBG dbgLog(LOG_ERR,"[NET] WiFi_AP_Pass must have 8-63 characters - using the default password\n");
    ap_pass=DEFAULT_PASSWORD;
  }
  DBG dbgLog(LOG_INFO,"[NET] Creating WiFi Access Point (AP) %s\n",ap_name);
  WiFi.softAP(ap_name, ap_pass, 8);
  return 0;
}


// Tries to connect to WiFi AP
//
boolean Start_WiFi_CLIENT(){

  if(!Prefs[PRF_WIFI_MODE].value.uint8) return 1;   // if WiFi disabled
  if(strlen(Prefs[PRF_WIFI_SSID].value.str)<1 || strlen(Prefs[PRF_WIFI_PASS].value.str)<1) return 1;  // missing SSI or password

  WiFi.mode(WIFI_STA);
   
  WiFi.begin(Prefs[PRF_WIFI_SSID].value.str, Prefs[PRF_WIFI_PASS].value.str);
  DBG dbgLog(LOG_INFO,"[NET] Connecting to WiFi as Client...\n");
    
  for(byte a=0; !Prefs[PRF_WIFI_RETRY_CNT].value.uint8 || a<Prefs[PRF_WIFI_RETRY_CNT].value.uint8; a++){  // if PRF_WIFI_RETRY_CNT - try indefinitely
    delay(1500);
    if (WiFi.status() == WL_CONNECTED) return 0;
    DBG dbgLog(LOG_INFO,"[NET] Connecting to AP WiFi... %d/%d\n",a+1,Prefs[PRF_WIFI_RETRY_CNT].value.uint8);
  }

  DBG dbgLog(LOG_INFO,"[NET] Connecting to AP WiFi failed!\n");
  Disable_WiFi();
  return 1;
}


// Starts WiFi and all networks services
//
boolean Setup_WiFi(){
struct tm timeinfo;

  Setup_start_date();

  if(Prefs[PRF_WIFI_MODE].value.uint8){ // we have WiFi enabled in config - start it
    int err=0;

    if(Prefs[PRF_WIFI_MODE].value.uint8==1 || Prefs[PRF_WIFI_MODE].value.uint8==2){ // 1 - tries as client if failed, be AP; 2 - just try as client
      err=Start_WiFi_CLIENT();
      if(!err){
        configTime(Prefs[PRF_GMT_OFFSET].value.int32, Prefs[PRF_DAYLIGHT_OFFSET].value.int16, Prefs[PRF_NTPSERVER1].value.str, Prefs[PRF_NTPSERVER2].value.str, Prefs[PRF_NTPSERVER3].value.str); // configure RTC clock with NTP server - or at least try
        SETUP_WebServer(); // Setup function for Webserver from PIDKiln_http.ino
        initSysLog();
        return 0;    // all is ok - connected
      }else if(Prefs[PRF_WIFI_MODE].value.uint8==2) return err;  // not connected and won't try be AP
    }
    // Now we try to be AP - if previous failed or not configured

    err=Start_WiFi_AP();
    
    if(!err){ // WiFi enabled in client mode (connected to AP - perhaps with Internet - try it)
      SETUP_WebServer();
      return 0;
    }return err;
  }else{
    Disable_WiFi();
    return 1; // WiFi disabled
  }
}


// Perform WiFi restart - usually when something does not work right - reinitialize it (perhaps preferences has changed)
//
boolean Restart_WiFi(){
  STOP_WebServer();
  delay(100);
  Disable_WiFi();
  delay(500);
  return Setup_WiFi();
}
