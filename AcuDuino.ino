/*
  Arduino reader for Superhet RF receiver to decode transmissions from AcuRite 5-in-1 weather station
  outputs to serial in machine-readable format (so I can read it with a Raspberry Pi and upload to Weather Underground or other services)

  Requires the TimerInterrupt library

  This weather station chops up sensor data and spreads it out across multiple bytes and nibbles.
  See this invaluable resource for decoding details:
  https://www.osengr.org/WxShield/Downloads/Weather-Sensor-RF-Protocols.pdf


  

*/


#define TIMER_INTERRUPT_DEBUG         0
#define _TIMERINTERRUPT_LOGLEVEL_     0
#define USE_TIMER_1                   true

#include <TimerInterrupt.h>
#include <TimerInterrupt.hpp>
#include <ISR_Timer.h>
#include <ISR_Timer.hpp>


//times are in microseconds
#define PULSE_T           620   //microseconds of each pulse (determined experimentally with o-scope, logic analyzer, or sound-card)
#define SAMPLES_PER_PULSE 8     //how many times to check the level during each pulse - if this is too high the program will not work because it will not be able to complete the interrupt handler fast enough, but too low and you risk garbling bits due to noise
#define PIN_RFINPUT       3     //the pin the superhet is plugged into
#define NUM_LINES         3     //the number of times the transmitter repeats itself
#define BYTES_PER_LINE    8     //the number of bytes each time the transmitter sends (one repetition)
#define BITS_PER_LINE     ( 8 * BYTES_PER_LINE )

//Calculated, only touch these if we want to fiddle with % thresholds for various bits
#define SAMPLE_T          ( PULSE_T / SAMPLES_PER_PULSE )
#define BIT_HIGH          ( 5 * PULSE_T / (10 * SAMPLE_T) ) //50% or higher is "high" when reading payload bytes
#define BIT_LOW           ( 4 * PULSE_T / (10 * SAMPLE_T) ) //Currently not used, as non-high bits are just considered low
#define PREAMBLE_HIGH     ( 9 * PULSE_T / (10 * SAMPLE_T) ) //90% or higher is "high" for preamble-pulse purposes
#define PREAMBLE_LOW      ( 2 * PULSE_T / (10 * SAMPLE_T) ) //20% or lower is "low" for preamble-pulse purposes
#define SAMPLE_FREQUENCY  ( 1000000 / SAMPLE_T )

volatile byte buffer[SAMPLES_PER_PULSE];
volatile byte buffer_index;
volatile byte buffer_sum;
volatile byte buffer_sum_min;
volatile unsigned int timer_counter;
volatile unsigned int sync_counter;
volatile bool preamble;
volatile bool preamble_sync;
volatile bool reading;
volatile byte line;
volatile byte outbytes[3][BYTES_PER_LINE] = {{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0}};
volatile byte *checkedbytes;

//final byte of each line is (sum of all other bytes)%256
//find the first line that matches (do checkbits on data bytes as well)
bool checkBytes() {
  for(int i = 0; i < NUM_LINES; i++) {
    byte sum = 0;
    for(int j = 0; j < BYTES_PER_LINE - 1; j++) {
      sum += outbytes[i][j];
    }
    if(sum == outbytes[i][BYTES_PER_LINE-1]){
      //the checksum passed; check data byte checkbits
      bool checkbits = true;
      for(int j = 3; j < 7; j++) {
        bool cb = false;
        for(int k = 0; k < 7; k++) {
          cb = cb ^ bitRead(outbytes[i][j],k);
        }
        checkbits = checkbits & (bitRead(outbytes[i][j],7) == cb);
      }
      if(checkbits) {
        checkedbytes = outbytes[i];
        return true;
      }
    }
  }
  //No line was acceptable; do not attempt to read
  return false;
}

//Take an int (tenths) and convert to a string, adding a decimal point before final digit
String convertTenths(int n) {
  String tenths = String(n);
  byte chars = tenths.length();
  return tenths.substring(0, chars-1)+"."+tenths.substring(chars-1);
}

String getWindSpeed() {
  int wind_speed_raw = (( checkedbytes[3] & 0x1F ) << 3)
                      + (( checkedbytes[4] & 0x70 ) >> 4);
  //avoiding floating-point math here by using tenths
  if(wind_speed_raw != 0)
    return convertTenths( (wind_speed_raw * 643 + 777 ) / 125);
  else
    return "0";
}

String getWindDirection() {
  //output in degrees
  byte wind_direction_raw = ( checkedbytes[4] & 0x0F );
  return String((wind_direction_raw + 1) * 15);
}

String getRain() {
  //each bucket tip is 0.01 inch.  Passing in tenths to avoid floating-point math
  int bucket_tips = ( int( checkedbytes[5] & 0x3F ) << 7 )
                           + int( checkedbytes[6] & 0x7F );
  return convertTenths(bucket_tips / 10);
}

String getTemp() {
  //avoiding floating-point math
  int temp_raw = (int( checkedbytes[4] & 0x0F ) << 7 )
                        + int( checkedbytes[5] & 0x7F );
  return convertTenths(temp_raw - 400);
}

