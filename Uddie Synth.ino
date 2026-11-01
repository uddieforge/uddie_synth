#include <Mozzi.h>
#include <Oscil.h>
#include <WaveShaper.h>
#include <mozzi_midi.h>
#include <tables/sin2048_int8.h>
#include <tables/triangle2048_int8.h>
#include <tables/saw2048_int8.h>
#include <tables/waveshape_chebyshev_3rd_256_int8.h>
#include <tables/waveshape_chebyshev_6th_256_int8.h>
#include <tables/waveshape_compress_512_to_488_int16.h>

// ==========================
// POTENTIOMETRES
// A0 = Attack
// A1 = Release (0 à 10 s — tempo max 2 s/pas, silence garanti absent à fond)
// A2 = Tempo
// A3 = Swing
// A4 = Vitesse aGain2 (waveform 0) / Vitesse LFO 0.02-10Hz (waveforms 1-2) / Densité grains (waveform 3) / Vitesse LFO quinte (waveform 4)
// A5 = Mix Chebyshev 3rd↔6th (waveform 0) / Depth LFO (waveforms 1-2) / Durée fenêtre grain (waveform 3) / Profondeur LFO quinte (waveform 4)
// A6 = Vitesse aGain1 (waveform 0) / Détune+vibrato saw (waveform 1) / PWM (waveform 2) / Jitter grain+fréq (waveform 3) / Mix fondamentale↔quinte (waveform 4)
// A7 = Clavier
//
// WAVEFORMS — sélection par bouton D2
// 0 = Waveshaper sinusoidal
//     A4 : vitesse de aGain2 (chemin Chebyshev 6th) — 0.05 Hz à 5 Hz
//     A5 : mix entre Chebyshev 3rd (doux) et 6th (agressif) — course inversée
//     A6 : vitesse de aGain1 (chemin Chebyshev 3rd) — 0.1 Hz à 30 Hz
// 1 = Super Saw
//     A4 : vitesse LFO — 0.02 Hz à 10 Hz (courbe quadratique)
//     A5 : profondeur vibrato fréquentiel (±5% de la note)
//     A6 : détune de base entre les deux oscillateurs saw (0 à 30 Hz)
// 2 = Pulse PWM
//     A4 : vitesse LFO — 0.02 Hz à 10 Hz (courbe quadratique)
//     A5 : profondeur LFO
//     A6 : seuil PWM (largeur d'impulsion)
// 3 = Granulaire
//     A4 : densité de base (période entre grains)
//     A5 : durée fenêtre grain (min 30% de la période pour éviter les silences)
//     A6 : jitter temporel + randomisation fréquence grain aux graves
//     En mode RANDOM_PLAY : A4, A5, A6 sont randomisés ±50%
//     autour de leur valeur courante à chaque nouveau pas
// 4 = Quintes (deux dents de scie : fondamentale + quinte juste à ×1.5)
//     A4 : vitesse LFO — 0.02 Hz à 10 Hz
//     A5 : profondeur LFO sur la quinte (vibrato fréquentiel de la quinte seule)
//     A6 : mix entre fondamentale pure (gauche) et quinte (droite)
//
// LEDS
// D3  = octave down / séquenceur (ledFlashD3) / REC / erase / PAUSED
// D11 = octave up  / séquenceur (ledFlashD11) / REC / erase / PAUSED
// D12 = changement de VCO (flash bref au appui sur D2)
// ==========================

// --- Waveform 0 : Waveshaper ---
Oscil <SIN2048_NUM_CELLS,      MOZZI_AUDIO_RATE> aSin(SIN2048_DATA);
Oscil <SIN2048_NUM_CELLS,      MOZZI_AUDIO_RATE> aGain1(SIN2048_DATA);
Oscil <SIN2048_NUM_CELLS,      MOZZI_AUDIO_RATE> aGain2(SIN2048_DATA);

WaveShaper <char> aCheby3rd(CHEBYSHEV_3RD_256_DATA);
WaveShaper <char> aCheby6th(CHEBYSHEV_6TH_256_DATA);
WaveShaper <int>  aCompress(WAVESHAPE_COMPRESS_512_TO_488_DATA);

// Mix Chebyshev : 0 = 3rd pur, 255 = 6th pur
uint8_t chebyMix = 128;

// --- Waveform 1 : Super Saw ---
Oscil <SAW2048_NUM_CELLS,      MOZZI_AUDIO_RATE> oSaw1(SAW2048_DATA);
Oscil <SAW2048_NUM_CELLS,      MOZZI_AUDIO_RATE> oSaw2(SAW2048_DATA);

float sawVibratoHz = 0.0f;

