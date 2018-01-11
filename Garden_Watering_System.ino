/*
TO DO:
The system currently runs much faster than it will in the final version. This needs to be changed

*/


#include <EtherCard.h>
#include <stdlib.h>
#include <stdio.h>

#define UNDETERMINED 9999
#define LONG_MAX 4294967295 
#define KELVIN 273.15

#define MIN 60
#define HOUR 3600
#define DAY 86400

#define SOIL_DRY 0
#define SOIL_MED 1 //medium wetness, can be watered or not watered
#define SOIL_WET 2




//////STRUCTS
/** Represents rainfall during a two hour period of time 
 * 
 * Variables:
 * unix -- start of period
 * rain -- amount of rainfall in mm
 */
typedef struct rainPeriod{
  long int unix;
  float rain;

}rainPeriod_t;

/** Represents a resistive soil moisture sensor
 *  
 *  Variables:
 *  pin -- the id of the analog in used by the resistrive sensor
 *  recentReadings -- the last two readings made with the sensor
 *  reading -- the most recent average of two reliable readings
 *  stateChanges -- the positions marking inbetween (too dry average) and (average too wet)
 */
typedef struct soilSens{
  byte pin; // analog pin
  int recentReadings[2];
  int reading;
  int stateChanges[2];
  struct soilSens* next;
}soilSens_t;

/** used to execute a function at a single unix time
 *
 * Variables:
 * execUnix -- unix time to execute at or asap after
 * func* -- function to exectute
 * next* -- the next single run task in linked list ordered by exec_unix
 */
typedef struct singleTask {
  long int execUnix;
  void (*func)();
  struct singleTask* next;
} singleTask_t;

/** used to execute a function periodically
 *
 * Variables:
 * execUnix -- first unix time the function should fire
 * execPeriod -- the period in seconds between fires
 * func* -- function to exectute
 */
typedef struct schedTask {
  long int execUnix;
  long int execPeriod;
  void (*func)();
} schedTask_t;


//////PINS
const byte sensorPowPin = 2; //control transistor powering soil moisture sensors and ultrasonic sensor
const byte solenoid1Pin = 3; //control relay powering solenoid for garden section 1
const byte solenoid2Pin = 4; //control relay powering solenoid for garden section 2
const byte motorPin = 5;     //motor used in motorized ball valve
const byte switchPowPin = 6; //power to switch sensor
const byte switchPin = 7;    //state of switch sensor (just switched to LOW = open , just switched to HIGH = closed)
const byte sonarTrigPin = 8; //generate sonar
const byte sonarEchoPin = 9; //recieve sonar


//////USER VARIABLES
////task managment
const byte schedTaskAmnt = 5; //amount of scheduled tasks
boolean schedTasksEnabled = false;
////ethernet
const int tryTimePeriod = 15;//wait time in between attempts to connect to a site
static byte mymac[] = { 0x74, 0x69, 0x69, 0x2B, 0x30, 0x31 };
static byte myip[] = { 192, 168, 2, 22 }; //site address
static byte gwip[] = { 192, 168, 2, 1 }; //router address
////weather
const byte lightRainPoint = 10; //rain needed to be considered light rain (mm)
const byte rainPoint = 100; //rain needed to be considered a full rain (mm)
const byte weatherPeriods = 17; //amount of 3 hour weather periods to keep in memory
////barrel
const byte barrelHeight = 150;//distance from bottom of barrel to sensor (cm)
const float barrelFillSpeed = 2; // speed barrels refill (cm/s)
const int barrelMinFill = 5; // min time to open barrel valve (s)
const float barrelHeightSpeedup = 0.01; //used to decrease water time when there is more pressure in barrels (s/cm)


