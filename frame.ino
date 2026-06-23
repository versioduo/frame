#include "Movie.h"
#include "Pictures.h"
#include <V2Base.h>
#include <V2Buttons.h>
#include <V2Device.h>
#include <V2LED.h>
#include <V2MIDI.h>
#include <V2Music.h>

V2DEVICE_METADATA("com.versioduo.frame", 8, "versioduo:samd:strip");

static V2LED::WS2812 LED(2, PIN_LED_WS2812, &sercom2, SPI_PAD_0_SCK_1, PIO_SERCOM);
static V2LED::WS2812 LEDExt(256, PIN_LED_WS2812_EXT, &sercom1, SPI_PAD_0_SCK_1, PIO_SERCOM);

// Config, written to EEPROM.
static constexpr struct Configuration {
  struct {
    uint8_t orientation{};
    bool    mirror{};
    float   power{0.5};
  } led;

  struct {
    uint32_t sleepSec{30};
    float    background{};
  } play;
} ConfigurationDefault{};

static class Device : public V2Device {
public:
  Device() : V2Device() {
    metadata.vendor      = "Versio Duo";
    metadata.product     = "V2 frame";
    metadata.description = "16 x 16 LED Matrix Display";
    metadata.home        = "https://versioduo.com/#frame";

    system.download  = "https://versioduo.com/download";
    system.configure = "https://versioduo.com/configure";

    // https://github.com/versioduo/arduino-board-package/blob/main/boards.txt
    usb.pid            = 0xe9c0;
    usb.ports.standard = 0;

    configuration = {.size{sizeof(config)}, .data{&config}};
  }

  Configuration config{ConfigurationDefault};

  void stop() {
    _state = State::Stop;
  }

  void playMovie() {
    _state = State::Movie;
  }

  void showPicture(uint8_t index) {
    const uint16_t xStart = index * 16 * 4;

    for (uint8_t y = 0; y < 16; y++) {
      const uint16_t yStart = y * nPictures * 16 * 4;

      for (uint8_t x = 0; x < 16; x++) {
        const uint8_t* pixel      = &Pictures[yStart + xStart + (x * 4)];
        const float    alpha      = (float)pixel[3] / 255.f;
        const uint8_t  background = (255.f * config.play.background) * (1.f - alpha);
        const uint8_t  r          = ((float)pixel[0] * alpha) + background;
        const uint8_t  g          = ((float)pixel[1] * alpha) + background;
        const uint8_t  b          = ((float)pixel[2] * alpha) + background;
        setLED(x, y, r, g, b);
      }
    }
  }

private:
  enum class CC {
    Manual = V2MIDI::CC::ModulationWheel,
  };

  enum class State { Sleep, Pictures, Clip, Movie, Stop } _state{};
  uint32_t _usec{};

  struct {
    bool     blend{};
    uint16_t oldFrame{};
    uint16_t frame{};
    bool     reverse{};
  } _movie;

  struct {
    uint16_t sequence[3]{};
    uint16_t next{};
  } _pictures;

  uint8_t _manual{};

  void handleReset() override {
    _state    = {};
    _usec     = V2Base::getUsec();
    _movie    = {};
    _pictures = {};
    _manual   = 0;

    LED.reset();
    LED.setHSV(V2Colour::Orange, 0.8, 0.15);
    LEDExt.reset();
    setMaxBrightness();
  }

  void setLED(uint8_t x, uint8_t y, uint8_t r, uint8_t g, uint8_t b) {
    if (config.led.mirror)
      x = 15 - x;

    switch (config.led.orientation) {
      case 1: {
        const uint8_t t = x;
        x               = 15 - y;
        y               = t;
      } break;

      case 2:
        x = 15 - x;
        y = 15 - y;
        break;

      case 3: {
        const uint8_t t = x;
        x               = y;
        y               = 15 - t;
      } break;
    }

    // The LEDs are in a zigzag pattern.
    const uint8_t xReversed = (y % 2 == 0) ? x : 15 - x;
    LEDExt.setRGB(y * 16 + xReversed, r, (float)g * 0.9f, (float)b * 0.8f);
  }

  // Return fraction, it does not reach 1.
  float getRandom() {
    const uint32_t random = V2Base::Cryptography::Random::read();
    return (float)(random - 1) / (float)UINT32_MAX;
  }

