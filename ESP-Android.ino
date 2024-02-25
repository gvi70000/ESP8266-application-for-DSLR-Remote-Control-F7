#include <EEPROM.h>
#include <ESP8266WiFi.h>

#define IP_Start        0
#define IP_Len          4
#define SSID_Len        11
#define Pass_Start      IP_Len + SSID_Len
#define Pass_Len        17
#define SrvPort         80
#define Task_Delay      5
#define WiFi_Delay      100

//We allow only one device  to connect
#define MAX_SRV_CLIENTS 1
#define ZERO_CHAR       48
#define StrConnOK       "GVI!\n"
#define StrDefaultSSID  "RemoteSen"
#define StrDefaultPASS  "01234567"
#define SignalReadyPin  2 // pin to inform STM that ESP is ready for action


//Default values for IP and gateway
uint8_t myIP[4] = {10, 0, 0, 1};
const uint8_t mySubNet[4] = {255, 255, 255, 0};
bool SoftAP_ready = false;
String mySSID, myPASSWORD;
//uint32_t myTime;

void get_IP(){
	for(uint8_t idx = IP_Start; idx < IP_Len; idx++){
		myIP[idx] = EEPROM.read(idx);
	}
}

void setIP(){
	for(uint8_t idx = IP_Start; idx < IP_Len; idx++){
		EEPROM.write(idx, myIP[idx]);
	}
	EEPROM.commit();	
}

String get_MemStr(uint8_t StartPos, uint8_t MaxLen){
	unsigned char crtChar;
	uint8_t pos = StartPos;
	char data[MaxLen + 1]; 
	//Read until null character
  for(uint8_t idx = 0; idx < MaxLen; idx++){
    crtChar = EEPROM.read(StartPos + idx);
    data[idx] = crtChar;
    if(crtChar == '\0') break;
  }
	return String(data);
}

//the lenght will be limited in Antroid app, no need to check it here
void set_MemStr(uint8_t myType, String my_data){
	uint8_t dataLen = my_data.length();
	uint8_t pos = 0;
  uint8_t idx = 0;
  // Serial.println("set_MemStr");
  // Serial.println(myType);
  // Serial.println(my_data);
	if(myType){
		//PASSWORD
		pos = Pass_Start;
	} else {
		//SSID
		pos = IP_Len;		
	}
	for(idx = 0; idx < dataLen; idx++){
		EEPROM.write(pos + idx, my_data[idx]);
	}
	//Add termination null character for String Data
	EEPROM.write(pos + idx, '\0');
	EEPROM.commit();
}

void resetData(){
	//Max len for password is 15
	//Max len for SSID is 11
	myIP[0] = 10;
  myIP[1] = 0;
  myIP[2] = 0;
  myIP[3] = 1;
	setIP();
  //Password must be min 8 chars for SoftAP to work
  set_MemStr(0, StrDefaultSSID);
	set_MemStr(1, StrDefaultPASS);
}

void arrayToIP(uint8_t myArr[], uint8_t len, uint8_t pos){
  uint8_t tmpIp = 0;
  switch(len){
    case 1:
      tmpIp = myArr[0] - ZERO_CHAR;
      break;
    case 2:
      tmpIp = 10*(myArr[0] - ZERO_CHAR) + (myArr[1] - ZERO_CHAR);
      break;
    case 3:
      tmpIp = 100*(myArr[0] - ZERO_CHAR) + 10*(myArr[1] - ZERO_CHAR) + (myArr[2] - ZERO_CHAR);
      break;       
  }
  myIP[pos] = tmpIp;
}

void processIP(uint8_t myCredentials[], uint8_t len){
  //First 2 bytes are !! and last 2 are \r\n
  uint8_t tmpLenA = len - 1;
  uint8_t tmpLenB = len - 2;
  uint8_t j = 0;
  uint8_t tmpIdx = 0;
  uint8_t tmpArr[3];
  for(uint8_t idx = 2; idx < tmpLenA; idx++){
    //if we have .
    if((myCredentials[idx] == 46) || (idx == tmpLenB)){
      //convert tmpArr to byte
      arrayToIP(tmpArr, tmpIdx, j);
      //Reset tmpIdx
      tmpIdx = 0;
      //Increment IP position
      j++;
    } else {
      //add the char in tmpArr
      tmpArr[tmpIdx] = myCredentials[idx];
      //Increment the index
      tmpIdx++;
    }
  }
  //Save IP in EEPROM
  setIP();
}

void processString(uint8_t myCredentials[], uint8_t len, uint8_t id){
  // Serial.println("Process string");
  char myStr[len - 2];
  uint8_t idx = 2;
  for (idx = 2; idx < len; idx++){
    // Serial.println(myCredentials[idx]);
    myStr[idx - 2] = myCredentials[idx];
  }
  myStr[idx - 2] = '\0';
  set_MemStr(id, String(myStr));
}

