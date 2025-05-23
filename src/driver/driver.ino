
#include <SPI.h>
#include <Adafruit_VS1053.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <SD.h>
#include <SdFat.h>
#include <vector>
#include <Preferences.h>

// SPI VS1053
#define VS_RST    9       // VS1053 reset pin (output)
#define VS_CS     10      // VS1053 chip select pin (output)
#define VS_DCS    8       // VS1053 Data/command select pin (output)
#define DREQ      3
#define CARDCS    4       // Card chip select pin

// Control Pins
#define BTN_PLAY  A0
#define BTN_NEXT  A1
#define BTN_PREV  A2
#define BTN_SEEKF A3
#define BTN_SEEKB A6

// Volume Pins
//#define POT_VOL   A7
#define BTN_VOLUP D0
#define BTN_VOLDN D1

// OLED display
#define OLED_RESET   -1
#define SCREEN_ADDRESS 0x3C 
Adafruit_SSD1306 display(128, 32, &Wire, OLED_RESET);

// VS1053 player object
Adafruit_VS1053_FilePlayer musicPlayer = Adafruit_VS1053_FilePlayer(VS_RST, VS_CS, VS_DCS, DREQ, CARDCS);

Preferences prefs;

// Dynamic Playlist
std::vector<String> playlist;
int currentTrackIndex = 0;
SdFat SD2;
SdFile root;

// Debounce state
uint32_t lastBtnTime = 0;

// Track end detection
bool wasPlaying = false;

// Volume state (0 = loud, 100 = silent)
uint8_t volumeLevel = 50;
const uint8_t VOLUME_STEP = 1;

void setup() {
  Serial.begin(9600);

  Serial.println("Adafruit VS1053 + Arduino ESP32-S3");

  // initialise the music player
  if (! musicPlayer.begin()) { // initialise the music player
     Serial.println(F("Couldn't find VS1053, do you have the right pins defined?"));
     while (1);
  }
  Serial.println(F("VS1053 found"));

  //musicPlayer.sineTest(0x44, 500);    // Make a tone to indicate VS1053 is working
 
  if (!SD.begin(CARDCS)) {
    Serial.println(F("SD failed, or not present"));
    while (1);  // don't do anything more
  }
  Serial.println("SD OK!");

  musicPlayer.useInterrupt(VS1053_FILEPLAYER_TIMER0_INT);

  Wire.begin();
  // SSD1306_SWITCHCAPVCC = generate display voltage from 3.3V internally
  if(!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    Serial.println(F("SSD1306 allocation failed"));
    for(;;); // Don't proceed, loop forever
  }

  //display.display();
  //delay(2000);
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  
  // File root = SD.open("/");
  // while (true) {
  //   File f = root.openNextFile();
  //   if (!f) break;
  //   String fn = f.name();
  //   if (!f.isDirectory() && fn.endsWith(".mp3") && !fn.startsWith(".")) {
  //     playlist.push_back("/" + fn);
  //   }
  //   f.close();
  // }
  SD2.begin(CARDCS);
  playlist.reserve(128);         // avoid repeated reallocations
  root.openRoot(&SD2);
  SdFile f;
  while (f.openNext(&root, O_READ)) {
    if (!f.isDir()) {
      char name[64];
      f.getName(name, sizeof(name));

      // *** skip macOS resource‐fork files “._…” ***
      if (name[0] == '.' && name[1] == '_') {
        f.close();
        continue;
      }

      int len = strlen(name);
      if (len>4 && !strcasecmp(name+len-4, ".mp3")) {
        playlist.push_back(String("/") + name);
      }
    }
    f.close();
  }
  Serial.print("Found "); Serial.print(playlist.size()); Serial.println(" tracks");

  for (String name : playlist) {
    Serial.println(name);
  }

  // buttons
  for (int pin : {BTN_PLAY,BTN_NEXT,BTN_PREV,BTN_SEEKF,BTN_SEEKB,BTN_VOLUP,BTN_VOLDN}) {
    pinMode(pin, INPUT_PULLUP);
  }
  
  // Set volume for left, right channels. lower numbers == louder volume!
  musicPlayer.setVolume(volumeLevel, volumeLevel);

  // start first track
  //playTrack(0);
  prefs.begin("player", false); // namespace "player", read/write
  
  // read saved state
  int savedIdx = prefs.getInt("idx", 0);
  uint32_t savedPos = prefs.getUInt("pos", 0);

  // clamp
  if (savedIdx >= 0 && savedIdx < playlist.size()) {
    currentTrackIndex = savedIdx;
  } else {
    currentTrackIndex = 0;
  }

  // start & seek
  playTrack(currentTrackIndex);

  // if we actually have a saved byte‐position, jump there:
  if (savedPos > 0) {
    File &f = musicPlayer.currentTrack;
    uint32_t fileLen = f.size();
    if (savedPos < fileLen) {
      f.seek(savedPos);        // jump to exactly where we left off
      musicPlayer.feedBuffer(); // refill the VS1053’s buffer
    }
  }
}

