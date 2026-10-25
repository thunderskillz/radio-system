#include <Wire.h>
#include <U8g2lib.h>
#include <Preferences.h>
#include <radio.h>
#include <SI4703.h>

#define FREQUENCY_MAX 10800
#define FREQUENCY_MIN 7600

#define LAST_FREQUENCY_KEY "last_frequency"

#define TIME_NO_SECONDS "%02d:%02d"
#define DATE_CLIMATE "%s %02d %s %02d C %02d%%"

#define INFO_SCROLL_DELAY 120
#define INFO_DRAW_DELAY 5
#define BATTERY_INERTIA_PADDING 120
#define CHARGE_COUNTER_INTERVAL 12
#define BATTERY_CUTOFF_VALUE 3050

#define VOLUME_HIGH_ICON 277
#define VOLUME_LOW_ICON 278
#define VOLUME_MUTE_ICON 279
#define SETTINGS_ICON 81

#define VOLUME_DISPLAY_TIME 5
#define VOLUME_INERTIA_PADDING 180
#define VOLUME_STEP_SIZE 120

#define INFORMATION_CHECK_INTERVAL 4

#define ROTARY_EVENT_DEBOUNCE_TIME 100

#define AHT20_ADDRESS 0x38
#define DS3231_ADDRESS 0x68
#define DS3231_SECONDS 0x00
#define DS3231_MINUTES 0x01
#define DS3231_HOURS 0x02
#define DS3231_DOW 0x03
#define DS3231_DAY 0x04
#define DS3231_MONTH 0x05
#define DS3231_YEAR 0x06

#define buttonRotPin 16
#define rotaryPinA 17
#define rotaryPinB 5

#define radioResetPin 18

#define SDAPin 21

#define batteryChargingPin 25
#define batteryVoltagePin 33

#define volumePin 2

#define onOffSensorPin 13
#define onPin 12
#define offPin 14