//////VARIABLES
////task managment
singleTask_t* singleTaskStart = NULL; // linked list of single run tasks
schedTask_t schedTasks[schedTaskAmnt]; //array of scheduled tasks
////ethernet
byte Ethernet::buffer[567]; // holds information from last packet recieved
BufferFiller bfill;
////weather
const char owmUrl[] PROGMEM = "api.openweathermap.org";
const byte owmIp[] = { 162,243,53,59 }; 
rainPeriod_t weatherData[weatherPeriods];
char weatherPeriod = -1; //used to keep track of index when open weather map sends multi-packet responce
long int frostWarning = 0; 
const String ownBufferWipe = "_ _ _ _ _ _ _ _ _ _ _ ";
String owmBuffer = ownBufferWipe;
////time
const char cutUrl[] PROGMEM = "www.convert-unix-time.com";
byte cutIp[] = { 198,211,122,7 };
long int startUnix = 0; //holds time system booted up
unsigned long int refMill = 0; //stores system millis at last time unix time updated
long int refUnix = 0; //holds unix time sent by convert unix time
long int lastLoopUnix = -1; //unix at last main loop
long int loopUnix = 0; //unix at current main loop
////barrel
int barrelRecentReadings[2] = {UNDETERMINED,UNDETERMINED};
int barrelReading = UNDETERMINED;
////soil moisture sensors
soilSens_t* soil1Sec1;
soilSens_t* soil2Sec1;
soilSens_t* soil1Sec2;
soilSens_t* soil2Sec2;

//////FUNCTIONS
////Task managment
/** adds a new singleTask to the singleTask linked list at the appropriate spot based on executing time
 *
 * keyword arguments:
 * execUnix -- time to exec at or asap after
 * func* -- function to execute
 */
void addSingleTask(long int execUnix, void (*func)()) {
  Serial.println(F("addSingleTask"));

  //add to front
  if (singleTaskStart == NULL || singleTaskStart->execUnix > execUnix) {
    singleTask_t* oldStart = singleTaskStart;

    singleTaskStart = (singleTask_t*) malloc(sizeof(singleTask_t));
    singleTaskStart->execUnix = execUnix;
    singleTaskStart->func = func;
    singleTaskStart->next = oldStart;
    Serial.print("added to front");
    
  //adding to middle or end
  } else {
    //find new position in list
    singleTask_t* prevTask = singleTaskStart;
    singleTask_t* nextTask = prevTask->next;
    Serial.print("finding add pos 1");
    while ( nextTask != NULL) {
      if (nextTask->execUnix >= execUnix) {
        break;
      }
      Serial.print("finding add pos");
      prevTask =  nextTask;
      nextTask = nextTask->next;
    }

    //create new task
    singleTask_t* newTask = (singleTask_t*) malloc(sizeof(singleTask_t));
    newTask->execUnix = execUnix;
    newTask->func = func;

    //fix links
    newTask->next = nextTask; // add after task next
    prevTask->next = newTask; // set new task to prev task next
  }
}

/* check if any single-run tasks should fire and be removed
 * 
 * newUnix -- current time
 */
void checkSingleTasks(long int newUnix) {
  singleTask_t* tst = singleTaskStart;
  Serial.println(F("SINGLE TASKS"));
  while (tst != NULL) {
    Serial.print("sch:");
    Serial.print(tst->execUnix - loopUnix);
    Serial.print(",");
    tst=tst->next;
  }
  Serial.println(" ");

  while (singleTaskStart != NULL) {
    //not time for any more tasks
    if (singleTaskStart->execUnix > newUnix) {
      break;
    }
    //execute and remove task
    singleTask_t* oldStart = singleTaskStart;
    singleTaskStart = singleTaskStart->next;
    oldStart->func();
    free(oldStart);
    Serial.println(F("exec single task"));
  }
}

/* check if any scheduled tasks should fire
 * 
 * newUnix -- current time
 * oldUnix -- time last check
 */
