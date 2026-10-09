/*
  I2C scanner - checks that the INA219 is wired correctly
  --------------------------------------------------------
  Wiring : INA219 VCC -> ESP32 3V3, GND -> GND, SDA -> D21, SCL -> D22
  Use    : upload, open Serial Monitor at 115200 baud
  Result : "Found 0x40" = INA219 OK
           "--- scan done ---" with nothing found = check the four wires
*/

#include <Wire.h>

void setup() {
  Serial.begin(115200);
  Wire.begin(21, 22);   // SDA = D21, SCL = D22
}

void loop() {
  for (byte address = 1; address < 127; address++) {
    Wire.beginTransmission(address);
    if (Wire.endTransmission() == 0) {
      Serial.print("Found 0x");
      Serial.println(address, HEX);
    }
  }
  Serial.println("--- scan done ---");
  delay(2000);
}
