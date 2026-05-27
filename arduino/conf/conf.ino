#include <stdint.h>
#define PACKET_START 0xAA
#define PACKET_END 0x55

typedef struct __attribute__((packed)) {
  uint8_t start;
  int16_t potentiometers[5]; // LFO depth | Attack | Decay | Sustain | Release
  int16_t joystick[2];       // x | y
  float ultrasonic;
  uint8_t buttons[5];
  uint8_t checksum;
  uint8_t end;

} Packet;

Packet conf;

const int buttonPins[5] = {49, 3, 4, 5, 6};

const int potPins[5] = {A15, A11, A12, A13, A14};

const int x_pin = A0;
const int y_pin = A1;

const int trigPin = 52;
const int echoPin = 53;

float duration, distance;

uint8_t last_button_state[5];            // LOW | HIGH
unsigned long last_debounce_time[5];     // milliseconds
const unsigned long debounce_delay = 50; // milliseconds

uint8_t checksum(Packet *p) {

  uint8_t *data = (uint8_t *)p;

  uint8_t sum = 0;

  // checksum and end byte are excluded
  for (size_t i = 0; i < sizeof(Packet) - 2; i++) {
    sum ^= data[i];
  }

  return sum;
}

void setup() {
  pinMode(trigPin, OUTPUT);
  pinMode(echoPin, INPUT);

  for (int i = 0; i < 5; i++) {
    pinMode(buttonPins[i], INPUT_PULLUP);
    last_button_state[i] = LOW;
    last_debounce_time[i] = 0;
    conf.buttons[i] = 0;
  }

  Serial.begin(9600);
}

void loop() {

  digitalWrite(trigPin, LOW);
  delayMicroseconds(2);

  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);

  digitalWrite(trigPin, LOW);

  duration = pulseIn(echoPin, HIGH);
  distance = (duration * .0343) / 2;

  conf.start = PACKET_START;

  for (int i = 0; i < 5; i++) {
    conf.potentiometers[i] = analogRead(potPins[i]);
  }

  conf.joystick[0] = analogRead(x_pin);
  conf.joystick[1] = analogRead(y_pin);

  conf.ultrasonic = distance;

  for (int i = 0; i < 5; i++) {
    uint8_t reading = !digitalRead(buttonPins[i]);

    if (reading != last_button_state[i]) {
      last_debounce_time[i] = millis();
    }

    if (((millis() - last_debounce_time[i]) > debounce_delay) &&
        reading != conf.buttons[i]) {

      conf.buttons[i] = reading;
    }

    last_button_state[i] = reading;
  }

  conf.checksum = checksum(&conf);

  conf.end = PACKET_END;

  Serial.write((uint8_t *)&conf, sizeof(conf));
  delay(5);
}