void checkSchedTasks(long int newUnix, long int oldUnix){
   Serial.println(F("checkSchedTasks"));
  if(newUnix > oldUnix && schedTasksEnabled){
    Serial.println(F("SCHED TASKS"));
    for (int i = 0; i < schedTaskAmnt; i++){
      Serial.print(">");
      Serial.print(i);
      Serial.print("-");
      Serial.println( schedTasks[i].execPeriod  - ((newUnix - schedTasks[i].execUnix) % schedTasks[i].execPeriod ));
      
      if( ((newUnix - schedTasks[i].execUnix) % schedTasks[i].execPeriod  
          < (oldUnix - schedTasks[i].execUnix) % schedTasks[i].execPeriod)
          && newUnix > schedTasks[i].execUnix ){
        schedTasks[i].func();      
      }
    }
  }
}

void enableSchedTasks(){
  schedTasksEnabled = true;
}


////ethernet
static word homePage() {
  /* return position of 
   * 
   * 
   */
  long int runSecs = loopUnix - startUnix;
  int curD =  (runSecs)/DAY; 
  byte curH =  (runSecs % DAY)/HOUR;  ///(loop_time + 21*  HOUR) % 24*HOUR))/3600;
  byte curM = (runSecs % HOUR )/MIN;
  byte curS = runSecs % MIN;

  bfill = ether.tcpOffset();
  bfill.emit_p(PSTR(
                 "HTTP/1.0 200 OK\r\n"
                 "Content-Type: text/html\r\n"
                 "Pragma: no-cache\r\n"
                 "\r\n"
                 "<meta http-equiv='refresh' content='2'/>"
                 "<title>Garden Info</title><br>"
                 "run:$D-$D-$D-$D<br>barrel reserves:$D<br>s1:$D<br>s2:$D<br>s3:$D<br>s4:$D<br>rain:$D,$D,$D,$D<br>frost:$S"
               ),curD,curH,curM,curS,barrelReading,
                soil1Sec1->reading,soil2Sec1->reading,soil1Sec2->reading,soil2Sec2->reading,
                (int)getRainfall(loopUnix,loopUnix+12*HOUR),(int)getRainfall(loopUnix+12*HOUR,loopUnix+DAY),(int)getRainfall(loopUnix+DAY,loopUnix+36*HOUR),(int)getRainfall(loopUnix+36*HOUR,loopUnix+2*DAY),
                String(frostWarning,DEC).c_str()
               );


  return bfill.position();
}

////time
/* callback function executed in responce to time data packets from convert unix time
*/
static void cutCallback (byte status, word off, word len) {
  Serial.print(F("cut_callback"));
  Ethernet::buffer[off+len] = 0;
  String ret = (const char*) Ethernet::buffer + off;

  String keyWord = "timestamp";
  boolean matches = false;
  for(int i = 0; i < len; i++){
     if( i > len -10){
      break;
     }
     Serial.print((char) Ethernet::buffer[off+i]);
     //check was send a timestape
     matches = true;
     for (int j = 0; j < 9; j++){
         if( (char) Ethernet::buffer[off+i+j] != keyWord.charAt(j)){
            matches = false;
            break;
         }
     }

     if (matches == true){
         Serial.print(F("r unix:"));
         Ethernet::buffer[off+i+21] = 0; // set cut off point
         Serial.println((const char*) (Ethernet::buffer +off+i+11));
         long int newRefUnix = String((const char*) Ethernet::buffer + off+i+11 ).toInt();
         
         refUnix = newRefUnix;  
         refMill = millis();
         if (startUnix == 0){
          addSingleTask(refUnix + 3, &tryUpdateWeather);
          addSingleTask(refUnix  + 20, &enableSchedTasks);
          fixSensors();
            
          startUnix = refUnix;
         }
         
         
         Serial.println(refUnix);
         break;
     }
  }
  //Serial.println("cut callback");
}

/** send http request to get time data from convert unix time
*/
void updateTime(){
  
  Serial.println("updateTime");
  ether.copyIp(ether.hisip, cutIp);
  //ether.dnsLookup(cut_url);
  ether.persistTcpConnection(false); 
  ether.browseUrl(PSTR("/"), "api?timestamp=now", cutUrl, cutCallback);
}

