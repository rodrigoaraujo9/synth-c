#include <stdint.h>
#define PACKET_START 0xAA
#define PACKET_END 0x55

typedef struct __attribute__((packed)) {
  uint8_t start;
  int32_t potentiometer;
  int32_t potentiometer_a;
  int32_t potentiometer_d;
  int32_t potentiometer_s;
  int32_t potentiometer_r;
  int32_t joystick_x;
  int32_t joystick_y;
  int32_t ultrasonic;
  int32_t button_1;
  int32_t button_2;
  int32_t button_3;
  int32_t button_4;
  int32_t button_5;
  uint8_t end;
} Packet;

Packet conf;

const int button_1_pin = 49;
const int button_2_pin = 3;
const int button_3_pin = 4;
const int button_4_pin = 5;
const int button_5_pin = 6;

const int pot_a_pin = A11;
const int pot_d_pin = A12;
const int pot_s_pin = A13;
const int pot_r_pin = A14;

const int pot_pin = A15;

const int x_pin = A0;
const int y_pin = A1;

const int trigPin = 52;
const int echoPin = 53;

float duration, distance;

void setup() {
  pinMode(trigPin, OUTPUT);
  pinMode(echoPin, INPUT);
  pinMode(button_1_pin, INPUT_PULLUP);
  pinMode(button_2_pin, INPUT);
  pinMode(button_3_pin, INPUT);
  pinMode(button_4_pin, INPUT);
  pinMode(button_5_pin, INPUT);

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

  conf.potentiometer = analogRead(pot_pin);

  conf.potentiometer_a = analogRead(pot_a_pin);
  conf.potentiometer_d = analogRead(pot_d_pin);
  conf.potentiometer_s = analogRead(pot_s_pin);
  conf.potentiometer_r = analogRead(pot_r_pin);

  conf.joystick_x = analogRead(x_pin);
  conf.joystick_y = analogRead(y_pin);

  conf.ultrasonic = distance;

  conf.button_1 = digitalRead(button_1_pin);
  conf.button_2 = digitalRead(button_2_pin);
  conf.button_3 = digitalRead(button_3_pin);
  conf.button_4 = digitalRead(button_4_pin);
  conf.button_5 = digitalRead(button_5_pin);

  conf.end = PACKET_END;

  Serial.write((uint8_t *)&conf, sizeof(conf));
  delay(5);
}