// --- Waveform 2 : Pulse PWM ---
Oscil <SAW2048_NUM_CELLS,      MOZZI_AUDIO_RATE> oPulse(SAW2048_DATA);

// --- Waveform 3 : Granulaire ---
Oscil <SIN2048_NUM_CELLS,      MOZZI_AUDIO_RATE> oGrain(SIN2048_DATA);

float grainFreqBase    = 110.0f;
float grainFreqCurrent = 110.0f;
float grainFreqDrift   = 0.0f;

// --- Waveform 4 : Quintes ---
Oscil <SAW2048_NUM_CELLS, MOZZI_AUDIO_RATE> oFund(SAW2048_DATA);
Oscil <SAW2048_NUM_CELLS, MOZZI_AUDIO_RATE> oFifth(SAW2048_DATA);

float fifthVibratoHz = 0.0f;
uint8_t fifthMix = 128;

// --- LFO triangle (waveforms 1 et 2) ---
Oscil <TRIANGLE2048_NUM_CELLS, MOZZI_CONTROL_RATE> aLfo(TRIANGLE2048_DATA);

// ==========================
// MOTEUR GRANULAIRE
// ==========================
unsigned int grainPeriod      = 800;
unsigned int grainWindow      = 400;
unsigned int grainJitter      = 0;
unsigned int grainPhase       = 0;
unsigned int grainNextTrigger = 800;
bool         grainActive      = false;
unsigned int grainEnvPhase    = 0;

unsigned int grainPeriodBase = 400;
int          grainNoteIndex  = 5;

float grainFreqJitterHz = 0.0f;

bool         grainRandActive = false;
unsigned int grainRandA4     = 512;
unsigned int grainRandA5     = 512;
unsigned int grainRandA6     = 512;

// ==========================
// FORME D'ONDE
// ==========================
byte waveform = 0;
bool lastD2   = false;

int8_t pwmThreshold = 0;
float  detuneHz     = 0.0f;

// LED D12 : indicateur VCO (N flashes par seconde selon waveform+1)
// Machine à états : FLASH_ON → FLASH_OFF → (répéter N fois) → PAUSE → recommence
unsigned long ledVcoLast    = 0;
byte          ledVcoFlashN  = 0;   // flash en cours dans le cycle (0-based)
bool          ledVcoOn      = false;
bool          ledVcoInPause = false;
const unsigned long LED_VCO_ON_MS    = 80;
const unsigned long LED_VCO_OFF_MS   = 150;
const unsigned long LED_VCO_PAUSE_MS = 800;

// ==========================
// CLAVIER
// ==========================
// Valeurs ADC mesurées sur A7 pour chaque note (Do à Si)
const int noteValues[12]  = {932, 845, 761, 680, 602, 526, 450, 375, 301, 226, 152, 76};
const int tolerance       = 40;
const int noteOffsets[12] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};

// ==========================
// SEQUENCEUR
// ==========================
#define MAX_STEPS 64
byte sequence[MAX_STEPS];
byte seqLength    = 0;
byte seqIndex     = 0;
byte activeLength = 0;

// ==========================
// MODE DE LECTURE
// ==========================
enum PlayMode { FORWARD, REVERSE, PINGPONG, PINGPONG_NO_REPEAT, RANDOM_PLAY };
PlayMode playMode = FORWARD;

bool pingPongDir       = true;
byte lastRandomIndex   = 255;
byte randomRepeatCount = 0;

bool lastD8 = false;

// ==========================
// ETATS
// ==========================
enum Mode { IDLE, REC, READ, PAUSED };
Mode mode = IDLE;

// ==========================
// BOUTON D6
// ==========================
bool          lastBtn          = false;
unsigned long btnPressTime     = 0;
bool          longPressHandled = false;

unsigned long lastStepTime = 0;

// ==========================
// TRANSPOSITION
// ==========================
int octaveOffset        = 0;
int transpoKeyOffset    = 0;
int transpoOctaveOffset = 0;

bool lastD4 = false;
bool lastD5 = false;

// ==========================
// FREQUENCE COURANTE
// ==========================
float current_freq = 110.0f;

// ==========================
// SWING
// ==========================
bool          swingStep        = false;
int           swingDelayMs     = 0;
bool          swingPending     = false;
unsigned long swingTriggerTime = 0;

// ==========================
// ENVELOPPE
// ==========================
float env         = 0.0f;
bool  keyHeld     = false;
bool  lastKeyHeld = false;
bool  gate        = false;

// ==========================
// LED SEQUENCEUR (D3 + D11)
// ==========================
unsigned long       ledFlashTime       = 0;
const unsigned long LED_FLASH_DURATION = 30;

