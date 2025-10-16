#include <NeopixelWSController.h>

NeopixelWSController lights("HomeSSID", "MySecretPass", 5, 16);

void setup() {
  lights.setConnectTimeout(10000); // try STA for 10 s
  lights.begin();
}

void loop() {
  lights.loop();
}