/**periodically try updating time data until successful
*/
void tryUpdateTime(){
  
   Serial.println(F("try update_time"));
  if(refUnix < 15000000 || ( loopUnix - refUnix > schedTasks[0].execPeriod )){
    updateTime();
    
    addSingleTask(loopUnix+tryTimePeriod, &tryUpdateTime);
  }
}

/** return the unix time correlated with a given system millis
*/
long int convertUnix (unsigned long int mill){
  long int deltaT;
  //millis has overflowed (happens about every 50 days)
  if (refMill > mill){
    deltaT = (LONG_MAX - refMill)+mill;
    
  //millis has increased like normal
  }else{
    deltaT = mill - refMill;
  }
  
  return refUnix + deltaT/1000;
}

////weather
/** callback function executed in responce to weather data packets from open weath map
 */
static void owmCallback (byte status, word off, word len) {
  Serial.print(F("owm_callback("));
  
  if(weatherPeriod == -1){
    frostWarning = 0;
  }
  
  // set ethernet buffer cutoff point
  Ethernet::buffer[off+len] = 0;

  // Strings marking key areas on json
  String date = "\"dt\"";
  String rain = "rain\":{\"3h";
  String temp = "\"temp_min\"";

  if(weatherPeriods > weatherPeriod){
    //continuously shift buffer, checking at what is left-most in the buffer for matches
   
    for(int i = 0; i < len /*l500 - off*/; i++){
      owmBuffer = owmBuffer.substring(1);
      owmBuffer.concat( String((const char) Ethernet::buffer[off+i]));
  
      //buffer contains unix time, start new weather period 
      if( owmBuffer.startsWith(date)){
        weatherPeriod += 1;
        if(weatherPeriod >= weatherPeriods){
          break;
        }
        weatherData[weatherPeriod].rain = 0.0;
        weatherData[weatherPeriod].unix = owmBuffer.substring(5,15).toInt();
        if(weatherPeriod == 0){
          Serial.println(F("SET UNIX TIME"));
          Serial.println(weatherData[0].unix );
        }
      }
      //buffer contains rain info, set rain value of current period
      else if(weatherPeriod > -1 && owmBuffer.startsWith(rain)){
        //  Serial.print("rain:");
         for(int j = 12; j < 23; j++){
            if(owmBuffer.charAt(j) == '}'){
              weatherData[weatherPeriods].rain = owmBuffer.substring(12,j).toFloat();
              //Serial.print(weather_data[weather_period].rain);
              break;
            }
         }
      }
      //buffer contains min_temp info, check for frost
      else if(frostWarning == 0 && weatherPeriod > -1 && owmBuffer.startsWith(temp)){
        for(int j = 12; j < 22; j ++){
          if(owmBuffer.charAt(j) == ','){
            if(owmBuffer.substring(12,j).toFloat()  < KELVIN + 5.0){          
              frostWarning = weatherData[weatherPeriod].unix;
            }
            break;
          }
        }
      }
    }
  }
}

/** send http request to open weather map for weather data packets
 */
void updateWeather(){
  Serial.print(F("update_weather"));
  ether.copyIp(ether.hisip, owmIp);
  ether.persistTcpConnection(true);
  weatherPeriod = -1;
  owmBuffer = ownBufferWipe;
  ether.browseUrl(PSTR("/data/2.5/"), "forecast?id=6156855&appid=[make account and insert your appid here!!!]", owmUrl, owmCallback);
}

/** periodically try updating weather data until sucessfull
 */
void tryUpdateWeather(){
  if (weatherData[0].unix  < loopUnix - 3 * HOUR){
    updateWeather();
    addSingleTask(loopUnix+tryTimePeriod, &tryUpdateWeather);
  }
}

/** determine the amount of rain in mm during a given period
 * 
 * Keyword arguments:
 * startUnix -- the start unix time of period
 * endUnix -- the end time of period
 */