const char *const weekdaysShort[] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
const char *const monthsShort[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

const byte AHT20_INIT[3] = {0xBE, 0x08, 0x00};
const byte AHT20_MEASURE[3] = {0xAC, 0x33, 0x00};

SI4703 radio;
U8G2_SH1106_128X64_NONAME_1_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

bool hasReleasedOnOffButton = false;
bool shuttingDown = false;
byte batteryLevel;
const int batteryThresholds[] = {3230, 3410, 3590, 3770, 4100};
byte chargeCounter;
short batterySamples[5] = {0, 0, 0, 0, 0};
byte batterySampleStartIndex = 0;

volatile uint16_t frequency;
byte temperature = 20;
byte humidity = 0;
char formattedTime[6];
char formattedDateClimate[25];

Preferences preferences;

bool buttonRotaryFlag = false;

bool volumeDisplayed;
byte volume = 255;
byte volumeHideTime = 255;

bool seeking = false;
/*
char informationText[65];
char rdsProgName[9];
int receivedRds = 0;
byte receivedRdsProgName = 0;
char rdsCurrentInformation[65];
byte rdsSameCounter = 0;
byte rdsProgNameSameCounter = 0;
char progName[9];
byte informationCheckTime = 255;
*/
uint16_t block1;
uint16_t block2;
uint16_t block3;
uint16_t block4;
bool rdsProcessingFlag = false;
byte setTimeCounter;
int setTimeMinutes;
byte minutesSetting, hoursSetting, daySetting, monthSetting, yearSetting;

int infoScrollCounter = 0;
byte drawInfoDelayCounter = 0;

byte seconds;

volatile bool screenDirty = false;

void IRAM_ATTR rotaryISR() {
  static long lastEventMillis = 0;
  long now = millis();
  if (now < lastEventMillis + ROTARY_EVENT_DEBOUNCE_TIME) {
    return;
  }
  lastEventMillis = now;
  if (digitalRead(rotaryPinB)) {
    frequency -= 10;
  } else {
    frequency += 10;
  }
}

void setup() {
  preferences.begin("radio_store", false); 
  setupPins();
  setupOnOff();
  setupRadio();
  initAht20();
  /*
  byte hour = requestTime(DS3231_HOURS);
  if(hour != 19) {
    setTime(8, 19, 20, 3, 5, 2026);
  }
  */
  u8g2.begin();
  u8g2.setFontPosTop();
}

void loop() {
  updateData();
  updateScreen();
  delay(10);
}

void setupRadio() {
  radio.setup(RADIO_RESETPIN, radioResetPin);
  radio.setup(RADIO_SDAPIN, SDAPin);
  radio.setup(RADIO_FMSPACING, RADIO_FMSPACING_100);
  radio.setup(RADIO_DEEMPHASIS, RADIO_DEEMPHASIS_50);
  radio.initWire(Wire);
  frequency = preferences.getUShort(LAST_FREQUENCY_KEY, 10190);
  radio.setBandFrequency(RADIO_BAND_FM, frequency);
  radio.setVolume(3);
  radio.setMono(false);
  radio.setMute(false);
  radio.attachReceiveRDS(rdsHandler);
 // resetInformationText();
}

void setupPins() {
  pinMode(onPin, OUTPUT);
  pinMode(offPin, OUTPUT);
  pinMode(onOffSensorPin, INPUT);
  pinMode(volumePin, INPUT);
  pinMode(buttonRotPin, INPUT_PULLUP);
  pinMode(rotaryPinA, INPUT_PULLUP);
  pinMode(rotaryPinB, INPUT_PULLUP);
  pinMode(batteryVoltagePin, INPUT);
  pinMode(batteryChargingPin, INPUT);
  attachInterrupt(rotaryPinA, rotaryISR, RISING);
}

void setupOnOff() {
  digitalWrite(offPin, LOW);
  digitalWrite(onPin, HIGH);
  delay(10);
  digitalWrite(onPin, LOW);
}

void initAht20() {
  i2cSendData(AHT20_ADDRESS, AHT20_INIT, 3);
}

void updateData() {
  checkOnOff();
  checkClimate();
  checkTime();
  checkBattery();
  checkVolume();
  checkFrequency();
  checkInformation();
  checkButtonRotary();
}

void checkOnOff() {
  if(hasReleasedOnOffButton) {
    bool onOffPressed = digitalRead(onOffSensorPin);
    shuttingDown |= onOffPressed;
    // Wait until button is released before shutting down
    if(shuttingDown && !onOffPressed) {
      digitalWrite(offPin, HIGH);
    }
  } else {
    hasReleasedOnOffButton = digitalRead(onOffSensorPin) == LOW;
  }
}

void checkTime() {
  static byte lastCheckedSecond = 61;
  seconds = requestTime(DS3231_SECONDS);
  if (seconds == lastCheckedSecond) {
    return;
  }
  lastCheckedSecond = seconds;

  byte minutes = requestTime(DS3231_MINUTES);
  byte hours = requestTime(DS3231_HOURS);
  byte day = requestTime(DS3231_DAY);
  sprintf(formattedTime, TIME_NO_SECONDS, hours, minutes);
  sprintf(formattedDateClimate, DATE_CLIMATE, weekdaysShort[requestTime(DS3231_DOW) - 1], day, monthsShort[requestTime(DS3231_MONTH) - 1], temperature, humidity);
  screenDirty = true;
}

void checkBattery() {
  static byte lastChargeCounterIncrementSeconds = 61;
  if (digitalRead(batteryChargingPin)) {
    if(lastChargeCounterIncrementSeconds != seconds) {
      batteryLevel = (batteryLevel + 1) % 5;
      lastChargeCounterIncrementSeconds = seconds;
    } 
  } else {
    calculateBatteryLevel();
  }
}

void calculateBatteryLevel() {
  uint16_t batteryVoltage = analogRead(batteryVoltagePin);

  if (batteryVoltage < BATTERY_CUTOFF_VALUE) {
    digitalWrite(offPin, HIGH);
  }
  if(batterySamples[0] == 0) {
    for(int i = 0; i < 5; i++) {
      batterySamples[i] = batteryVoltage;
    }
  }

  batterySamples[batterySampleStartIndex] = batteryVoltage;
  
  if(batterySampleStartIndex == 4) {
    batterySampleStartIndex = 0;
  } else {
    batterySampleStartIndex++;
  }

  uint16_t batteryTotal = 0;

  for(int i = 0; i < 5; i++) {
    batteryTotal += batterySamples[i];
  }

  short batteryAverage = batteryTotal / 5;

  for (byte i = 0; i < 5; i++) {
    if(batteryAverage < (batteryThresholds[i] + (batteryLevel == i ? BATTERY_INERTIA_PADDING : 0))) {
      batteryLevel = i;
      return;
    }
  }
}

void checkClimate() {
  static byte lastCheckedSecond = 61;
  if (seconds == lastCheckedSecond) {
    return;
  }
  lastCheckedSecond = seconds;
  i2cSendData(AHT20_ADDRESS, AHT20_MEASURE, 3);
  Wire.requestFrom(AHT20_ADDRESS, 7); 
  byte bytes[6];
  if (7 > Wire.available()) {
    return;
  }
  for (int i = 0; i < 6; i++) {
    bytes[i] = Wire.read();
  }
  humidity = ((float)(((long)bytes[1] << 12) + (bytes[2] << 4) + (bytes[3] >> 4)) / ((long)1 << 20) * 100);
  temperature = ((float)((((long)bytes[3] & 0b1111) << 16) + ((long)bytes[4] << 8) + bytes[5])/((long)1 << 20)) * 200 - 50;
}

void checkVolume() {
  int readVal = analogRead(volumePin);
  int newVolume = readVal / VOLUME_STEP_SIZE;
  byte radioVolume = newVolume * 1.7;
  if(volume == 255) {
    volume = newVolume;
    radio.setVolume(radioVolume);
    return;
  }
  bool newVolumeDisplayed;
  if(newVolume != volume && (readVal > (volume + 1) * VOLUME_STEP_SIZE + VOLUME_INERTIA_PADDING || readVal < volume * VOLUME_STEP_SIZE - VOLUME_INERTIA_PADDING)) {
    volumeDisplayed = true;
    volume = newVolume;
    volumeHideTime = (seconds + VOLUME_DISPLAY_TIME) % 60;
    radio.setVolume(radioVolume);
    screenDirty = true;
  } else {
    volumeDisplayed = volumeDisplayed && (seconds < volumeHideTime || (volumeHideTime < VOLUME_DISPLAY_TIME && seconds > volumeHideTime));
  }
}

void checkFrequency() {
  if(frequency > FREQUENCY_MAX) frequency = FREQUENCY_MAX;
  if(frequency < FREQUENCY_MIN) frequency = FREQUENCY_MIN;
  uint16_t radioFrequency = radio.getFrequency();
  if(radioFrequency != frequency) {
    if(seeking) {
      frequency = radioFrequency;
      seeking = false;
    } else {
      radio.setFrequency(frequency);
    }
    screenDirty = true;
    preferences.putUShort(LAST_FREQUENCY_KEY, frequency);
   // resetInformationText();
  }
}

void checkInformation() {
  if(rdsProcessingFlag) {
    processRds();
  }
  radio.checkRDS();
}

void checkButtonRotary() {
  bool pressed = digitalRead(buttonRotPin) == LOW;
  if(!pressed && buttonRotaryFlag) {
    radio.seekUp();
    seeking = true;
    buttonRotaryFlag = false;
  }
  buttonRotaryFlag = pressed;
}

void updateScreen() {
  if(!screenDirty) {
    return;
  }
  u8g2.firstPage();
  do {
    drawBattery();
    drawTimeClimate();
    drawVolume();
    drawRadio();
  } while(u8g2.nextPage());
  screenDirty = false;
}

void drawBattery() {
  int fillWidth = batteryLevel * 5;
  u8g2.drawFrame(102,0,24,9);
  u8g2.drawBox(104,2,fillWidth,5);
  u8g2.drawBox(126,3,2,3);
}

void drawTimeClimate() {
  u8g2.setFont(u8g2_font_profont10_tr);
  u8g2.drawStr(0, 2, formattedDateClimate);
  u8g2.setFont(u8g2_font_m2icon_5_tf);
  u8g2.drawGlyph(65, 0, 81);
  u8g2.setFont(u8g2_font_profont22_tf);
  u8g2.drawStr(28, 42, formattedTime);
}

void drawVolume() {
  if(!volumeDisplayed) return;
  byte distance = round(4.3 * volume);
  byte boxY = 57 - distance;
  byte boxH = 7 + distance;
  u8g2.drawRFrame(114, 14, 12, 50, 5);
  if (volume > 0)  {
    u8g2.drawRBox(114, boxY, 12, boxH, 5);
  }

  int icon;
  if(volume == 0) {
    icon = VOLUME_MUTE_ICON;
  } else if(volume < 6) {
    u8g2.setDrawColor(0);
    icon = VOLUME_LOW_ICON;
  } else {
    u8g2.setDrawColor(0);
    icon = VOLUME_HIGH_ICON; 
  }
  u8g2.setFont(u8g2_font_open_iconic_all_1x_t);
  u8g2.drawGlyph(116, 53, icon);
  u8g2.setDrawColor(1);
}

void drawRadio() {
  drawFrequency();
  //drawInfo();
} 

void drawFrequency() {
  u8g2.setFont(u8g2_font_profont17_tr);
  char frequencyText[13];
  dtostrf((double)frequency / 100, 5, 1, frequencyText);
  strcat(frequencyText, " MHz FM");
  u8g2.drawStr(2, 16, frequencyText);
}
/*
void drawInfo() {
  //byte textLength = strlen(informationText);
  byte height = 34;
  /*
  if(textLength > 2) {
    int xd = infoScrollCounter < INFO_SCROLL_DELAY ? 0 : infoScrollCounter - INFO_SCROLL_DELAY;
    int textSteps = xd / 7;
    u8g2.setFont(u8g2_font_profont15_tf);
    if(volumeDisplayed) {
      char* ss = subStr(informationText, textSteps + 16 > textLength ? textLength : textSteps + 15 + (xd % 7 > 3 ? 1 : 0));
      u8g2.drawStr(2 - xd, height, ss);
      if(textLength - textSteps < 10) {
        int nextLineX = textLength*7 + 60 - xd;
        int numChars = (106 - nextLineX)/7;
        if(numChars > 0) {
          ss = subStr(informationText, numChars > textLength ? textLength : numChars);
          u8g2.drawStr(nextLineX, height, ss);
        }
        if(nextLineX == 2) infoScrollCounter = 0;
      }
    } else {
      u8g2.drawStr(2 - xd, height, informationText);
      if(textLength - textSteps < 10) {
        int nextLineX = textLength*7 + 60 - xd;
        u8g2.drawStr(nextLineX, height, informationText);
        if(nextLineX == 2) infoScrollCounter = 0;
      }
    }

    u8g2.setColorIndex(0);
    if(volumeDisplayed) {
      u8g2.drawBox(104, height, 6, 24);
    } else {
      u8g2.drawBox(126, height, 2, 24);
    }
    u8g2.drawBox(0, height, 2, 24);
    u8g2.setColorIndex(1);
    if(drawInfoDelayCounter == INFO_DRAW_DELAY) {
      infoScrollCounter++;
      drawInfoDelayCounter = 0;
    } else {
      drawInfoDelayCounter++;
    }
    u8g2.drawStr(2, 50, progName);
  } else if (strlen(progName) > 2) {
    u8g2.setFont(u8g2_font_profont15_tf);
    u8g2.drawStr(2, height, progName);
  //}
}
*/
void rdsHandler(uint16_t b1, uint16_t b2, uint16_t b3, uint16_t b4) {
  if(!rdsProcessingFlag) {
    rdsProcessingFlag = true;
    block1 = b1;
    block2 = b2;
    block3 = b3;
    block4 = b4;
  }
}

void processRds() {
  byte type = (block2 >> 11);

  if(type == 0b1000) {
    processRdsTime();
  } // else if ((type < 2) && (receivedRdsProgName != 0b1111)) {
    // processRdsName();
  //} else if (type == 0b100) {
    //processRdsInfo();
  //}
  rdsProcessingFlag = false;
}

/*
 * 1. Calculate the date and time.
 * 2. Sanity check the date.
 * 3. Check what date and time is already set.
 * 4. If they are different keep going. If they are the same, we should mark our time as "good" by setting `setTimeCounter` to 0.
 * When `setTimeCounter` is >2, the time will be set.
 * 5. We check if `setTimeCounter` is 0, if so, we save this "candidate time". We should get the same time 3 times in a row in
 * order to actually set it. This makes sure we don't receive bad data and set some random time.
 * 6. If we got some new time, we throw away the old candidate time and save this new one.
 * 7. We repeat until the 3rd time we get the same time, then we set it and reset `setTimeCounter`.
 */
void processRdsTime() {
  long mjd = (((long)block2 & 0b11) << 15) + (block3 >> 1);
  byte hour = ((block3 & 1) << 4) + (block4 >> 12);
  byte minute = (block4 >> 6) & 0b111111;
  byte offset = block4 & 0b11111;

  minute += offset * 30 * ((block4 & 0b100000) ? -1 : 1);
  byte hourOffset = minute/60;
  minute = minute % 60;
  hour = hour + hourOffset;
  byte dayOffset = hour / 24;
  hour = hour % 24;
  mjd += dayOffset;
  unsigned int tempY = (mjd - 15078.2)/365.25;
  unsigned long tempYB = tempY * 365.25;
  byte tempM = (mjd - 14956.1 - tempYB)/30.6001;
  unsigned int tempMB = tempM * 30.6001;
  byte day = mjd - 14956 - tempYB - tempMB;
  byte k = tempM == 14 || tempM == 15 ? 1 : 0;
  byte month = tempM - 1 - k * 12;
  byte year = tempY + k - 100;

  if((day <= 31) && (month <= 12)) {
    byte minutesC = requestTime(DS3231_MINUTES);
    byte hoursC = requestTime(DS3231_HOURS);
    byte dayC = requestTime(DS3231_DAY);
    byte monthC = requestTime(DS3231_MONTH);
    byte yearC = requestTime(DS3231_YEAR);

    if(!((minute == minutesC) && (hour == hoursC) && (day == dayC) && (month == monthC) && (year == yearC))) {
      if(setTimeCounter > 0) {
        int minuteOffset = 60 * hoursC + minutesC - setTimeMinutes;
        hoursSetting = (hoursSetting + ((minuteOffset + minutesSetting) / 60)) % 24;
        minutesSetting = (minutesSetting + minuteOffset) % 60;
        setTimeMinutes = 60 * hoursC + minutesC;
        if((minute == minutesSetting) && (hour == hoursSetting) && (day == daySetting) && (month == monthSetting) && (year == yearSetting)) {
          if(setTimeCounter > 1) {
            setTime(minute, hour, day, (mjd + 2) % 7 + 1, month, year);
            setTimeCounter = 0;
          } else {
            setTimeCounter++;
          }
        } else {
          setTimeCounter = 1;
          saveCandidateTime(minute, hour, day, month, year, minutesC, hoursC);
        }
      } else {
        setTimeCounter = 1;
        saveCandidateTime(minute, hour, day, month, year, minutesC, hoursC);
      }
    } else {
      setTimeCounter = 0;
    }
  }
}

/*
 * setTimeMinutes is the exact current minute of the day. This is used to compare future received minutes since they won't be the same.
 * We don't do it for hours since we don't have to. Worst case, it takes twice as long for the hour to be set.
 */
void saveCandidateTime(byte minute, byte hour, byte day, byte month, byte year, byte minutesC, byte hoursC) {
  minutesSetting = minute;
  hoursSetting = hour;
  daySetting = day;
  monthSetting = month;
  yearSetting = year;
  setTimeMinutes = 60 * hoursC + minutesC;
}
/*
void processRdsName() {
  byte k = (block2 & 0b11);
  byte i = k*2;

  char blockText[2];
  blockText[0] = block4>>8;
  blockText[1] = block4;

  if (!(receivedRdsProgName >> k & 1)) {
    rdsProgNameSameCounter = 0;
    receivedRdsProgName |= (1<<k);
    for (byte j = 0; j < 2; j++) {
      rdsProgName[i + j] = blockText[j];
    }
  } else if (allBitsSet(receivedRdsProgName)) {
    rdsProgNameSameCounter++;
  }
  
  bool same = true;
  for (byte j = 0; j < 2; j++) {
    same = same && rdsProgName[i + j] == blockText[j];
  }

  if (!same) {
    rdsProgNameSameCounter = 0;
    receivedRdsProgName = 0;
    for (int j = 0; j < 9; j++) {
      rdsProgName[j] = '\0';
    }
  }

  if ((receivedRdsProgName == 0b1111) || (rdsProgNameSameCounter > 10)) {
    rdsProgNameSameCounter = 0;
    resetProgName();
    strcpy_convert(progName, rdsProgName);
    progName[16 - (2*__builtin_clz(receivedRdsProgName))] = '\0';
  }
}
*/
/*
void processRdsInfo() {
  byte k = (block2 & 0b1111);
  byte i = k*4;

  char blockText[4];
  blockText[0] = block3>>8;
  blockText[1] = block3;
  blockText[2] = block4>>8;
  blockText[3] = block4;

  if (receivedRds >> k & 1) {
    bool same = true;
    for(byte j = 0; j < 4; j++) {
      same = same && blockText[j] == rdsCurrentInformation[i + j];
      rdsCurrentInformation[i + j] = blockText[j];
    }
    if (!same) {
      clearRdsCurrentInformation();
      addBlockToCurrentText(k, blockText);
    } else if (allBitsSet(receivedRds)) {
      rdsSameCounter++;
      if(rdsSameCounter > 8) {
        rdsSameCounter = 0;
        resetInfo();
        strcpy_convert(informationText, rdsCurrentInformation);
        informationText[64 - (4*__builtin_clz(receivedRds))] = '\0';
      }
    }
  } else {
    addBlockToCurrentText(k, blockText);
  }
}

void addBlockToCurrentText(byte k, char blockText[]) {
  receivedRds |= (1<<k);
  rdsSameCounter = 0;
  for(byte j = 0; j < 4; j++) {
    rdsCurrentInformation[(k*4) + j] = blockText[j];
  }
}
*/
/*
void strcpy_convert(char* to, char* from) {
  byte i = 0;
  char c;
  while((c = from[i]) != '\0') {
    to[i] = convertSpecialCharacter(c);
    i++;
  }
  to[i] = '\0';
}
*/
/*
void clearRdsCurrentInformation() {
  for(byte i = 0; i < 65; i++) {
    rdsCurrentInformation[i] = '\0';
  }
  receivedRds = 0;
}
char convertSpecialCharacter(char c) {
  if (c == 0x91) {
    return 0xE4;
  } else if (c == 0x97) {
    return 0xF6;
  } else if (c == 0xD1) {
    return 0xC4;
  } else if (c == 0xD7) {
    return 0xD6;
  } else if (c == 0xE1) {
    return 0xC5;
  } else if (c == 0xF1) {
    return 0xE5;
  } else {
    return c;
  }
}

bool allBitsSet(int i) {
  return (i & (i + 1)) == 0;
}

void resetInformationText() {
  resetProgName();
  resetInfo();
}

void resetProgName() {
  receivedRdsProgName = 0;
  for(int i = 0; i < 9; i++) {
    progName[i] = '\0';
  }
}

void resetInfo() {
  receivedRds = 0;
  for(int i = 0; i < 65; i++) {
    informationText[i] = '\0';
  }
}
*/

void i2cSendData(byte address, const byte data[], byte length) {
  Wire.beginTransmission(address);
  for (byte i = 0; i < length; i ++) {
    Wire.write(data[i]);
  }
  Wire.endTransmission();
}

void i2cSendData(byte address, const byte data) {
  Wire.beginTransmission(address);
  Wire.write(data);
  Wire.endTransmission();
}

byte requestTime(byte type) {
  i2cSendData(DS3231_ADDRESS, type);
  return i2cReadTime();
}

void setTime(byte type, byte value) {
  Wire.beginTransmission(DS3231_ADDRESS);
  Wire.write(type);
  Wire.write(decimalToBinary(value));
  Wire.endTransmission();
}

void setTime(byte minute, byte hour, byte day, byte dow, byte month, byte year) {
  setTime(DS3231_SECONDS, 1);
  setTime(DS3231_MINUTES, minute);
  setTime(DS3231_HOURS, hour);
  setTime(DS3231_DAY, day);
  setTime(DS3231_DOW, dow);
  setTime(DS3231_MONTH, month);
  setTime(DS3231_YEAR, year);
}

byte i2cReadTime() {
  Wire.requestFrom(DS3231_ADDRESS, 1); 
  if (1 <= Wire.available()) {
    return binaryToDecimal(Wire.read());
  } else {
    return 0;
  }
}

byte binaryToDecimal(byte binary) {
  return (binary >> 4) * 10 + (binary & 0b1111);
}

byte decimalToBinary(byte decimal) {
  return ((decimal / 10) << 4) + (decimal % 10);
}

char* subStr(char* s, byte end) {
  static char ns[65];
  for (byte i = 0; i < 65; i++) {
    if(i < end) {
      ns[i] = s[i];
    } else {
      ns[i] = '\0';
    }
  }
  return ns;
}