String getHumidity() {
  return String( checkedbytes[6] & 0x7F );
}


void decodeMessage() {
  byte message_id = checkedbytes[2] & 0x0F;
  //The only valid message ids are 1 and 8

  /*for(int i = 0; i < BYTES_PER_LINE; i++) {
    Serial.print(checkedbytes[i], "HEX");
    Serial.print(" ");
  }
  Serial.print("\n");*/

  if(message_id == 1) {
    Serial.println("windspeedmph:"+getWindSpeed()+",winddir:"+getWindDirection()+",rainin:"+getRain());
  } else if (message_id == 8) {
    Serial.println("windspeedmph:"+getWindSpeed()+",tempf:"+getTemp()+",humidity:"+getHumidity());
  } 
}

//Interrupt handler - runs with a frequency based on SAMPLES_PER_PULSE
//If SAMPLES_PER_PULSE is too high the program will not function properly, but if it is too low you risk losing a lot of data to noise
void sample() {
  noInterrupts();
  timer_counter += 1;
  buffer_sum -= buffer[buffer_index];
  buffer[buffer_index] = digitalRead(PIN_RFINPUT);
  buffer_sum += buffer[buffer_index];
  buffer_index = (buffer_index + 1) % SAMPLES_PER_PULSE;

  //start preamble check if high
  if(reading && timer_counter % SAMPLES_PER_PULSE == 0) {
    unsigned short int bytenum = (timer_counter-1)/SAMPLES_PER_PULSE/8;
    unsigned short int bitnum = 7-((timer_counter-1)/SAMPLES_PER_PULSE)%8;
    if(buffer_sum > BIT_HIGH) {
      bitSet(outbytes[line][bytenum], bitnum); 
    } else {
      bitClear(outbytes[line][bytenum], bitnum); 
    }
    if(timer_counter >= BITS_PER_LINE*SAMPLES_PER_PULSE) {
      //finished with line
      reading = false;
      timer_counter = 0;
      line += 1;
      if(line == 3) {
        //We made it to the end of the 3 repeats
        //Now we'll check each for validity with their checkbyte and checkbits on data bytes
        //Use the first one that passes the checks to put out the data received
        if(checkBytes()) 
          decodeMessage();
        line = 0;
      }
    }
  } else if(!preamble && buffer_sum >= PREAMBLE_HIGH) {
    //Sense the start of the preamble
    preamble_sync = true;
    sync_counter = 0;
    timer_counter = 0;
    buffer_sum_min = 255;    
  } else if(preamble_sync) {
    //In the first part of the preamble, synchronize using low pulse
    if(buffer_sum < buffer_sum_min){
      //minimum buffer sum should be right at the end of the second sync (low) pulse, reset counter right then
      buffer_sum_min = buffer_sum;
      timer_counter = 0;
    }
    sync_counter += 1;
    if(sync_counter >= 3 * SAMPLES_PER_PULSE / 2) {
      //done reading preamble low pulse and (hopefully) synced; move on to verifying the rest of the preamble
      preamble_sync = false;
      preamble = true;
    }
  } else if(preamble) {
    //if in the middle of preamble check, check at end of each pulse-length
    if(timer_counter % SAMPLES_PER_PULSE == 0) {
      if(timer_counter >= SAMPLES_PER_PULSE * 4 ) {
        //preamble successfully read! Begin reading bytes
        preamble = false;
        reading = true;
        timer_counter = 0;
      } else {
        //check each preamble pulse to make sure it is the appropriate value (alternating high and low)
        byte val = 2;
        if(buffer_sum >= PREAMBLE_HIGH) val = 1;
        if(buffer_sum <= PREAMBLE_LOW) val = 0;
        if((timer_counter/SAMPLES_PER_PULSE)%2 != val) {
          //preamble mismatch, aborting and returning to regular seek cycle
          preamble = false;
        }
      }
    }
  }
  interrupts();
}

void setup() {
  //Set up input pin
  pinMode(PIN_RFINPUT, INPUT);

  //Initialize hardware timer
  ITimer1.init();

  //Initialize all variables
  buffer_index = 0;
  buffer_sum = 0;
  timer_counter = 0;
  line = 0;
  preamble = false;
  preamble_sync = false;
  reading = false;
  checkedbytes = outbytes[0];

  //start serial (to send data to Pi)
  Serial.begin(9600);
  while (!Serial);  //Wait for it to start
  //Serial.println("Starting reader...");

  //Attach interrupt to timer
  if(!ITimer1.attachInterrupt(SAMPLE_FREQUENCY, sample)) {
    Serial.println("Could not start timer!");
  }
}

void loop() {
  //Everything is handled in interrupts
  //Possible future to-do: in loop() periodically check to see if there is valid data waiting and put it out to Serial (instead of in handler)
  //This might shorten the interrupt handler's runtime and allow for more samples per pulse
  //Could time somehoe to run in the 15-second interval between pulse-trains
}