float getRainfall(long int startUnix,long int endUnix){
  Serial.println(F("getRainfall"));

  //detect if hasn't gotten time
  if(weatherData[weatherPeriods-1].unix < startUnix){
    Serial.println(F("get_rainfall fail"));
    return UNDETERMINED;
  }

  //sum rain of all subperiods
  float rain = 0;
  for(int i = 0; i < weatherPeriods; i++){  // find first sub period
     if(weatherData[i].unix >= startUnix){  
       for(int j = i; j < weatherPeriods; j++){//keep adding rain from subperiods
         if(weatherData[j].unix < endUnix){
           rain += weatherData[j].rain;
         }else{//stop at last subperiod
           return rain;
         }
       }
       break;
     }
  }
  return rain;
}

////barrel
/** return val restricted between a min and max value
 */
int restrict(int val, int minVal, int maxVal){
  if (val < minVal){
    return minVal;
  }else if (val > maxVal){
    return maxVal;
  }
  return val;
}

/** set state of ball valve that refills rainbarrels
* 
* Keyword arguments:
* setOpen -- set to open position
*/
void setRefillValve(boolean setOpen) {
  Serial.println(F("setRefillValve"));
 /* digitalWrite(switchPowPin, HIGH); //enable switch power

  //getStartState
  boolean lastVal = (digitalRead(switchPin) == HIGH);
  delay(4);
  boolean newVal = (digitalRead(switchPin) == HIGH);

  //turn if needed
  if(lastVal != setOpen or newVal != setOpen){
    digitalWrite(motorPin,HIGH);
    Serial.println(F("turning"));
    while (lastVal != setOpen or newVal != setOpen) {
      lastVal = newVal;
      delay(6);
      newVal = (digitalRead(switchPin) == HIGH);
    }
     Serial.println(F("done turning"));
    digitalWrite(motorPin,LOW);
  }
  digitalWrite(switchPowPin, LOW);*/
}

/** turns the refill valve to the off position
*/
void closeRefillValve() {
  Serial.println(F("closeRefillValve"));
  setRefillValve(false);
}

/** opens the refill valve for a given amount of time
*
* Keyword arguments: 
* secs -- time to keep open in seconds
*/
void openRefillValve(int secs) {
  Serial.println(F("openRefillValve"));
  setRefillValve(true);
  addSingleTask(loopUnix + secs, &closeRefillValve);
}

/** return the time needed to fill the rainbarrel
* 
* Keyword arguments:
* height -- height the rain barrels need to be filled to
*/
int fillRainbarrel(int height){
  Serial.print(F("fillRainbarrel "));
  
  if (barrelReading < height){
    int openTime = (height - barrelReading) / barrelFillSpeed;
    if(openTime > barrelMinFill){
      Serial.println(openTime);
      openRefillValve( openTime );
      return openTime;
    }
  }
  Serial.println(0);
  return 0;
}

////soil moisture sensors
/** return state of soil sensor
 * Keyword arguments:
 *  sensor -- the soil moisture sensor to get the state of
 */
char getSoilState(soilSens_t* sensor){
   Serial.println(F("getSoilState "));
  for(char i = 0; i < 2;i++){
    if(sensor->stateChanges[i] < sensor->reading){
      Serial.println(i);
      return i;
    }
  }
  Serial.println(SOIL_WET);
  return SOIL_WET;
}

/** shift new values into a section's soil sensors
 *  
 *  sensor -- the first sensor in the linked list to be updated
 */
void updateSoilSection(soilSens_t* sensor){
   Serial.println(F("updateSoilSection"));
   while(sensor != NULL){
      sensor->recentReadings[0] = sensor->recentReadings[1];
      sensor->recentReadings[1] = analogRead(sensor->pin);
      
      if (abs(sensor->recentReadings[0] - sensor->recentReadings[1]) < 100){
        sensor->reading = (sensor->recentReadings[0] + sensor->recentReadings[1])/2;
      }
      
      sensor = sensor->next;
   }
}

