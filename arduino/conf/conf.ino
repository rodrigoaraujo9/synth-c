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