  void handleLoop() override {
    switch (_state) {
      case State::Sleep:
        if (V2Base::getUsecSince(_usec) < config.play.sleepSec * 1000 * 1000)
          return;

        if (getRandom() < 0.95f) {
          // Randomly select three unique pictures as a sequence.
          _state                = State::Pictures;
          _pictures.sequence[0] = getRandom() * nPictures;

          for (;;) {
            _pictures.sequence[1] = getRandom() * nPictures;
            if (_pictures.sequence[1] != _pictures.sequence[0])
              break;
          }

          for (;;) {
            _pictures.sequence[2] = getRandom() * nPictures;

            if (_pictures.sequence[2] == _pictures.sequence[0])
              continue;

            if (_pictures.sequence[2] == _pictures.sequence[1])
              continue;

            break;
          }

          // Show 2 or 3 of the pictures.
          _pictures.next = getRandom() < 0.8f ? 0 : 1;

        } else {
          _state = State::Clip;
        }
        break;

      case State::Pictures:
        if (V2Base::getUsecSince(_usec) < 1500 * 1000)
          return;

        if (_pictures.next == V2Base::countof(_pictures.sequence)) {
          LEDExt.reset();
          _state = State::Sleep;
          break;
        }

        showPicture(_pictures.sequence[_pictures.next]);
        _pictures.next++;
        break;

      case State::Clip:
        if (V2Base::getUsecSince(_usec) < 150 * 1000)
          return;

        if (playMovieFrame())
          break;

        LEDExt.reset();
        _state = State::Sleep;
        break;

      case State::Movie:
        if (V2Base::getUsecSince(_usec) < 150 * 1000)
          return;

        playMovieFrame();
        break;

      case State::Stop:
        return;
    }

    _usec = V2Base::getUsec();
  }

  bool playMovieFrame() {
    for (uint8_t y = 0; y < 16; y++) {
      for (uint8_t x = 0; x < 16; x++) {
        const uint8_t* pixel = &Movie[_movie.frame][(y * 16 * 3) + (x * 3)];

        if (_movie.blend) {
          const uint8_t* oldPixel = &Movie[_movie.oldFrame][(y * 16 * 3) + (x * 3)];
          const uint8_t  r        = (pixel[0] + oldPixel[0]) / 2;
          const uint8_t  g        = (pixel[1] + oldPixel[1]) / 2;
          const uint8_t  b        = (pixel[2] + oldPixel[2]) / 2;
          setLED(x, y, r, g, b);
          _movie.blend = false;

        } else {
          setLED(x, y, pixel[0], pixel[1], pixel[2]);
          _movie.blend = true;
        }
      }
    }

    if (_movie.blend)
      return true;

    if (_movie.frame == V2Base::countof(Movie) - 1) {
      _movie.reverse = true;
    }

    else if (_movie.frame == 0) {
      _movie.reverse = false;
    }

    _movie.oldFrame = _movie.frame;
    _movie.frame += _movie.reverse ? -1 : 1;

    return _movie.frame > 0;
  }

  void handleSystemReset() override {
    reset();
  }

  void handleControlChange(uint8_t channel, uint8_t controller, uint8_t value) override {
    if (channel != 0)
      return;

    switch (controller) {
      case (uint8_t)CC::Manual:
        switch (value) {
          case 0:
            _manual = value;
            reset();
            break;

          case 1:
            _manual = value;
            playMovie();
            break;

          case 2 ... nPictures + 1:
            _manual = value;
            stop();
            showPicture(_manual - 2);
            break;
        }
        break;

      case V2MIDI::CC::AllSoundOff:
      case V2MIDI::CC::AllNotesOff:
        reset();
    }
  }

  void setMaxBrightness() {
    const float min   = 0.05;
    const float max   = 0.5;
    const float range = (max - min) * config.led.power;
    LEDExt.setMaxBrightness(min + range);
  }

  void exportInput(JsonObject json) override {
    JsonArray jsonControllers = json["controllers"].to<JsonArray>();
    {
      JsonObject jsonController = jsonControllers.add<JsonObject>();
      jsonController["name"]    = "Picture";
      jsonController["number"]  = (uint8_t)CC::Manual;
      jsonController["value"]   = _manual;
      jsonController["max"]     = nPictures + 1;
    }
  }