/** return if should water section
 *  
 * Keyword arguments:
 * sensor -- the first sensor in the linked list of soil moisture sensors of section
 */
boolean sectionNeedsWater(soilSens_t* sensor){
   Serial.println(F("sectionNeedsWater"));
   float rain = getRainfall(loopUnix,loopUnix + 36 * HOUR);
   float recentRain = getRainfall(loopUnix - HOUR, loopUnix + 5*HOUR);
   if ( rain < rainPoint && recentRain < 4){
     boolean water = (rain < lightRainPoint);// if all sensors report meduim soil wetness, water based on if there is light rainfall
     while (sensor != NULL){
       char state = getSoilState(sensor);
       if (state == SOIL_WET){ //a sensor is too wet do not water
         water = false;
         break;
       }else if ( state == SOIL_DRY){ //a sensor is too dry would prefer water
         water = true;
       }
       sensor = sensor->next;
     }
     return water;
   }
   return false;
}

////sensor updating
void updateSensors(){
  Serial.println(F("updateSensors"));
  digitalWrite(sensorPowPin, HIGH);

  delay(2000);
 
  Serial.print(F("new barrel:"));
  Serial.print(barrelRecentReadings[1]);
  Serial.print(F("  barrel:"));
  Serial.println(barrelReading);

  //UPDATE SOIL SENSORS
  updateSoilSection(soil1Sec1);
  updateSoilSection(soil1Sec2);

  Serial.print(F("s1s1:"));
  Serial.println(soil1Sec1->reading);
  Serial.print(F("s2s1:"));
  Serial.println(soil2Sec1->reading);
  Serial.print(F("s1s2:"));
  Serial.println(soil1Sec2->reading);
  Serial.print(F("s2s2:"));
  Serial.println(soil2Sec2->reading);

  ////UPDATE BARREL LEVEL READINGS
  barrelRecentReadings[0] = barrelRecentReadings[1];
  
  digitalWrite(sonarTrigPin, LOW);
  delayMicroseconds(2);
  digitalWrite(sonarTrigPin, HIGH);
  delayMicroseconds(9);
  digitalWrite(sonarTrigPin, LOW);
  
  barrelRecentReadings[1] = pulseIn(sonarEchoPin, HIGH) / 58.0; //barrelHeight -

  //update reading value if has consistent data
  if( abs(barrelRecentReadings[0]-barrelRecentReadings[1]) < 15 ){
    barrelReading = restrict((barrelRecentReadings[0] + barrelRecentReadings[1])/2, 0, barrelHeight);
  }

  digitalWrite(sensorPowPin, LOW);
  Serial.println(F("senssors updated"));
}

/** Make sensor readings every 2 seconds until all sensors have generated consistent readings
 */
void fixSensors(){
  if(soil1Sec1->reading == UNDETERMINED  || soil2Sec1->reading == UNDETERMINED
     || soil1Sec2->reading == UNDETERMINED  || soil2Sec2->reading == UNDETERMINED 
     || barrelReading == UNDETERMINED){
    updateSensors();
    addSingleTask(loopUnix+2, &fixSensors);
  }
}

////watering
const byte bHeight = 100;

void stopWaterSec1(){
  Serial.println(F("stopWaterSec1"));
  digitalWrite(solenoid1Pin,HIGH);
}

void startWaterSec1(){
    Serial.println(F("startWaterSec1"));
    digitalWrite(solenoid1Pin,LOW);
    Serial.print(F("watering sec 1 for "));
    Serial.println(5 - (barrelHeightSpeedup*barrelReading));
    addSingleTask(loopUnix + 5 -  (int)(barrelHeightSpeedup*barrelReading), &stopWaterSec1); // *0
}