bool ledFlashD3  = false;
bool ledFlashD11 = false;

bool reverseToggle = false;

// ==========================
// LED REC
// ==========================
unsigned long       ledRecTime     = 0;
bool                ledRecState    = false;
const unsigned long LED_REC_PERIOD = 100;

// ==========================
// LED CONFIRMATION ERASE
// ==========================
bool                eraseConfirm           = false;
unsigned long       eraseConfirmTime       = 0;
const unsigned long ERASE_CONFIRM_DURATION = 1000;

// ==========================
// SYNC
// ==========================
#define PIN_SYNC_IN            7
#define PIN_SYNC_OUT           10
#define SYNC_PULSE_DURATION_MS 20

bool          lastSyncIn    = false;
unsigned long syncOutTime   = 0;
bool          syncOutActive = false;

byte syncStepCounter = 0;


// ==========================
// PSEUDO-ALEATOIRE (xorshift)
// ==========================
static uint16_t xorRng = 12345u;

uint16_t fastRand(){
  xorRng ^= (xorRng << 7);
  xorRng ^= (xorRng >> 9);
  xorRng ^= (xorRng << 8);
  return xorRng;
}

unsigned int randomizeAdc(unsigned int center){
  int offset = (int)(fastRand() % 1023) - 511;
  int result = (int)center + offset;
  if(result < 0)    result = 0;
  if(result > 1023) result = 1023;
  return (unsigned int)result;
}


// ==========================
void setAllOscFreq(float freq){
  aSin.setFreq(freq);

  oSaw1.setFreq(freq + sawVibratoHz + detuneHz * 0.5f);
  oSaw2.setFreq(freq + sawVibratoHz - detuneHz * 0.5f);

  oPulse.setFreq(freq);

  grainFreqBase = freq;

  oFund.setFreq(freq);
  oFifth.setFreq(freq * 1.5f + fifthVibratoHz);
}


// ==========================
void updateGrainPeriod(){
  int norm      = (11 - grainNoteIndex);
  int factor_q8 = 64 + (norm * norm * 192) / 121;
  if(factor_q8 < 64)  factor_q8 = 64;
  if(factor_q8 > 256) factor_q8 = 256;
  grainPeriod = (unsigned int)(((unsigned long)grainPeriodBase * factor_q8) >> 7);
  if(grainPeriod < 50)   grainPeriod = 50;
  if(grainPeriod > 1600) grainPeriod = 1600;
}


// ==========================
void triggerGrain(){
  grainActive   = true;
  grainEnvPhase = 0;

  float jitterAmp = grainFreqJitterHz;
  float offset = 0.0f;
  if(jitterAmp > 0.0f){
    int r   = (int)(fastRand() % 1001) - 500;
    offset  = (float)r * jitterAmp / 500.0f;
  }
  grainFreqCurrent = grainFreqBase + offset;
  if(grainFreqCurrent < 20.0f) grainFreqCurrent = 20.0f;

  oGrain.setFreq(grainFreqCurrent);

  if(grainWindow > 0 && offset != 0.0f){
    grainFreqDrift = -offset / (float)grainWindow;
  } else {
    grainFreqDrift = 0.0f;
  }
}


// ==========================
void setup(){
  startMozzi();

  pinMode(2, INPUT);
  pinMode(4, INPUT);
  pinMode(5, INPUT);
  pinMode(6, INPUT);
  pinMode(8, INPUT);

  pinMode(3,  OUTPUT);   // LED séquenceur / octave down
  pinMode(11, OUTPUT);   // LED séquenceur / octave up
  pinMode(12, OUTPUT);   // LED changement VCO

  pinMode(PIN_SYNC_IN,  INPUT);
  pinMode(PIN_SYNC_OUT, OUTPUT);
  digitalWrite(PIN_SYNC_OUT, LOW);

  setAllOscFreq(110.f);
  aGain1.setFreq(2.f);
  aGain2.setFreq(0.4f);
  aLfo.setFreq(0.08f);
}


// ==========================
void outputSyncPulse(){
  digitalWrite(PIN_SYNC_OUT, HIGH);
  syncOutActive = true;
  syncOutTime   = millis();
}


