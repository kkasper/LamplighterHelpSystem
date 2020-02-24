#include <FS.h>                   //this needs to be first, or it all crashes and burns...

#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <WiFiManager.h>          //https://github.com/tzapu/WiFiManager
#include <ArduinoJson.h>          //https://github.com/bblanchon/ArduinoJson
#include <PubSubClient.h>
#include <FastLED.h>
#include <OneButton.h>
#include <Ticker.h>
Ticker ticker;

//flag for saving data
bool shouldSaveConfig = false;

// Constants
#define HELP_BUTTON     5
#define PORTAL_BUTTON   14
#define LED_DATA_PIN    4
#define NUM_LEDS        1
#define mqtt_server       "142.93.207.70"
#define mqtt_port         "1883"
#define mqtt_user         "lamp"
#define mqtt_pass         "lamplighter"
#define help_topic        "requests"
#define cancel_topic      "cancels"


//For identifying who requested help. Change for each drive.
#define LAMP_ID         "Backroom"
#define LAMP_ID_NUM     0

//Setup buttons on Pin 5 (D1) and Pin 14 (D5)
OneButton button(HELP_BUTTON, true);  
OneButton portalButton(PORTAL_BUTTON, true);  


WiFiClient espClient;
PubSubClient client(espClient);
CRGB leds[NUM_LEDS];

//callback notifying us of the need to save config
void saveConfigCallback () {
  Serial.println("Should save config");
  shouldSaveConfig = true;
}

long helpStartedTime=0;
bool helpRequested = false;
bool helpComing = false;
int gBrightness = 10;
int gBrightnessDelta = 10;

void tick()
{
  if (gBrightness > 155) {
    gBrightnessDelta = -gBrightnessDelta;
  }
  else if (gBrightness < 5) {
    gBrightnessDelta = -gBrightnessDelta;
  }
  gBrightness += gBrightnessDelta;
  
  FastLED.setBrightness(gBrightness);
  FastLED.show();
}

//gets called when WiFiManager enters configuration mode
void configModeCallback (WiFiManager *myWiFiManager) {
  Serial.println("Entered config mode");
  Serial.println(WiFi.softAPIP());
  //if you used auto generated SSID, print it
  Serial.println(myWiFiManager->getConfigPortalSSID());
  //entered config mode, make led toggle faster
  leds[0] = CRGB::Blue;
  ticker.attach(0.1, tick);
}

void setup() {
  // put your setup code here, to run once:
  Serial.begin(115200);
  Serial.println();

  FastLED.addLeds<WS2811, LED_DATA_PIN, GRB>(leds, NUM_LEDS);
  leds[0] = CRGB::Black;
  FastLED.show();
  
  button.attachClick(cancelHelpRequest);
  button.attachLongPressStart(longPressStart);
  button.attachLongPressStop(callHelp);
  portalButton.attachClick(restartDevice);
  portalButton.attachLongPressStop(setupPortal);
  
  //clean FS for testing
//  SPIFFS.format();

  //read configuration from FS json
  Serial.println("mounting FS...");

  if (SPIFFS.begin()) {
    Serial.println("mounted file system");
    if (SPIFFS.exists("/config.json")) {
      //file exists, reading and loading
      Serial.println("reading config file");
      File configFile = SPIFFS.open("/config.json", "r");
      if (configFile) {
        Serial.println("opened config file");
        size_t size = configFile.size();
        // Allocate a buffer to store contents of the file.
        std::unique_ptr<char[]> buf(new char[size]);

        configFile.readBytes(buf.get(), size);
        //        DynamicJsonBuffer jsonBuffer;
        //        JsonObject& json = jsonBuffer.parseObject(buf.get());

        DynamicJsonDocument jsonDoc(1024);
        DeserializationError error = deserializeJson(jsonDoc, buf.get());
        if (error)
          return;

        serializeJson(jsonDoc, Serial);

        if (!error) {
          Serial.println("\nparsed json");
          strcpy(mqtt_server, jsonDoc["mqtt_server"]);
          strcpy(mqtt_port, jsonDoc["mqtt_port"]);
          strcpy(mqtt_user, jsonDoc["mqtt_user"]);
          strcpy(mqtt_pass, jsonDoc["mqtt_pass"]);

        } else {
          Serial.println("failed to load json config");
        }
      }
    }
  } else {
    Serial.println("failed to mount FS");
  }
  //end read



  // The extra parameters to be configured (can be either global or just in the setup)
  // After connecting, parameter.getValue() will get you the configured value
  // id/name placeholder/prompt default length
  WiFiManagerParameter custom_mqtt_server("server", "mqtt server", mqtt_server, 40);
  WiFiManagerParameter custom_mqtt_port("port", "mqtt port", mqtt_port, 6);
  WiFiManagerParameter custom_mqtt_user("user", "mqtt user", mqtt_user, 20);
  WiFiManagerParameter custom_mqtt_pass("pass", "mqtt pass", mqtt_pass, 20);

  //WiFiManager
  //Local intialization. Once its business is done, there is no need to keep it around
  WiFiManager wifiManager;

  // Reset Wifi settings for testing
//    wifiManager.resetSettings();

  //set config save notify callback
  wifiManager.setSaveConfigCallback(saveConfigCallback);
  wifiManager.setAPCallback(configModeCallback);

  //add all your parameters here
  wifiManager.addParameter(&custom_mqtt_server);
  wifiManager.addParameter(&custom_mqtt_port);
  wifiManager.addParameter(&custom_mqtt_user);
  wifiManager.addParameter(&custom_mqtt_pass);

  if (!wifiManager.autoConnect("Lamplighter Helpsys Configuration")) {
    Serial.println("failed to connect and hit timeout");
    delay(3000);
    //reset and try again, or maybe put it to deep sleep
    ESP.reset();
    delay(5000);
  }

  //if you get here you have connected to the WiFi
  Serial.println("connected...yeey :)");
  ticker.detach();

  flashColor(100, CRGB::Green);

  
  //read updated parameters
  strcpy(mqtt_server, custom_mqtt_server.getValue());
  strcpy(mqtt_port, custom_mqtt_port.getValue());
  strcpy(mqtt_user, custom_mqtt_user.getValue());
  strcpy(mqtt_pass, custom_mqtt_pass.getValue());

  //save the custom parameters to FS
  if (shouldSaveConfig) {
    Serial.println("saving config");
    DynamicJsonDocument jsonDoc(1024);

    jsonDoc["mqtt_server"] = mqtt_server;
    jsonDoc["mqtt_port"] = mqtt_port;
    jsonDoc["mqtt_user"] = mqtt_user;
    jsonDoc["mqtt_pass"] = mqtt_pass;
    serializeJson(jsonDoc, Serial);

    File configFile = SPIFFS.open("/config.json", "w");
    if (!configFile) {
      Serial.println("failed to open config file for writing");
    }

    serializeJson(jsonDoc, Serial);
    serializeJson(jsonDoc, configFile);
    configFile.close();
    //end save
  }

  Serial.print("Local ip: ");
  Serial.println(WiFi.localIP());
  long temp = atol(mqtt_port);
  unsigned int mqtt_port_x = (unsigned int)temp;
  client.setServer(mqtt_server, mqtt_port_x);
  client.setCallback(messageCallback);
  client.subscribe(help_topic);
  client.subscribe(cancel_topic);
}


