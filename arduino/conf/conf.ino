#include <Keypad.h>

struct dataDef {
    int pot_val;
} conf;

int pot_pin = A3; // potenciometer

void setup()
{
  Serial.begin(9600);
}

void loop()
{
    conf.pot_val = analogRead(pot_pin);
    byte *c_bytes = (byte*) &conf;

    for (int i = 0; i < sizeof(conf); i++) {
        Serial.write(c_bytes[i]);
    }
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

Keypad customKeypad = Keypad(makeKeymap(hexaKeys), rowPins, colPins, ROWS, COLS); 

void setup(){
  Serial.begin(9600);
}
  
void loop(){
  char customKey = customKeypad.getKey();
  
  if (customKey){
    Serial.println(customKey);
  }
}*/