void tryWaterSec1(){
  if(sectionNeedsWater(soil1Sec1)){
    Serial.println(F("WAT G1"));
    addSingleTask(loopUnix + 3 + fillRainbarrel(bHeight), &startWaterSec1);
  }
}

void stopWaterSec2(){
  Serial.println(F("stopWaterSec2"));
  digitalWrite(solenoid2Pin,HIGH);
}

void startWaterSec2(){
  Serial.println(F("startWaterSec2"));
  Serial.print(F("watering sec2 for "));
  Serial.println(5 - (barrelHeightSpeedup*barrelReading));
  digitalWrite(solenoid2Pin,LOW);
  addSingleTask(loopUnix + 5 - (int)(barrelHeightSpeedup*barrelReading), &stopWaterSec2);//- heightPressureSpeedup*barrelReading*0
  Serial.println(F("startWaterSec2 done"));
}

void tryWaterSec2(){
 if(sectionNeedsWater(soil1Sec2)){
    Serial.println(F("needs water"));
    addSingleTask(loopUnix + 3 + fillRainbarrel(bHeight), &startWaterSec2);
  }
}


////SETUP
void setup() {
  Serial.begin(9600);
  Serial.println(F("booting"));
  //set pins
  pinMode(motorPin, OUTPUT);
  pinMode(switchPowPin, OUTPUT);
  pinMode(sonarTrigPin, OUTPUT);
  pinMode(sensorPowPin, OUTPUT);
  pinMode(solenoid1Pin, OUTPUT);
  pinMode(solenoid2Pin, OUTPUT);
  digitalWrite(solenoid1Pin,HIGH);
  digitalWrite(solenoid2Pin,HIGH);
  digitalWrite(sensorPowPin,LOW);

  // start up ethernet
  if (ether.begin(sizeof Ethernet::buffer, mymac, 10) == 0)
    Serial.println(F("Failed to access Ethernet controller"));
  ether.staticSetup(myip, gwip, gwip);
  
  Serial.println(F("working"));
  
  //set initial weather values
  for (byte i = 0; i < weatherPeriods; i++){
    weatherData[i].rain = UNDETERMINED;
    weatherData[i].unix = UNDETERMINED;
  }
  //setup scheduled tasks
  schedTasks[0].execUnix = 17*HOUR;
  schedTasks[0].execPeriod = 10*MIN;
  schedTasks[0].func = &tryUpdateTime;

  schedTasks[1].execUnix = 17*HOUR + 30*MIN;
  schedTasks[1].execPeriod = 10*MIN;
  schedTasks[1].func = &tryUpdateWeather;

  schedTasks[2].execUnix = 18*HOUR;
  schedTasks[2].execPeriod = 2*MIN;
  schedTasks[2].func = &tryWaterSec1;

  schedTasks[3].execUnix = 18*HOUR + 45*MIN;
  schedTasks[3].execPeriod = 2*MIN;
  schedTasks[3].func = &tryWaterSec2;

  schedTasks[4].execUnix = 0;
  schedTasks[4].execPeriod = 10;
  schedTasks[4].func = &updateSensors;
  
  //define soil sensors
  soil2Sec1 = (soilSens_t*) malloc(sizeof(soilSens_t));
  soil2Sec1->pin = 1; //analog pin
  soil2Sec2->stateChanges[0] = 650; //reading at which soil should stop being considered too dry - the lower the reading the dryer the soil
  soil2Sec2->stateChanges[1] = 450; //reading at which soil should start being considered too wet
  soil2Sec1->recentReadings[0] = UNDETERMINED;
  soil2Sec1->recentReadings[1] = UNDETERMINED;
  soil2Sec1->reading = UNDETERMINED;
  soil2Sec1->next = NULL;
  
  soil1Sec1 = (soilSens_t*) malloc(sizeof(soilSens_t));
  soil1Sec1->pin = 0;
  soil2Sec2->stateChanges[0] = 650;
  soil2Sec2->stateChanges[1] = 400;
  soil1Sec1->recentReadings[0] = UNDETERMINED;
  soil1Sec1->recentReadings[1] = UNDETERMINED;
  soil1Sec1->reading = UNDETERMINED;
  soil1Sec1->next = soil2Sec1;

  soil2Sec2 = (soilSens_t*) malloc(sizeof(soilSens_t));
  soil2Sec2->pin = 3;
  soil2Sec2->stateChanges[0] = 550;
  soil2Sec2->stateChanges[1] = 350;
  soil2Sec2->recentReadings[0] = UNDETERMINED;
  soil2Sec2->recentReadings[1] = UNDETERMINED;
  soil2Sec2->reading = UNDETERMINED;
  soil2Sec2->next = NULL;
  
  soil1Sec2 = (soilSens_t*) malloc(sizeof(soilSens_t));
  soil1Sec2->pin = 2;
  soil2Sec2->stateChanges[0] = 550;
  soil2Sec2->stateChanges[1] = 400;
  soil1Sec2->recentReadings[0] = UNDETERMINED;
  soil1Sec2->recentReadings[1] = UNDETERMINED;
  soil1Sec2->reading = UNDETERMINED;
  soil1Sec2->next = soil2Sec2;
  
  setRefillValve(false);
  tryUpdateTime();
}