  void exportSettings(JsonArray json) override {
    {
      JsonObject setting = json.add<JsonObject>();
      setting["title"]   = "LED";
      setting["type"]    = "number";
      setting["label"]   = "Power";
      setting["min"]     = 0;
      setting["max"]     = 1;
      setting["step"]    = 0.01;
      setting["default"] = ConfigurationDefault.led.power;
      setting["path"]    = "led/power";
    }
    {
      JsonObject setting = json.add<JsonObject>();
      setting["type"]    = "toggle";
      setting["label"]   = "Mirror";
      setting["path"]    = "led/mirror";
    }
    {
      JsonObject setting = json.add<JsonObject>();
      setting["type"]    = "number";
      setting["label"]   = "Orientation";
      setting["min"]     = 0;
      setting["max"]     = 3;
      setting["input"]   = "select";
      setting["default"] = ConfigurationDefault.led.orientation;
      setting["path"]    = "led/orientation";
      JsonArray names    = setting["names"].to<JsonArray>();
      names.add("0°");
      names.add("90°");
      names.add("180°");
      names.add("270°");
    }
    {
      JsonObject setting = json.add<JsonObject>();
      setting["title"]   = "Pictures";
      setting["type"]    = "number";
      setting["label"]   = "Sleep";
      setting["text"]    = "Seconds";
      setting["min"]     = 10;
      setting["max"]     = 1000;
      setting["step"]    = 10;
      setting["default"] = ConfigurationDefault.play.sleepSec;
      setting["path"]    = "play/sleep";
    }
    {
      JsonObject setting = json.add<JsonObject>();
      setting["type"]    = "number";
      setting["label"]   = "Canvas";
      setting["text"]    = "Brightness";
      setting["max"]     = 1;
      setting["step"]    = 0.01;
      setting["default"] = ConfigurationDefault.play.background;
      setting["path"]    = "play/background";
    }
  }

  void importConfiguration(JsonObject json) override {
    if (!json["led"].isNull()) {
      JsonObject led = json["led"];

      if (!led["orientation"].isNull()) {
        config.led.orientation = led["orientation"];
        if (config.led.orientation > 3)
          config.led.orientation = 3;
      }

      if (!led["mirror"].isNull())
        config.led.mirror = led["mirror"];

      if (!led["power"].isNull()) {
        config.led.power = led["power"];
        if (config.led.power < 0.f)
          config.led.power = 0;

        if (config.led.power > 1.f)
          config.led.power = 1;
      }
    }

    if (!json["play"].isNull()) {
      JsonObject play = json["play"];

      if (!play["sleep"].isNull()) {
        config.play.sleepSec = play["sleep"];
        if (config.play.sleepSec < 10)
          config.play.sleepSec = 10;

        if (config.play.sleepSec > 1000)
          config.play.sleepSec = 1000;
      }

      if (!play["background"].isNull()) {
        config.play.background = play["background"];
        if (config.play.background < 0.f)
          config.play.background = 0;

        if (config.play.background > 1.f)
          config.play.background = 1;
      }
    }

    setMaxBrightness();
  }

  void exportConfiguration(JsonObject json) override {
    JsonObject jsonLED      = json["led"].to<JsonObject>();
    jsonLED["#orientation"] = "Rotate the picture in 90 degree steps (0..3)";
    jsonLED["orientation"]  = config.led.orientation;
    jsonLED["#mirror"]      = "Mirror the picture";
    jsonLED["mirror"]       = config.led.mirror;
    jsonLED["#power"]       = "The maximum brightness of the LEDs (0..1)";
    jsonLED["power"]        = serialized(String(config.led.power, 2));

    JsonObject jsonPlay = json["play"].to<JsonObject>();
    jsonPlay["#sleep"]  = "The number of seconds between the pictures (10..1000)";
    jsonPlay["sleep"]   = config.play.sleepSec;

    jsonPlay["#background"] = "The brightness of the background (0..1)";
    jsonPlay["background"]  = config.play.background;
  }
} Device;

// Dispatch MIDI packets
static class MIDI {
public:
  void loop() {
    if (!Device.usb.midi.receive(_midi))
      return;

    if (_midi.port != 0)
      return;

    Device.dispatch(&Device.usb.midi, &_midi);
  }

private:
  V2MIDI::Packet _midi;
} MIDI;

static class Button : public V2Buttons::Button {
public:
  Button() : V2Buttons::Button(&_config, PIN_BUTTON) {}

private:
  const V2Buttons::Config _config{.clickUsec{200 * 1000}, .holdUsec{500 * 1000}};

  void handleClick(uint8_t count) override {
    Device.reset();
  }

  void handleHold(uint8_t count) override {
    switch (count) {
      case 0:
        Device.stop();
        LED.setHSV(V2Colour::Cyan, 0.8, 0.15);
        LEDExt.rainbow(1, 3, 0.75);
        break;

      case 1:
        Device.playMovie();
        break;

      case 2 ... nPictures + 1:
        Device.stop();
        Device.showPicture(count - 2);
        break;
    }
  }
} Button;

void setup() {
  Serial.begin(9600);

  LED.begin();
  LED.setMaxBrightness(0.5);
  LEDExt.begin();

  Button.begin();
  Device.begin();
  Device.reset();
}

void loop() {
  LED.loop();
  LEDExt.loop();
  MIDI.loop();
  V2Buttons::loop();
  Device.loop();

  if (Device.idle())
    Device.sleep();
}
