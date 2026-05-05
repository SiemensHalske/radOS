int pins[] = {20, 19, 0, 1, 18, 9, 6, 11, 12, 4, 5, 7, 10};
const int numPins = 13;
bool staticMatrix[numPins][numPins];

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  // Alle Pins auf Pullup
  for (int i = 0; i < numPins; i++) {
    pinMode(pins[i], INPUT_PULLUP);
  }

  Serial.println("--- Kalibrierung: Erfasse statisches PCB-Layout ---");
  // Wir lernen, welche Verbindungen 'normal' sind
  for (int i = 0; i < numPins; i++) {
    pinMode(pins[i], OUTPUT);
    digitalWrite(pins[i], LOW);
    delay(5);
    for (int j = 0; j < numPins; j++) {
      if (i != j && digitalRead(pins[j]) == LOW) {
        staticMatrix[i][j] = true;
      } else {
        staticMatrix[i][j] = false;
      }
    }
    pinMode(pins[i], INPUT_PULLUP);
  }
  Serial.println("Bereit! Bitte drücke jetzt nacheinander die Buttons.");
}

void loop() {
  for (int i = 0; i < numPins; i++) {
    pinMode(pins[i], OUTPUT);
    digitalWrite(pins[i], LOW);

    for (int j = 0; j < numPins; j++) {
      if (i == j) continue;

      bool currentState = (digitalRead(pins[j]) == LOW);
      
      // Wenn Verbindung da ist, die vorher NICHT da war -> Button-Event
      if (currentState && !staticMatrix[i][j]) {
        Serial.print("BUTTON TREFFER: GPIO ");
        Serial.print(pins[i]);
        Serial.print(" <-> GPIO ");
        Serial.println(pins[j]);
        delay(250); // Simples Entprellen für die Zuordnung
      }
    }
    pinMode(pins[i], INPUT_PULLUP);
  }
}