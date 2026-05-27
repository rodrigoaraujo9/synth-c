#include <stdint.h>
#define PACKET_START 0xAA
#define PACKET_END 0x55

typedef struct __attribute__((packed)) {
  uint8_t start;
  uint8_t waveform;
  int16_t potentiometers[5]; // LFO depth | Attack | Decay | Sustain | Release
  int16_t joystick[2];       // x | y
  float ultrasonic;
  uint8_t buttons[5];
  uint8_t checksum;
  uint8_t end;

} Packet;

Packet conf;

const int wave_buttonPin = 49;

const int buttonPins[5] = {2, 3, 4, 5, 6};

const int potPins[5] = {A15, A11, A12, A13, A14};

const int x_pin = A0;
const int y_pin = A1;

const int trigPin = 52;
const int echoPin = 53;

float duration, distance;

uint8_t last_button_state[5];            // LOW | HIGH
unsigned long last_debounce_time[5];     // milliseconds
const unsigned long debounce_delay = 50; // milliseconds

uint8_t wav_button_state;             // LOW | HIGH
uint8_t last_wav_button_state;        // LOW | HIGH
unsigned long last_wav_debounce_time; // milliseconds

uint8_t checksum(Packet *p) {

  uint8_t *data = (uint8_t *)p;

  uint8_t sum = 0;

  // checksum and end byte are excluded
  for (size_t i = 0; i < sizeof(Packet) - 2; i++) {
    sum ^= data[i];
  }

  return sum;
}

void toggle_waveform() { conf.waveform = (conf.waveform + 1) % 4; }

void setup() {
  pinMode(trigPin, OUTPUT);
  pinMode(echoPin, INPUT);

  pinMode(wave_buttonPin, INPUT_PULLUP);

  for (int i = 0; i < 5; i++) {
    pinMode(buttonPins[i], INPUT_PULLUP);
    last_button_state[i] = LOW;
    last_debounce_time[i] = 0;
    conf.buttons[i] = 0;
  }

  conf.waveform = 0;
  wav_button_state = LOW;
  last_wav_button_state = 0;
  last_wav_debounce_time = 0;

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

    if (((millis() - last_wav_debounce_time) > debounce_delay) &&
        reading != wav_button_state) {

      wav_button_state = reading;

      if (wav_button_state == HIGH) {
        toggle_waveform();
      }
    }

    last_button_state[i] = reading;
  }

  uint8_t reading = !digitalRead(wav_buttonPin);

  if (reading != last_wav_button_state) {
    last_wav_debounce_time = millis();
  }

  if (((millis() - last_wav_debounce_time) > debounce_delay) &&
      reading != wav_button_state) {

    toggle_waveform();
    wav_button_state = reading;
  }

  last_wav_button_state = reading;

  conf.checksum = checksum(&conf);

  conf.end = PACKET_END;

  Serial.write((uint8_t *)&conf, sizeof(conf));
  delay(5);
}