// ==========================
byte nextSeqIndex(){

  if(activeLength == 0) return 0;

  switch(playMode){

    case FORWARD:
      seqIndex++;
      if(seqIndex >= activeLength) seqIndex = 0;
      return seqIndex;

    case REVERSE:
      if(seqIndex == 0 || seqIndex >= activeLength) seqIndex = activeLength - 1;
      else seqIndex--;
      return seqIndex;

    case PINGPONG:
      if(pingPongDir){
        seqIndex++;
        if(seqIndex >= activeLength){
          seqIndex    = activeLength - 1;
          pingPongDir = false;
        }
      } else {
        if(seqIndex == 0){
          seqIndex    = 0;
          pingPongDir = true;
        } else {
          seqIndex--;
        }
      }
      return seqIndex;

    case PINGPONG_NO_REPEAT:
      if(pingPongDir){
        seqIndex++;
        if(seqIndex >= activeLength){
          seqIndex    = (activeLength >= 2) ? activeLength - 2 : 0;
          pingPongDir = false;
        }
      } else {
        if(seqIndex == 0){
          seqIndex    = (activeLength >= 2) ? 1 : 0;
          pingPongDir = true;
        } else {
          seqIndex--;
        }
      }
      return seqIndex;

    case RANDOM_PLAY:{
      if(activeLength == 1) return 0;
      byte candidate;
      byte attempts = 0;
      do {
        candidate = rand() % activeLength;
        attempts++;
      } while(
        candidate == lastRandomIndex &&
        randomRepeatCount >= 2       &&
        attempts < 20
      );
      if(candidate == lastRandomIndex) randomRepeatCount++;
      else                             randomRepeatCount = 1;
      lastRandomIndex = candidate;
      seqIndex        = candidate;
      return seqIndex;
    }

    default:
      return 0;
  }
}


// ==========================
void computeLedFlash(){

  switch(playMode){

    case FORWARD:
      ledFlashD3  = true;
      ledFlashD11 = true;
      break;

    case REVERSE:
      reverseToggle = !reverseToggle;
      ledFlashD3    =  reverseToggle;
      ledFlashD11   = !reverseToggle;
      break;

    case PINGPONG:
      if(seqIndex == 0 || seqIndex == activeLength - 1){
        ledFlashD3  = true;
        ledFlashD11 = true;
      } else if(pingPongDir){
        ledFlashD3  = true;
        ledFlashD11 = false;
      } else {
        ledFlashD3  = false;
        ledFlashD11 = true;
      }
      break;

    case PINGPONG_NO_REPEAT:
      if(pingPongDir){
        ledFlashD3  = true;
        ledFlashD11 = false;
      } else {
        ledFlashD3  = false;
        ledFlashD11 = true;
      }
      break;

    case RANDOM_PLAY:{
      byte r      = rand() % 3;
      ledFlashD3  = (r == 0 || r == 2);
      ledFlashD11 = (r == 1 || r == 2);
      break;
    }

    default:
      ledFlashD3  = true;
      ledFlashD11 = true;
      break;
  }
}


// ==========================
void randomizeGrainParams(){
  grainRandA4     = randomizeAdc((unsigned int)mozziAnalogRead(A4));
  grainRandA5     = randomizeAdc((unsigned int)mozziAnalogRead(A5));
  grainRandA6     = randomizeAdc((unsigned int)mozziAnalogRead(A6));
  grainRandActive = true;
}


// ==========================
void triggerStep(){
  nextSeqIndex();
  computeLedFlash();
  ledFlashTime = millis();

  syncStepCounter++;
  if(syncStepCounter >= 2){
    syncStepCounter = 0;
    outputSyncPulse();
  }

  if(waveform == 3 && playMode == RANDOM_PLAY){
    randomizeGrainParams();
  } else {
    grainRandActive = false;
  }

  int midi_note = sequence[seqIndex];

  if(midi_note != 0){
    int transposed = midi_note + transpoKeyOffset + (transpoOctaveOffset * 12);
    current_freq   = mtof(transposed);

    if(waveform == 3){
      grainNoteIndex = transposed % 12;
      updateGrainPeriod();
    }

    env  = 0.0f;
    gate = true;
  }
}


