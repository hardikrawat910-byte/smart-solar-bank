/*
  Blink test - checks that the ESP32 board, cable, driver and upload work
  -----------------------------------------------------------------------
  Use    : upload, open Serial Monitor at 115200 baud, press EN
  Result : blue LED blinks and "ESP32 is alive!" then LED ON / LED OFF
  Note   : some DevKit V1 clones have no blue LED on GPIO2 - judge by the
           Serial Monitor only on those boards.
*/

void setup() {
  Serial.begin(115200);
  pinMode(2, OUTPUT);                 // built-in blue LED on DevKit V1
  delay(500);
  Serial.println("ESP32 is alive!");
  Serial.print("Chip: ");
  Serial.println(ESP.getChipModel());
}

void loop() {
  digitalWrite(2, HIGH);
  Serial.println("LED ON");
  delay(500);
  digitalWrite(2, LOW);
  Serial.println("LED OFF");
  delay(500);
}
