#include <LiquidCrystal.h>
#include <stdint.h>

/*-----------------------------------------------------------------------------------------------------------*/

/* Types and Defines */

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

/*-----------------------------------------------------------------------------------------------------------*/

/* Wiring */

const int wave_buttonPin = 45;
const int buttonPins[5] = {4, 5, 6, 7, 8};

const int potPins[5] = {A15, A0, A1, A2, A3};

const int x_pin = A11;
const int y_pin = A12;

const int trigPin = 48;
const int echoPin = 49;

const int rs = 40, en = 41, d4 = 34, d5 = 35, d6 = 32, d7 = 33;

/*-----------------------------------------------------------------------------------------------------------*/

/* Constants */

const unsigned long debounce_delay = 5;
const unsigned long packet_interval = 5;
const unsigned long ultrasonic_timeout = 25000UL;

/*-----------------------------------------------------------------------------------------------------------*/

/* Globals */

Packet conf;

float duration = 0;
float distance = 0;

uint8_t button_state[5];
uint8_t last_button_reading[5];
unsigned long last_debounce_time[5];

uint8_t wav_button_state;
uint8_t last_wav_button_reading;
unsigned long last_wav_debounce_time;

unsigned long last_packet_time = 0;

LiquidCrystal lcd(rs, en, d4, d5, d6, d7);

/*-----------------------------------------------------------------------------------------------------------*/

/* Helper Functions */

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

void print_waveform() {
  switch (conf.waveform) {
  case 0:
    lcd.print("Sine");
    return;
  case 1:
    lcd.print("Square");
    return;
  case 2:
    lcd.print("Triangle");
    return;
  case 3:
    lcd.print("Sawtooth");
    return;
  default:
    lcd.print("Error");
    return;
  }
}

void update_lcd_waveform() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Waveform:");
  lcd.setCursor(0, 1);
  print_waveform();
}

void update_buttons() {
  for (int i = 0; i < 5; i++) {
    uint8_t reading = !digitalRead(buttonPins[i]);

    if (reading != last_button_reading[i]) {
      last_debounce_time[i] = millis();
      last_button_reading[i] = reading;
    }

    if ((millis() - last_debounce_time[i]) >= debounce_delay) {
      if (reading != button_state[i]) {
        button_state[i] = reading;
        conf.buttons[i] = button_state[i];
      }
    }
  }
}

void update_waveform_button() {
  uint8_t reading = !digitalRead(wave_buttonPin);

  if (reading != last_wav_button_reading) {
    last_wav_debounce_time = millis();
    last_wav_button_reading = reading;
  }

  if ((millis() - last_wav_debounce_time) >= debounce_delay) {
    if (reading != wav_button_state) {
      wav_button_state = reading;

      if (wav_button_state == HIGH) {
        toggle_waveform();
        update_lcd_waveform();
      }
    }
  }
}

void update_ultrasonic() {
  digitalWrite(trigPin, LOW);
  delayMicroseconds(2);

  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);

  digitalWrite(trigPin, LOW);

  duration = pulseIn(echoPin, HIGH, ultrasonic_timeout);

  if (duration > 0) {
    distance = (duration * 0.0343f) / 2.0f;
  }

  conf.ultrasonic = distance;
}

void send_packet() {
  conf.start = PACKET_START;
  conf.end = PACKET_END;

  for (int i = 0; i < 5; i++) {
    conf.potentiometers[i] = analogRead(potPins[i]);
  }

  conf.joystick[0] = analogRead(x_pin);
  conf.joystick[1] = analogRead(y_pin);

  update_ultrasonic();

  conf.checksum = checksum(&conf);

  Serial.write((uint8_t *)&conf, sizeof(conf));
}

/*-----------------------------------------------------------------------------------------------------------*/

/* Main Functions */

void setup() {
  pinMode(trigPin, OUTPUT);
  pinMode(echoPin, INPUT);

  pinMode(wave_buttonPin, INPUT_PULLUP);

  for (int i = 0; i < 5; i++) {
    pinMode(buttonPins[i], INPUT_PULLUP);

    button_state[i] = !digitalRead(buttonPins[i]);
    last_button_reading[i] = button_state[i];
    last_debounce_time[i] = 0;

    conf.buttons[i] = button_state[i];
  }

  conf.waveform = 0;

  wav_button_state = !digitalRead(wave_buttonPin);
  last_wav_button_reading = wav_button_state;
  last_wav_debounce_time = 0;

  conf.start = PACKET_START;
  conf.end = PACKET_END;

  lcd.begin(16, 2);
  lcd.display();
  update_lcd_waveform();

  Serial.begin(115200);
}

void loop() {
  update_buttons();
  update_waveform_button();

  unsigned long now = millis();

  if ((now - last_packet_time) >= packet_interval) {
    last_packet_time = now;
    send_packet();
  }
}
