#include <NeopixelCommander.h>

NeopixelCommander neopixelCommander("HomeSSID", "MySecretPass", 5, 64);

void setup() {
  neopixelCommander.setConnectTimeout(10000); // try STA for 10 s
  neopixelCommander.begin();
}

void loop() {
  neopixelCommander.loop();
}
