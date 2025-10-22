#include <NeopixelCommander.h>

NeopixelCommander neopixelCommander("HomeSSID", "MySecretPass", 5, 64);

void setup() {
  Serial.begin(115200);
  neopixelCommander.begin();
}

void loop() {
  neopixelCommander.loop();
}
