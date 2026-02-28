#include <WiFiManager.h> // https://github.com/tzapu/WiFiManager
#include <LiquidCrystal_I2C.h>


LiquidCrystal_I2C lcd(0x3F,17,2);  // set the LCD address to 0x27 for a 16 chars and 2 line display

void setup() {
    Serial.begin(115200);
    
    WiFiManager wm;

    lcd.init();
    lcd.backlight();
    lcd.println("  Hello World   ");

    bool res;
    res = wm.autoConnect("AutoConnectAP","password");

    if(!res) {
        Serial.println("Failed to connect");
    } 
    else { 
        Serial.println("connected...yeey :)");
    }

}

void loop() {
    // put your main code here, to run repeatedly:   
}