// ==========================
void updateControl(){

  unsigned long now = millis();

  // --- SYNC OUT ---
  if(syncOutActive && (now - syncOutTime >= SYNC_PULSE_DURATION_MS)){
    digitalWrite(PIN_SYNC_OUT, LOW);
    syncOutActive = false;
  }

  // --- SYNC IN ---
  bool syncIn = digitalRead(PIN_SYNC_IN);

  if(syncIn && !lastSyncIn){
    if(mode == READ && seqLength > 0){
      swingPending = false;
      lastStepTime = now;
      triggerStep();
    }
    else if((mode == IDLE || mode == PAUSED) && seqLength > 0){
      mode                = READ;
      seqIndex            = 0;
      pingPongDir         = true;
      reverseToggle       = false;
      swingStep           = false;
      swingPending        = false;
      activeLength        = seqLength;
      transpoKeyOffset    = 0;
      transpoOctaveOffset = 0;
      syncStepCounter     = 0;
      grainRandActive     = false;
      lastStepTime        = now;
      triggerStep();
    }
  }

  lastSyncIn = syncIn;

  // ==========================
  // ==========================
  // BOUTON D2 — SELECTION FORME D'ONDE
  // ==========================
  bool stateD2 = digitalRead(2);

  if(stateD2 && !lastD2){
    waveform++;
    if(waveform > 4) waveform = 0;
    grainRandActive = false;
    sawVibratoHz    = 0.0f;
    fifthVibratoHz  = 0.0f;
    // Redémarre le cycle de clignotement D12 immédiatement
    ledVcoFlashN  = 0;
    ledVcoOn      = true;
    ledVcoInPause = false;
    ledVcoLast    = now;
    digitalWrite(12, HIGH);
  }

  lastD2 = stateD2;

  // --- LED D12 : N flashes = numéro du VCO (waveform+1) ---
  // Séquence : ON(80ms) OFF(150ms) × N  puis  PAUSE(800ms)  puis recommence
  {
    byte target = waveform + 1;   // 1 à 5
    if(ledVcoOn){
      if(now - ledVcoLast >= LED_VCO_ON_MS){
        digitalWrite(12, LOW);
        ledVcoOn   = false;
        ledVcoLast = now;
      }
    } else if(ledVcoInPause){
      if(now - ledVcoLast >= LED_VCO_PAUSE_MS){
        ledVcoInPause = false;
        ledVcoFlashN  = 0;
        ledVcoOn      = true;
        ledVcoLast    = now;
        digitalWrite(12, HIGH);
      }
    } else {
      // inter-flash OFF
      if(now - ledVcoLast >= LED_VCO_OFF_MS){
        ledVcoFlashN++;
        if(ledVcoFlashN >= target){
          // cycle complet → pause
          ledVcoInPause = true;
          ledVcoLast    = now;
        } else {
          ledVcoOn   = true;
          ledVcoLast = now;
          digitalWrite(12, HIGH);
        }
      }
    }
  }

  // --- BOUTON D8 ---
  bool stateD8 = digitalRead(8);

  if(stateD8 && !lastD8){
    if(mode == REC){
      if(seqLength < MAX_STEPS) sequence[seqLength++] = 0;
    }
    else if(mode == READ){
      int pm = (int)playMode + 1;
      if(pm > 4) pm = 0;
      playMode          = (PlayMode)pm;
      pingPongDir       = true;
      lastRandomIndex   = 255;
      randomRepeatCount = 0;
      seqIndex          = 0;
      reverseToggle     = false;
      swingStep         = false;
      swingPending      = false;
      grainRandActive   = false;
    }
  }

  lastD8 = stateD8;

  // --- BOUTON D6 ---
  bool btn = digitalRead(6);

  if(btn && !lastBtn){
    btnPressTime     = now;
    longPressHandled = false;
  }

  if(btn && !longPressHandled && (now - btnPressTime > 1000)){
    seqLength           = 0;
    seqIndex            = 0;
    activeLength        = 0;
    mode                = IDLE;
    playMode            = FORWARD;
    pingPongDir         = true;
    reverseToggle       = false;
    swingStep           = false;
    swingPending        = false;
    transpoKeyOffset    = 0;
    transpoOctaveOffset = 0;
    syncStepCounter     = 0;
    grainRandActive     = false;
    sawVibratoHz        = 0.0f;
    fifthVibratoHz      = 0.0f;
    longPressHandled    = true;
    eraseConfirm        = true;
    eraseConfirmTime    = now;
    digitalWrite(3,  HIGH);
    digitalWrite(11, HIGH);
  }

  if(!btn && lastBtn){
    if(!longPressHandled){
      if(mode == IDLE){
        mode        = REC;
        ledRecState = true;
        ledRecTime  = now;
      }
      else if(mode == REC){
        if(seqLength > 0){
          mode                = READ;
          seqIndex            = 0;
          pingPongDir         = true;
          reverseToggle       = false;
          swingStep           = false;
          swingPending        = false;
          activeLength        = seqLength;
          transpoKeyOffset    = 0;
          transpoOctaveOffset = 0;
          syncStepCounter     = 0;
          grainRandActive     = false;
          lastStepTime        = now;
        }
      }
      else if(mode == READ)   { mode = PAUSED; }
      else if(mode == PAUSED) { mode = READ; lastStepTime = now; swingPending = false; }
    }
  }

  lastBtn = btn;

  // --- A0 = Attack / A1 = Release (max 10 s) ---
  int attackMs  = map(mozziAnalogRead(A0), 0, 1023, 10, 5000);
  int releaseMs = map(mozziAnalogRead(A1), 0, 1023, 0, 10000);

  float attackRate  = (attackMs  <= 0) ? 1.0f : 1.0f / (attackMs  * 0.02f);
  float releaseRate = (releaseMs <= 0) ? 1.0f : 1.0f / (releaseMs * 0.02f);

  if(attackRate  > 1.0f) attackRate  = 1.0f;
  if(releaseRate > 1.0f) releaseRate = 1.0f;

  // ==========================
  // A4 / A5 / A6 selon waveform
  // ==========================

  if(waveform == 0){

    // A4 → vitesse aGain2 (Chebyshev 6th) : 0.05 Hz à 5 Hz
    float a4Norm  = mozziAnalogRead(A4) * (1.0f / 1023.0f);
    float a4Curve = a4Norm * a4Norm;
    aGain2.setFreq(a4Curve * 4.95f + 0.05f);

    // A5 → mix Chebyshev 3rd↔6th (course inversée : gauche = 6th, droite = 3rd)
    chebyMix = (uint8_t)(255 - (mozziAnalogRead(A5) >> 2));

    // A6 → vitesse aGain1 (Chebyshev 3rd) : 0.1 Hz à 30 Hz
    float a6Norm = mozziAnalogRead(A6) * (1.0f / 1023.0f);
    aGain1.setFreq(a6Norm * 29.9f + 0.1f);

    aLfo.next();

  } else if(waveform == 1){

    float a4Norm  = mozziAnalogRead(A4) * (1.0f / 1023.0f);
    float a4Curve = a4Norm * a4Norm;
    aLfo.setFreq(a4Curve * 9.98f + 0.02f);

    float lfoRaw = (float)aLfo.next() / 128.0f;
    float depth  = mozziAnalogRead(A5) * (1.0f / 1023.0f);

    sawVibratoHz = lfoRaw * depth * current_freq * 0.05f;

    float a6Norm  = mozziAnalogRead(A6) * (1.0f / 1023.0f);
    float a6Curve = a6Norm * a6Norm;
    detuneHz      = a6Curve * 30.0f;

  } else if(waveform == 2){

    float a4Norm  = mozziAnalogRead(A4) * (1.0f / 1023.0f);
    float a4Curve = a4Norm * a4Norm;
    aLfo.setFreq(a4Curve * 9.98f + 0.02f);

    float lfoRaw = (float)aLfo.next() / 128.0f;
    float depth  = mozziAnalogRead(A5) * (1.0f / 1023.0f);

    float a6Raw = (float)mozziAnalogRead(A6);
    float a6Mod = a6Raw + lfoRaw * depth * 511.0f;
    if(a6Mod < 0.0f)    a6Mod = 0.0f;
    if(a6Mod > 1023.0f) a6Mod = 1023.0f;

    float a6Norm  = a6Mod / 1023.0f;
    float a6Curve = a6Norm * a6Norm;
    pwmThreshold  = (int8_t)(a6Curve * 120.0f);

    sawVibratoHz = 0.0f;

  } else if(waveform == 3){

    unsigned int valA4 = grainRandActive ? grainRandA4 : (unsigned int)mozziAnalogRead(A4);
    unsigned int valA5 = grainRandActive ? grainRandA5 : (unsigned int)mozziAnalogRead(A5);
    unsigned int valA6 = grainRandActive ? grainRandA6 : (unsigned int)mozziAnalogRead(A6);

    float a4Norm    = valA4 * (1.0f / 1023.0f);
    grainPeriodBase = (unsigned int)(800.0f - a4Norm * a4Norm * 700.0f);
    if(grainPeriodBase < 100) grainPeriodBase = 100;

    updateGrainPeriod();

    float a5Norm = valA5 * (1.0f / 1023.0f);
    unsigned int grainWindowMin = (unsigned int)(grainPeriod * 0.30f);
    if(grainWindowMin < 10) grainWindowMin = 10;
    grainWindow = grainWindowMin + (unsigned int)(a5Norm * (float)(grainPeriod - grainWindowMin));
    if(grainWindow >= grainPeriod) grainWindow = grainPeriod - 1;

    float a6Norm = valA6 * (1.0f / 1023.0f);
    grainJitter  = (unsigned int)(a6Norm * a6Norm * (float)(grainPeriod >> 1));

    float graveBoost  = 1.0f + (float)(11 - grainNoteIndex) * (2.0f / 11.0f);
    grainFreqJitterHz = a6Norm * current_freq * 0.08f * graveBoost;

    sawVibratoHz   = 0.0f;
    fifthVibratoHz = 0.0f;
    aLfo.next();

  } else {

    float a4Norm  = mozziAnalogRead(A4) * (1.0f / 1023.0f);
    float a4Curve = a4Norm * a4Norm;
    aLfo.setFreq(a4Curve * 9.98f + 0.02f);

    float lfoRaw = (float)aLfo.next() / 128.0f;
    float depth  = mozziAnalogRead(A5) * (1.0f / 1023.0f);

    fifthVibratoHz = lfoRaw * depth * current_freq * 1.5f * 0.05f;

    fifthMix = (uint8_t)(mozziAnalogRead(A6) >> 2);

    sawVibratoHz = 0.0f;
  }

  // --- A2 = Tempo / A3 = Swing ---
  int   tempoMs   = map(mozziAnalogRead(A2), 0, 1023, 2000, 30);
  float swingNorm = mozziAnalogRead(A3) * (1.0f / 1023.0f);
  swingDelayMs    = (int)(swingNorm * swingNorm * tempoMs * 0.40f);

  // --- OCTAVES D4/D5 ---
  bool stateD4 = digitalRead(4);
  bool stateD5 = digitalRead(5);

  if(mode == READ || mode == PAUSED){
    if(stateD4 && !lastD4){ transpoOctaveOffset--; if(transpoOctaveOffset < -3) transpoOctaveOffset = -3; }
    if(stateD5 && !lastD5){ transpoOctaveOffset++; if(transpoOctaveOffset >  3) transpoOctaveOffset =  3; }
  } else {
    if(stateD4 && !lastD4){ octaveOffset--; if(octaveOffset < -3) octaveOffset = -3; }
    if(stateD5 && !lastD5){ octaveOffset++; if(octaveOffset >  3) octaveOffset =  3; }
  }

  lastD4 = stateD4;
  lastD5 = stateD5;

  // ==========================
  // LEDs D3 + D11 (séquenceur / octave)
  // D12 est géré indépendamment (VCO flash, voir section BOUTON D2)
  // ==========================
  if(eraseConfirm){
    if(now - eraseConfirmTime >= ERASE_CONFIRM_DURATION){
      eraseConfirm = false;
      digitalWrite(3,  LOW);
      digitalWrite(11, LOW);
    }
    // pendant eraseConfirm : D3 et D11 restent HIGH (allumées dans le bloc long-press)
  } else if(mode == REC){
    if(now - ledRecTime >= LED_REC_PERIOD){ ledRecTime = now; ledRecState = !ledRecState; }
    digitalWrite(3,  ledRecState ? HIGH : LOW);
    digitalWrite(11, ledRecState ? HIGH : LOW);
  } else if(mode == READ){
    if(now - ledFlashTime < LED_FLASH_DURATION){
      digitalWrite(3,  ledFlashD3  ? HIGH : LOW);
      digitalWrite(11, ledFlashD11 ? HIGH : LOW);
    } else {
      digitalWrite(3,  LOW);
      digitalWrite(11, LOW);
    }
  } else if(mode == PAUSED){
    digitalWrite(3,  HIGH);
    digitalWrite(11, HIGH);
  } else {
    // IDLE : clignotement indiquant l'offset d'octave
    // D3 clignote si octave down, D11 clignote si octave up
    int flashes = abs(octaveOffset);
    int ledDown = LOW, ledUp = LOW;
    static int blinkCount = 0;
    static bool inPause = false, ledStateLocal = false;
    static unsigned long lastTime = 0;
    if(flashes == 0){ ledStateLocal = false; blinkCount = 0; inPause = false; }
    else {
      if(inPause){ if(now - lastTime > 1000){ inPause = false; blinkCount = 0; lastTime = now; } }
      else {
        if(now - lastTime > 100){
          lastTime = now; ledStateLocal = !ledStateLocal;
          if(!ledStateLocal){ blinkCount++; if(blinkCount >= flashes){ inPause = true; ledStateLocal = false; } }
        }
      }
    }
    if     (octaveOffset < 0) ledDown = ledStateLocal;
    else if(octaveOffset > 0) ledUp   = ledStateLocal;
    digitalWrite(3,  ledDown);
    digitalWrite(11, ledUp);
  }

  // --- CLAVIER ---
  int val       = mozziAnalogRead(A7);
  int noteIndex = -1;
  for(int i = 0; i < 12; i++){
    if(val > (noteValues[i] - tolerance) && val < (noteValues[i] + tolerance)){
      noteIndex = i; break;
    }
  }
  keyHeld = (noteIndex != -1);

  if(mode == REC){
    if(keyHeld && !lastKeyHeld && seqLength < MAX_STEPS)
      sequence[seqLength++] = 60 + noteIndex + (octaveOffset * 12);
  }

  if(mode == READ && keyHeld)
    transpoKeyOffset = noteOffsets[noteIndex];

  if((mode == IDLE || mode == REC || mode == PAUSED) && keyHeld){
    current_freq = mtof(60 + noteIndex + (octaveOffset * 12));
    if(waveform == 3){
      grainNoteIndex = noteIndex;
      updateGrainPeriod();
    }
  }

  // --- FREQUENCE DIRECTE ---
  setAllOscFreq(current_freq);

  // --- MODE READ — tempo interne ---
  if(mode == READ && seqLength > 0){
    if(swingPending && (now - swingTriggerTime >= (unsigned long)swingDelayMs)){
      swingPending = false;
      triggerStep();
    }
    if(!swingPending && (now - lastStepTime >= (unsigned long)tempoMs)){
      lastStepTime = now;
      swingStep    = !swingStep;
      if(swingStep && swingDelayMs > 0){ swingPending = true; swingTriggerTime = now; }
      else triggerStep();
    }
  }

  // --- ENVELOPPE ---
  if(mode == READ){
    if(gate){
      env += attackRate;
      if(env >= 1.0f){ env = 1.0f; gate = false; }
    } else {
      env -= releaseRate;
      if(env < 0.0f) env = 0.0f;
    }
  } else {
    if(keyHeld){ env += attackRate; if(env > 1.0f) env = 1.0f; }
    else        { env -= releaseRate; if(env < 0.0f) env = 0.0f; }
  }

  lastKeyHeld = keyHeld;
}