////LOOP
void loop() {
  // put your main code here, to run repeatedly:
  word len = ether.packetReceive();
  word pos = ether.packetLoop(len);

  //Serial.println(sched_tasks[0].exec_time_unix);
  lastLoopUnix = loopUnix;
  
  loopUnix = convertUnix(millis());
  
  if (pos)  // check if valid tcp data is received
    ether.httpServerReply(homePage()); // send web page data
  if( loopUnix != lastLoopUnix){
    Serial.print("t:");
    Serial.println(loopUnix);
    checkSingleTasks(loopUnix);
    checkSchedTasks(loopUnix,lastLoopUnix);
    }
}




/*
void fixSensors(){
  Serial.println(F("fx sns loop"));
    updateSensors();
    if(evalSoilSection(soil1Sec1) == -1 || evalSoilSection(soil1Sec2) == -1 || !isReadingSteady(barrelReadings)){
      addSingleTask(loopUnix + 10, &fixSensors);
    }
}



void tryWaterGarden1(){
  Serial.println(F("twg1"));
  
  char soilState = evalSoilSection(soil1Sec1);
  //unsteady sensor data
  if(soilState == -1 || !isReadingSteady(barrelReadings)){
    addSingleTask(loopUnix + MIN, &tryWaterGarden1);
    Serial.println(F("wait water1"));
  //needs water
  }else if(soilState == 1){
    Serial.println(F("WAT G1"));
    waterGarden1();
  }
  
}

void waterGarden2(){
  Serial.println(F("wg2"));
  digitalWrite(solenoid2Pin,HIGH);
  addSingleTask(loopUnix + 20, &stopWaterGarden2);
}

void stopWaterGarden2(){
  Serial.println(F("swg2"));
  digitalWrite(solenoid2Pin,LOW);
}

void tryWaterGarden2(){
   Serial.println(F("twg2"));
  
  char soilState = evalSoilSection(soil1Sec2);
  //unsteady sensor data
  if(soilState == -1 || !isReadingSteady(barrelReadings)){
    addSingleTask(loopUnix+MIN,&tryWaterGarden2);
    Serial.println(F("wait water2"));
  //needs water
  }else if(soilState == 1){
     Serial.println(F("WAT G2"));
    waterGarden2();
  }
}*/


/*
 * 
 * 
 * void waterGarden1(){
    Serial.println(F("wg1"));
    digitalWrite(solenoid1Pin,HIGH);
    addSingleTask(loopUnix + 20, &stopWaterGarden1);
}
void waterGarden2(){
    Serial.println(F("wg2"));
    digitalWrite(solenoid2Pin,HIGH);
    addSingleTask(loopUnix + 20, &stopWaterGarden2);
}*/