WiFiServer server(SrvPort);
WiFiClient serverClient;

void setup() {
  //myTime = millis();
  //Set pin2 high to put STM on hold till we finish the setup
  pinMode(SignalReadyPin, OUTPUT);
  digitalWrite(SignalReadyPin, HIGH);
	//require ESP8266 >= 2.4.0 https://github.com/esp8266/Arduino/releases/tag/2.4.0-rc1
	Serial.setRxBufferSize(128);
	Serial.begin(115200);
	//IP = 4bytes; SSID = 12bytes; PASSWORD = min 8 max 16 bytes
	EEPROM.begin(32);

  //Serial.println();
	//Serial.println("Starting...");
	WiFi.mode(WIFI_AP);
  
	////Run this code only once to set default values for IP, SSID and password in EEPROM
	//resetData();
	////
  
	//Here we read from EEPROM the IP and gateway
	get_IP();
	//Here we read from EEPROM the network name (SSID) and password
	mySSID = get_MemStr(IP_Len, SSID_Len);
	myPASSWORD = get_MemStr(Pass_Start, Pass_Len);
  // Serial.print("PASS...");Serial.println(myPASSWORD);
  // Serial.print("SSID...");Serial.println(mySSID);
  // Serial.print("Pass...");Serial.println(myPASSWORD);
  // Serial.print("IP...");Serial.print(myIP[0]);Serial.print(".");Serial.print(myIP[1]);Serial.print(".");Serial.print(myIP[2]);Serial.print(".");Serial.println(myIP[3]);
  //Wait 1s for AP_START...
  //channel - optional parameter to set Wi-Fi channel, from 1 to 13. Default channel = 1.
  uint8_t channel = random(1, 13);
  // Serial.print("Channel = "); Serial.println(channel);
  while(!SoftAP_ready){
    SoftAP_ready = WiFi.softAP(mySSID, myPASSWORD, channel, false, MAX_SRV_CLIENTS);
	  // Serial.println(SoftAP_ready ? "Ready" : "Failed!");
    delay(WiFi_Delay);
  }
	
	//Setup the IP...
	IPAddress Ip(myIP[0], myIP[1], myIP[2], myIP[3]);
	IPAddress GateWay(myIP[0], myIP[1], myIP[2], 1);
	IPAddress NetMask(mySubNet[0], mySubNet[1], mySubNet[2], mySubNet[3]);
	//Set new configuration
	WiFi.softAPConfig(Ip, GateWay, NetMask);
	server.begin();
  serverClient.setNoDelay(true);
	server.setNoDelay(true);
	WiFi.setSleepMode(WIFI_NONE_SLEEP); // disable WiFi sleep for more performance
  //Set pin2 low to remove STM from hold
	digitalWrite(LED_BUILTIN, LOW);
  //Serial.println("***");
  //Serial.println(millis() -myTime);
}

void loop() {
	serverClient = server.available();
	if(serverClient){
		if(serverClient.connected()){
			//Inform STM board thet ESP is ready 
			serverClient.write(StrConnOK);
      // Serial.println(StrConnOK);
		}
		while(serverClient.connected()){
			size_t rxlen = serverClient.available();
			if (rxlen > 0){
				uint8_t rx_buff[rxlen];
				serverClient.readBytes(rx_buff, rxlen);
				//reset the password, IP and SSID
				if((rx_buff[0] == '!') && (rx_buff[1] == '!')){
				  //we get the IP
          processIP(rx_buff, rxlen);
				} else if((rx_buff[0] == '#') && (rx_buff[1] == '#')){
          //we get the SSID
          processString(rx_buff, rxlen - 2, 0);      
				}else if((rx_buff[0] == '$') && (rx_buff[1] == '$')){
          //we get the Password
          processString(rx_buff, rxlen - 2, 1);     
       }else if((rx_buff[0] == '%') && (rx_buff[1] == '%')){
					//Reset all data
					resetData();
				} else {
          //Serial.println("--->");
					//We send the data directly to STM board
					Serial.write(rx_buff, rxlen);
          //for(uint8_t i = 0; i<rxlen;i++){
          //  Serial.print(rx_buff[i]);Serial.print(" ");
          //}
          //Serial.println("RX");
				}
			}
     
			size_t txlen = Serial.available();
			if (txlen > 0){
        //Serial.println("<---");
				uint8_t tx_buff[txlen];
				Serial.readBytes(tx_buff, txlen);
				serverClient.write(tx_buff, txlen);
        //for(uint8_t i = 0; i<txlen;i++){
        //  Serial.print(tx_buff[i]);Serial.print(" ");
        //}
        //Serial.println("TX");
			}
      //WiFi.RSSI();
      delay(Task_Delay); 
		}
	}
	delay(WiFi_Delay);
}