void flashColor(unsigned int duration, CRGB color){   
    FastLED.setBrightness(255); 
    leds[0] = color;
    FastLED.show();
    delay(duration);
    leds[0] = CRGB::Black;
    FastLED.show();
    delay(duration);
    leds[0] = color;
    FastLED.show();
    delay(duration);
    leds[0] = CRGB::Black;
    FastLED.show();
}


void reconnect() {
  // Loop until we're reconnected
  while (!client.connected()) {
    Serial.print("Attempting MQTT connection...");
    if (client.connect("ESP8266Client", mqtt_user, mqtt_pass)) {
      Serial.println("Connected");
      client.subscribe(help_topic);
      client.subscribe(cancel_topic);
    } else {
      Serial.print("Failed, rc=");
      Serial.print(client.state());
      Serial.println(" try again in 5 seconds");
      // Wait 5 seconds before retrying
      delay(5000);
    }
  }
}


String messageCallback(char* topic, byte* payload, unsigned int length) {
  String message = "";
  for (int i = 0; i < length; i++) {
    message += (String)((char)payload[i]);
  }
  Serial.println();
  
  Serial.print("Message arrived: ");

  Serial.println(message);
}


// Functions for second button
void restartDevice() {
  Serial.println("Restart requested. Rebooting...");
  delay(100);
  ESP.restart();
}


void setupPortal() {
  Serial.println("On-demand Portal Requested. Launching...");
  WiFiManager wifiManager;
  wifiManager.setTimeout(120);
  wifiManager.setAPCallback(configModeCallback);
  if (!wifiManager.startConfigPortal("Lamplighter Helpsys Configuration")) {
    Serial.println("Failed to connect and hit timeout");
    delay(3000);
    //reset and try again, or maybe put it to deep sleep
    ESP.reset();
    delay(5000);
  }
  Serial.println("Connected...yeey :)");
  ESP.restart();
}


void cancelHelpRequest(){
  Serial.print("Button clicked: ");
  Serial.println("Help cancelled.");
  flashColor(100, CRGB::Red);
  if(helpRequested){
    helpStartedTime=millis();
    helpRequested=false;
    ticker.detach();
  }
  String cancel = String(LAMP_ID_NUM) + " cancel: " + String(LAMP_ID) + " ";
  client.publish(cancel_topic, (char*) cancel.c_str());
}


void longPressStart(){
  leds[0] = CRGB::Orange;
  ticker.attach(0.1, tick);
}


void callHelp(){
  Serial.print("Button long press end: ");
  Serial.println("Help called.");
  helpStartedTime=millis();
  String help = String(LAMP_ID_NUM) + " help: " + String(LAMP_ID) + " ";
  client.publish(help_topic, (char*) help.c_str());
}


void loop() {
  FastLED.show();

  button.tick();
  portalButton.tick();

  long now = millis();
  if(now - helpStartedTime > 1000*60*5 && helpRequested){
    Serial.println("Help request timed out.");
    ticker.detach();
    leds[0] = CRGB::Black;
    FastLED.show();
  }
  
  // put your main code here, to run repeatedly:
  if (!client.connected()) {
    reconnect();
  }
  client.loop();

}
