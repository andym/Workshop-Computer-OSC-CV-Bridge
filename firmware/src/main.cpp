#include "ComputerCard.h"
#include "ComputerCardExtensions.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"

// ---------------------------------------------------------------------------
// Channel mapping
// ---------------------------------------------------------------------------
//
// Outputs (host → device, 0xC0 packet):
//   ch1 / target[0] → Audio Out 1  (SPI DAC, 12-bit, 48kHz — best for LFO)
//   ch2 / target[1] → Audio Out 2  (SPI DAC, 12-bit, 48kHz)
//   ch3 / target[2] → CV Out 1     (PWM, 11-bit, MIDI-calibrated)
//   ch4 / target[3] → CV Out 2     (PWM, 11-bit)
//   /pulse/1 / flags bit 0 → Pulse Out 1  (GPIO, digital)
//   /pulse/2 / flags bit 1 → Pulse Out 2  (GPIO, digital)
//
// Inputs (device → host, 0xC1 packet, 16 bytes):
//   Byte 0:      0xC1 sync
//   Byte 1:      flags — bit 0: pulse1, bit 1: pulse2, bits 2-3: switch (0/1/2)
//   Bytes 2-5:   int16_t[2]  CV In 1-2      (-2048..+2047)  → /ch/3-4
//   Bytes 6-9:   int16_t[2]  Audio In 1-2   (-2048..+2047)  → /ch/1-2
//   Bytes 10-15: int16_t[3]  Main, X, Y knobs (0-4095)
//   (Python remaps so inputs go 1-2-3-4 top-to-bottom: audio, CV)
//
// All values are ComputerCard native range: -2048 to +2047
// (approx -6V to +6V, 12V span). Voltage conversion is done in Python.

// ---------------------------------------------------------------------------
// Shared state between cores
// ---------------------------------------------------------------------------

// Output targets: written by core 0 (USB reader), read by core 1 (audio ISR)
static volatile int16_t target[4] = {0, 0, 0, 0};
static volatile uint8_t target_flags = 0; // bit 0: pulse out 1, bit 1: pulse out 2

// Input state: written by core 1 (audio ISR), read by core 0 (USB writer)
static volatile int16_t input_cv[2] = {0, 0};
static volatile int16_t input_audio[2] = {0, 0};
static volatile int16_t input_knobs[3] = {0, 0, 0}; // Main, X, Y (0-4095)
static volatile uint8_t input_flags = 0;
static volatile bool inputs_ready = false;

// Input reporting rate in samples (48000 = 1Hz, 480 = 100Hz)
static constexpr int INPUT_REPORT_INTERVAL = 48; // 1000Hz

// ---------------------------------------------------------------------------
// Binary protocol constants
// ---------------------------------------------------------------------------

static constexpr uint8_t SYNC_HOST_TO_DEVICE = 0xC0;
static constexpr uint8_t SYNC_DEVICE_TO_HOST = 0xC1;
static constexpr int OUTPUT_PACKET_SIZE = 10; // host → device
static constexpr int INPUT_PACKET_SIZE = 16;  // device → host

// ---------------------------------------------------------------------------
// Startup pattern: cascade down then "bridge locked" — suggests data flowing
// through a relay/proxy.
//
// LED grid:     0 1       cascade: top → mid → bottom → all on
//               2 3
//               4 5
// ---------------------------------------------------------------------------

// clang-format off
static constexpr uint8_t kTop[6]  = {1,1,0,0,0,0};
static constexpr uint8_t kMid[6]  = {0,0,1,1,0,0};
static constexpr uint8_t kBot[6]  = {0,0,0,0,1,1};
static constexpr uint8_t kAll[6]  = {1,1,1,1,1,1};
static constexpr uint8_t kOff[6]  = {0,0,0,0,0,0};

static const CardExtensions::StartupPatterns::Pattern kBridgePattern = {
    {   // Two cascades (data flowing through) then "locked" glow
        kTop, kMid, kBot,           // cascade 1
        kTop, kMid, kBot,           // cascade 2
        kAll, kAll, kAll, kAll,     // bridge established
        kOff, kOff                  // fade out
    },
    "Bridge",
    "OSC-CV bridge — data flowing through"
};
// clang-format on

// ---------------------------------------------------------------------------
// Core 1: Audio processing (48kHz)
// ---------------------------------------------------------------------------
//
// LED layout mirrors channel mapping (top 4 LEDs):
//   LED 0 = ch1 (Audio Out 1)    LED 1 = ch2 (Audio Out 2)
//   LED 2 = ch3 (CV Out 1)       LED 3 = ch4 (CV Out 2)
// Brightness tracks |output voltage|. LEDs 4-5 unused.

class OSCBridge : public CardExtensions::ExtendedCard {
private:
  int reportCounter = 0;

protected:
  const CardExtensions::StartupPatterns::Pattern &GetStartupPattern() override {
    return kBridgePattern;
  }

