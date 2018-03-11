#ifdef DEBUG_ESP_PORT
#define DEBUG_MSG(...) DEBUG_ESP_PORT.printf( __VA_ARGS__ )
#else
#define DEBUG_MSG(...)
#endif

#include <FS.h>                   //this needs to be first, or it all crashes and burns...
#include <ESP8266WiFi.h>          //https://github.com/esp8266/Arduino
#include <PubSubClient.h>
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <WiFiManager.h>          //https://github.com/tzapu/WiFiManager
#include <ArduinoJson.h>
#include <OneButton.h>

/* ==========Map of pin==========
* GPIO 12  --> Relay(1)
* GPIO 13  --> Relay(2)
* GPIO 14  --> LED
* GPIO 16  --> Touch Button
*/

#define Triac1 12
#define Triac2 13
#define LED 14
#define Button 16

char mqtt_server[40];
char mqtt_port[6] = "1883";

bool shouldSaveConfig = false;

unsigned long lastRecon = 0;
unsigned long lastStatus = 0;

WiFiClient espClient;
PubSubClient clientMQTT(espClient);

OneButton button(Button, false);

boolean reconnect() {
	if (clientMQTT.connect("switchRoom")) {
		clientMQTT.subscribe("/switchRoom/gpio/12");
		clientMQTT.subscribe("/switchRoom/gpio/13");
	}
	return clientMQTT.connected();
}

void saveConfigCallback() {
	DEBUG_MSG("Should save config\n");
	shouldSaveConfig = true;
}

void sendStatus() {
	DynamicJsonBuffer sendBuf;
	JsonObject& root = sendBuf.createObject();
	root["freeHeap"] = String(ESP.getFreeHeap());
	root["cpuFreqMHz"] = String(ESP.getCpuFreqMHz());
	root["resetReason"] = ESP.getResetReason();
	String out;
	root.printTo(out);
	clientMQTT.publish("/switchRoom/status", out.c_str());
	out = "";
}

void callback(char* topic, byte* payload, unsigned int length) {
	DEBUG_MSG("New callback of MQTT-broker\n");
	payload[length] = '\0';
	String strTopic = String(topic);
	String strPayload = String((char*)payload);
	DEBUG_MSG(strTopic.c_str());
	DEBUG_MSG(" =>> ");
	DEBUG_MSG(strPayload.c_str());
	DEBUG_MSG("\n");
	if (strTopic == "/switchRoom/gpio/12") {
		if (strPayload == "off" || strPayload == "0" || strPayload == "false") digitalWrite(Triac1, 0);
		if (strPayload == "on" || strPayload == "1" || strPayload == "true") digitalWrite(Triac1, 1);
		yield();
		DEBUG_MSG("ON/OFF Triac1 callback\n");
		return;
	}
	if (strTopic == "/switchRoom/gpio/13") {
		if (strPayload == "off" || strPayload == "0" || strPayload == "false") digitalWrite(Triac2, 0);
		if (strPayload == "on" || strPayload == "1" || strPayload == "true") digitalWrite(Triac2, 1);
		yield();
		DEBUG_MSG("ON/OFF Triac2 callback\n");
		return;
	}
}

void Click() {
	DEBUG_MSG("Click\n");
	clientMQTT.publish("/switchRoom/Button/Click", "1");
}

void DoubleClick() {
	DEBUG_MSG("DoubleClick\n");
	clientMQTT.publish("/switchRoom/Button/DoubleClick", "1");
}

void LongPressStart() {
	DEBUG_MSG("LongPressStart\n");
	clientMQTT.publish("/switchRoom/Button/LongPressStart", "1");
}

void LongPressStop() {
	DEBUG_MSG("LongPressStop\n");
	clientMQTT.publish("/switchRoom/Button/LongPressStop", "1");
}