// ==========================
AudioOutput updateAudio(){

  if(env <= 0.001f){
    return MonoOutput::fromAlmostNBit(9, 0);
  }

  int raw;

  switch(waveform){

    case 0: {
      char asig0 = aSin.next();

      byte asig1 = (byte)128 + ((asig0 * ((byte)128 + aGain1.next())) >> 8);
      char ws1   = aCheby3rd.next(asig1);

      byte asig2 = (byte)128 + ((asig0 * ((byte)128 + aGain2.next())) >> 8);
      char ws2   = aCheby6th.next(asig2);

      int mixed = (((int)ws1 * (255 - chebyMix)) + ((int)ws2 * chebyMix)) >> 8;

      raw = aCompress.next(256 + mixed);
      break;
    }

    case 1: {
      int s1 = -(int)oSaw1.next();
      int s2 = -(int)oSaw2.next();
      raw    = (s1 + s2) >> 1;
      break;
    }

    case 2: {
      int8_t s = oPulse.next();
      raw      = (s > pwmThreshold) ? 96 : -96;
      break;
    }

    case 3: {
      grainPhase++;

      if(grainPhase >= grainNextTrigger){
        grainPhase = 0;

        uint16_t jit = (grainJitter > 0) ? (fastRand() % grainJitter) : 0;
        if(jit & 1) grainNextTrigger = (grainPeriod > jit) ? grainPeriod - jit : grainPeriod;
        else        grainNextTrigger = grainPeriod + jit;
        if(grainNextTrigger < 50) grainNextTrigger = 50;

        triggerGrain();
      }

      if(grainActive){
        if(grainEnvPhase < grainWindow){

          if(grainFreqDrift != 0.0f){
            grainFreqCurrent += grainFreqDrift;
            if(grainFreqCurrent < 20.0f) grainFreqCurrent = 20.0f;
            oGrain.setFreq(grainFreqCurrent);
          }

          unsigned int half     = grainWindow >> 1;
          unsigned int num      = (unsigned int)grainEnvPhase * (grainWindow - grainEnvPhase);
          unsigned int denom    = (half * half) >> 7;
          int8_t       grainEnv = (denom > 0) ? (int8_t)(num / denom) : 0;
          if(grainEnv > 127) grainEnv = 127;
          int8_t sig = oGrain.next();
          raw        = ((int)sig * (int)grainEnv) >> 7;
          grainEnvPhase++;
        } else {
          grainActive = false;
          raw         = 0;
        }
      } else {
        raw = 0;
      }
      break;
    }

    case 4: {
      int sFund  = -(int)oFund.next();
      int sFifth = -(int)oFifth.next();
      raw = (((int)sFund * (255 - fifthMix)) + ((int)sFifth * fifthMix)) >> 8;
      break;
    }

    default:
      raw = 0;
      break;
  }

  int out = (int)(raw * env) >> 1;

  return MonoOutput::fromAlmostNBit(9, out);
}


// ==========================
void loop(){
  audioHook();
}