  void __not_in_flash_func(ProcessMainSample)() override {
    // Apply target values to outputs — pure integer, no scaling
    AudioOut1(target[0]);
    AudioOut2(target[1]);
    CVOut1(target[2]);
    CVOut2(target[3]);
    uint8_t f = target_flags;
    PulseOut1(f & 0x01);
    PulseOut2((f & 0x02) != 0);

    // Per-channel activity LEDs: brightness tracks |voltage|
    // target range is -2048..+2047, LED brightness is 0..4095
    for (int i = 0; i < 4; i++) {
      int32_t v = target[i];
      if (v < 0)
        v = -v;
      LedBrightness(i, (uint16_t)(v * 2));
    }

    // Sample inputs at configured rate
    reportCounter++;
    if (reportCounter >= INPUT_REPORT_INTERVAL) {
      reportCounter = 0;

      input_cv[0] = Connected(CV1) ? CVIn1() : 0;
      input_cv[1] = Connected(CV2) ? CVIn2() : 0;
      input_audio[0] = Connected(Audio1) ? AudioIn1() : 0;
      input_audio[1] = Connected(Audio2) ? AudioIn2() : 0;
      input_knobs[0] = (int16_t)KnobVal(Main);
      input_knobs[1] = (int16_t)KnobVal(X);
      input_knobs[2] = (int16_t)KnobVal(Y);
      uint8_t p1 = Connected(Pulse1) ? (PulseIn1() ? 0x01 : 0x00) : 0x00;
      uint8_t p2 = Connected(Pulse2) ? (PulseIn2() ? 0x02 : 0x00) : 0x00;
      input_flags = p1 | p2 | ((uint8_t)SwitchVal() << 2);
      inputs_ready = true;
    }
  }

public:
  OSCBridge() { EnableNormalisationProbe(); }
};

// Pointer for core 1 to access the bridge instance constructed in main().
static OSCBridge *bridge_ptr = nullptr;

// Core 1 entry: runs audio pipeline (blocks forever)
static void core1_audio_entry() { bridge_ptr->RunWithBootSupport(); }

// ---------------------------------------------------------------------------
// Core 0: USB CDC reader/writer (main thread)
// ---------------------------------------------------------------------------
// stdio_init_all() registers the TinyUSB background task on core 0,
// so USB reading MUST happen on core 0 — getchar_timeout_us() on
// another core silently gets nothing.

static void __not_in_flash_func(usb_loop)() {
  uint8_t pktBuf[OUTPUT_PACKET_SIZE];
  uint8_t pktPos = 0;

  while (true) {
    // --- Read incoming packets from host ---
    int c = getchar_timeout_us(100);
    if (c != PICO_ERROR_TIMEOUT) {
      uint8_t b = (uint8_t)c;

      // Resync: if we're mid-packet and see a sync byte, restart
      if (pktPos > 0 && b == SYNC_HOST_TO_DEVICE) {
        pktBuf[0] = b;
        pktPos = 1;
        continue;
      }

      // Wait for sync byte to start a packet
      if (pktPos == 0 && b != SYNC_HOST_TO_DEVICE) {
        continue;
      }

      pktBuf[pktPos++] = b;

      if (pktPos == OUTPUT_PACKET_SIZE) {
        // Complete packet — copy to targets
        target_flags = pktBuf[1];
        target[0] = (int16_t)(pktBuf[2] | (pktBuf[3] << 8));
        target[1] = (int16_t)(pktBuf[4] | (pktBuf[5] << 8));
        target[2] = (int16_t)(pktBuf[6] | (pktBuf[7] << 8));
        target[3] = (int16_t)(pktBuf[8] | (pktBuf[9] << 8));
        pktPos = 0;
      }
    }

    // --- Send input packets to host (16 bytes) ---
    if (inputs_ready) {
      inputs_ready = false;

      // Snapshot volatile values
      uint8_t flags = input_flags;
      int16_t cv0 = input_cv[0];
      int16_t cv1 = input_cv[1];
      int16_t audio0 = input_audio[0];
      int16_t audio1 = input_audio[1];
      int16_t knob0 = input_knobs[0];
      int16_t knob1 = input_knobs[1];
      int16_t knob2 = input_knobs[2];

      uint8_t outPkt[INPUT_PACKET_SIZE];
      outPkt[0] = SYNC_DEVICE_TO_HOST;
      outPkt[1] = flags;
      outPkt[2] = (uint8_t)(cv0 & 0xFF);
      outPkt[3] = (uint8_t)((cv0 >> 8) & 0xFF);
      outPkt[4] = (uint8_t)(cv1 & 0xFF);
      outPkt[5] = (uint8_t)((cv1 >> 8) & 0xFF);
      outPkt[6] = (uint8_t)(audio0 & 0xFF);
      outPkt[7] = (uint8_t)((audio0 >> 8) & 0xFF);
      outPkt[8] = (uint8_t)(audio1 & 0xFF);
      outPkt[9] = (uint8_t)((audio1 >> 8) & 0xFF);
      outPkt[10] = (uint8_t)(knob0 & 0xFF);
      outPkt[11] = (uint8_t)((knob0 >> 8) & 0xFF);
      outPkt[12] = (uint8_t)(knob1 & 0xFF);
      outPkt[13] = (uint8_t)((knob1 >> 8) & 0xFF);
      outPkt[14] = (uint8_t)(knob2 & 0xFF);
      outPkt[15] = (uint8_t)((knob2 >> 8) & 0xFF);

      for (int i = 0; i < INPUT_PACKET_SIZE; i++) {
        putchar_raw(outPkt[i]);
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int main() {
  // OSCBridge must exist before core 1 launch since core 1 immediately
  // calls RunWithBootSupport() on it. Note: ComputerCard's constructor
  // also calls flash_get_unique_id() which disables XIP temporarily —
  // a hazard if the other core were running flash-resident code.
  static OSCBridge bridge;
  bridge_ptr = &bridge;

  stdio_init_all();

  // Launch audio on core 1 (DMA ISR will fire on core 1)
  multicore_launch_core1(core1_audio_entry);

  // Core 0: USB reader/writer (stdio lives here)
  usb_loop();

  return 0;
}