void setup()
{
#ifdef DEBUG_ESP_PORT
	Serial.begin(115200);
#endif
	//read configuration from FS json
	DEBUG_MSG("mounting FS...");

	if (SPIFFS.begin()) {
		DEBUG_MSG("mounted file system\n");
		if (SPIFFS.exists("/config.json")) {
			//file exists, reading and loading
			DEBUG_MSG("reading config file\n");
			File configFile = SPIFFS.open("/config.json", "r");
			if (configFile) {
				DEBUG_MSG("opened config file\n");
				size_t size = configFile.size();
				// Allocate a buffer to store contents of the file.
				std::unique_ptr<char[]> buf(new char[size]);

				configFile.readBytes(buf.get(), size);
				DynamicJsonBuffer jsonBuffer;
				JsonObject& json = jsonBuffer.parseObject(buf.get());
				json.printTo(Serial);
				if (json.success()) {
					DEBUG_MSG("\nparsed json\n");

					strcpy(mqtt_server, json["mqtt_server"]);
					strcpy(mqtt_port, json["mqtt_port"]);

				}
				else {
					DEBUG_MSG("failed to load json config\n");
				}
			}
		}
	}
	else {
		DEBUG_MSG("failed to mount FS\n");
	}
	//end read

	// The extra parameters to be configured (can be either global or just in the setup)
	// After connecting, parameter.getValue() will get you the configured value
	// id/name placeholder/prompt default length
	WiFiManagerParameter custom_mqtt_server("server", "mqtt server", mqtt_server, 40);
	WiFiManagerParameter custom_mqtt_port("port", "mqtt port", mqtt_port, 6);

	//WiFiManager
	//Local intialization. Once its business is done, there is no need to keep it around
	WiFiManager wifiManager;

	//set config save notify callback
	wifiManager.setSaveConfigCallback(saveConfigCallback);


	//add all your parameters here
	wifiManager.addParameter(&custom_mqtt_server);
	wifiManager.addParameter(&custom_mqtt_port);

	wifiManager.setTimeout(120);

	//fetches ssid and pass and tries to connect
	//if it does not connect it starts an access point with the specified name
	//here  "AutoConnectAP"
	//and goes into a blocking loop awaiting configuration
	if (!wifiManager.autoConnect("switchRoom", "password")) {
		DEBUG_MSG("failed to connect and hit timeout\n");
		delay(3000);
		//reset and try again, or maybe put it to deep sleep
		ESP.reset();
		delay(5000);
	}

	//if you get here you have connected to the WiFi
	DEBUG_MSG("connected...yeey :)\n");

	//read updated parameters
	strcpy(mqtt_server, custom_mqtt_server.getValue());
	strcpy(mqtt_port, custom_mqtt_port.getValue());

	//save the custom parameters to FS
	if (shouldSaveConfig) {
		DEBUG_MSG("saving config\n");
		DynamicJsonBuffer jsonBuffer;
		JsonObject& json = jsonBuffer.createObject();
		json["mqtt_server"] = mqtt_server;
		json["mqtt_port"] = mqtt_port;

		File configFile = SPIFFS.open("/config.json", "w");
		if (!configFile) {
			DEBUG_MSG("failed to open config file for writing\n");
		}
		json.printTo(configFile);
		configFile.close();
		//end save
	}
	pinMode(Triac1, OUTPUT);
	pinMode(Triac2, OUTPUT);
	DEBUG_MSG("local ip => ");
	DEBUG_MSG(String(WiFi.localIP()).c_str());
	clientMQTT.setServer(mqtt_server, atoi(mqtt_port));
	clientMQTT.setCallback(callback);

	button.setDebounceTicks(5);
	button.attachClick(Click);
	button.attachDoubleClick(DoubleClick);
	button.attachLongPressStart(LongPressStart);
	button.attachLongPressStop(LongPressStop);
}

void loop()
{

	if (millis() - lastStatus >= 60000) {
		lastStatus = millis();
		sendStatus();
	}
	if (!clientMQTT.connected()) {
		if (millis() - lastRecon >= 5000) {
			reconnect();
			lastRecon = millis();
		}
	}
	else {
		button.tick();
		clientMQTT.loop();
	}
}