void loop() {
  bool isPlaying = musicPlayer.playingMusic;
  if (musicPlayer.stopped()) {
    nextTrack();
    delay(50);
  }
  wasPlaying = isPlaying;

  if (millis() - lastBtnTime < 200) return; // simple debounce

  if (!digitalRead(BTN_PLAY)) {
    pauseToggle();
    lastBtnTime = millis();
  }
  else if (!digitalRead(BTN_NEXT)) {
    nextTrack();
    lastBtnTime = millis();
  }
  else if (!digitalRead(BTN_PREV)) {
    prevOrRestart();
    lastBtnTime = millis();
  }
  else if (!digitalRead(BTN_SEEKF)) {
    seekRelative(+5);  // forward 5 s
    lastBtnTime = millis();
  }
  else if (!digitalRead(BTN_SEEKB)) {
    seekRelative(-5);  // back 5 s
    lastBtnTime = millis();
  }
  if (!digitalRead(BTN_VOLUP)) {
    volumeLevel = max((int)volumeLevel - VOLUME_STEP, 0);
    musicPlayer.setVolume(volumeLevel, volumeLevel);
    updateDisplay();
  }
  if (!digitalRead(BTN_VOLDN)) {
    volumeLevel = min((int)volumeLevel + VOLUME_STEP, 100);
    musicPlayer.setVolume(volumeLevel, volumeLevel);
    updateDisplay();
  }

  updateDisplay();
  delay(10);
}

//––– Transport routines –––

void playTrack(int idx) {
  if (idx < 0 || idx >= (int)playlist.size()) return;
  musicPlayer.stopPlaying();
  currentTrackIndex = idx;
  musicPlayer.startPlayingFile(playlist[idx].c_str());
  updateDisplay();
  saveState();
}

void nextTrack() {
  Serial.println("Next");
  playTrack( (currentTrackIndex + 1) % playlist.size() );
  saveState();
}

void prevOrRestart() {
  // decodeTime() gives seconds into the current song
  if (musicPlayer.decodeTime() > 2) {
    Serial.println("Restart");
    playTrack(currentTrackIndex);
  } else {
    Serial.println("Previous");
    int prevIdx = (currentTrackIndex - 1 + playlist.size()) % playlist.size();
    playTrack(prevIdx);
    saveState();
  }
}

void pauseToggle() {
  bool nowPaused = !musicPlayer.paused();
  musicPlayer.pausePlaying(nowPaused);
  Serial.println(nowPaused ? "Paused" : "Resumed");
  updateDisplay();
  saveState();
}

void seekRelative(int sec) {
  File &f = musicPlayer.currentTrack;
  if (!f) return;
  uint32_t bytesPerSec = (f.size() / (musicPlayer.decodeTime() + 1));
  int32_t newPos = (int32_t)f.position() + sec * bytesPerSec;
  newPos = constrain(newPos, 0, (int32_t)f.size());
  f.seek(newPos);
  musicPlayer.feedBuffer();
  Serial.printf("Seek %+.ds → byte %ld\n", sec, newPos);
  saveState();
}

// String readID3Title(const String &path) {
//   File f = SD.open(path, FILE_READ);
//   if (f && f.size() > 128) {
//     f.seek(f.size() - 128);
//     char tag[3];
//     f.read((uint8_t*)tag, 3);
//     if (!memcmp(tag, "TAG", 3)) {
//       char title[31] = {0};
//       f.read((uint8_t*)(title), 30);
//       f.close();
//       return String(title);
//     }
//   }
//   if(f) f.close();
//   // fallback to filename (without path)
//   int slash = path.lastIndexOf('/');
//   return path.substring(slash + 1);
// }

void saveState() {
  // record track index
  prefs.putInt("idx", currentTrackIndex);
  // record byte‐offset
  File &f = musicPlayer.currentTrack;
  prefs.putUInt("pos", f.position());
}

void updateDisplay() {
  display.clearDisplay();
  display.setCursor(0, 0);

  // 1) Track name
  display.println(playlist[currentTrackIndex].substring(1,playlist[currentTrackIndex].length() - 4));
  //display.println( readID3Title( playlist[currentTrackIndex] ) ); // BLOCKING CALLS SO TOO SLOW, DONT USE

  // 2) Elapsed + total time
  File &f = musicPlayer.currentTrack;
  uint16_t elapsed = musicPlayer.decodeTime();     // seconds played
  uint32_t pos     = f.position();                 // bytes read so far
  uint32_t size    = f.size();                     // total bytes in file

  // estimate total seconds: size * elapsed / pos
  uint16_t total = (pos > 0)
    ? (uint16_t)((uint64_t)size * elapsed / pos)
    : 0;

  // format "mm:ss/mm:ss"
  char buf[16];
  snprintf(buf, sizeof(buf), "%02u:%02u/%02u:%02u",
           elapsed/60, elapsed%60,
           total/60,   total%60);
  display.println(buf);

  // 3) Volume
  display.print(F("Vol: "));
  display.print(map(volumeLevel, 0, 100, 100, 0));
  display.println(F("%"));

  display.display();
}
