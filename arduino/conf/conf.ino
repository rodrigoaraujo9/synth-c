
#define PACKET_START 0xAA
#define PACKET_END 0x55

typedef struct __attribute__((packed)) {
  uint8_t start;
  int32_t potentiometer;
  int32_t joystick_x;
  int32_t joystick_y;
  int32_t ultrasonic;
  int32_t first_note; // can change
  int32_t second_note;
  int32_t third_note;
  int32_t fourth_note;
  uint8_t end;
} Packet;

Packet conf;

const int first_note_pin  = 2;
const int second_note_pin = 3;
const int third_note_pin  = 4;
const int fourth_note_pin = 5;

const int pot_pin = A3; // potenciometer
const int x_pin   = A8;
const int y_pin   = A9;
const int trigPin = 9;
const int echoPin = 10;

float duration, distance;

void setup() {
  pinMode(trigPin,         OUTPUT);
  pinMode(echoPin,         INPUT);
  pinMode(first_note_pin,  INPUT);
  pinMode(second_note_pin, INPUT);
  pinMode(third_note_pin,  INPUT);
  pinMode(fourth_note_pin, INPUT);

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

  conf.start           = PACKET_START;
  conf.potentiometer   = analogRead(pot_pin);
  conf.joystick_x      = analogRead(x_pin);
  conf.joystick_y      = analogRead(y_pin);
  conf.ultrasonic      = distance;
  conf.first_note_up   = analogRead(first_note_pin);
  conf.second_note_pin = analogRead(second_note_pin);
  conf.third_note_pin  = analogRead(third_note_pin);
  conf.fourth_note_pin = analogRead(fourth_note_pin);
  conf.end             = PACKET_END;

  Serial.write((uint8_t *)&conf, sizeof(conf));
  delay(5);
}

/*
#include <Keypad.h>

const byte ROWS = 4;
const byte COLS = 4;

char hexaKeys[ROWS][COLS] = {
  {'1', '2', '3', 'A'},
  {'4', '5', '6', 'B'},
  {'7', '8', '9', 'C'},
  {'*', '0', '#', 'D'}
};

byte rowPins[ROWS] = {9, 8, 7, 6};
byte colPins[COLS] = {5, 4, 3, 2};

Keypad customKeypad = Keypad(makeKeymap(hexaKeys), rowPins, colPins, ROWS,
COLS);

void setup(){
  Serial.begin(9600);
}

void loop(){
  char customKey = customKeypad.getKey();

  if (customKey){
    Serial.println(customKey);
  }
}*/
