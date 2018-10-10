//currency with esp8266

#include <SoftwareSerial.h>

#define DEBUG_SERIAL

typedef void (*DataCompleteFunc)(char* dataStr);
typedef void (*CommandResultFunc)(uint8_t commandNum, const char** commandArray, char* commandResponse);
typedef void (*CommandsCompleteFunc)(uint8_t commandNum, const char** commandArray, bool error);

const byte rxPin = 2;
const byte txPin = 3;

SoftwareSerial esp8266(rxPin, txPin);

class DataManager {
#define DM_DATA_BUFFER_SIZE 256
public:
  DataManager(DataCompleteFunc func) {
    m_workMode = WorkMode::none;
    m_dataCompleteFunc = func;
  }
  
  void add(const char dataChar) {
    if (m_workMode == WorkMode::data) {  // write to buffer
      if ((m_dataBufferPos - m_dataBuffer) < DM_DATA_BUFFER_SIZE) {
        *m_dataBufferPos = dataChar;
        m_dataBufferPos++;
      } else {  //overbuffer
        m_workMode = WorkMode::none;
        return;
      }
    }

    if (dataChar == '{') {  // start write to buffer
      m_dataBufferPos = m_dataBuffer;
      m_workMode = WorkMode::data;
    }
    if (dataChar == '}') {  // end write to buffer
      m_dataBufferPos--;
      *m_dataBufferPos = '\0';
      m_workMode = WorkMode::none;
      //call result func
      m_dataCompleteFunc(m_dataBuffer);
    }    
  }
  
protected:
  enum WorkMode {none, data};
  char m_dataBuffer[DM_DATA_BUFFER_SIZE];
  
private:
  char* m_dataBufferPos;
  WorkMode m_workMode;
  DataCompleteFunc m_dataCompleteFunc;
};

class CommandManager {
#define CM_COMMAND_BUFFER_SIZE 64
#define CM_COMMAND_BUFFER_LAST (CM_COMMAND_BUFFER_SIZE - 1)
#define CM_TIMEOUT_MSEC 10000
#define CM_DATA_WAIT_MSEC 2500
public:
  CommandManager(DataCompleteFunc dFunc, CommandResultFunc crFunc, CommandsCompleteFunc cFunc) : 
                m_dataManager(dFunc), 
                m_commandBufferLast(m_commandBuffer + CM_COMMAND_BUFFER_LAST) {
    m_finishedWork = true;
    m_dataCounter = 0;
    m_commandResultFunc = crFunc;
    m_commandsCompleteFunc = cFunc;
  }
  
  void call(const char** commandArray) {
    m_commandArray = commandArray;
    m_numCommandForCall = 1;  // because 0 - is array size
    m_countForCall = (int)(*m_commandArray);
    m_finishedWork = false;
    m_callRepeatCount = 0;
    call();
  }

  void process() {
    const char* dataHeadSignature = "+IPD,";
    if (esp8266.available()) {
      char responseChar = esp8266.read();
      Serial.write(responseChar);
      char* headPos;
      String dataLen;
      if (!m_finishedWork) {
        if (m_workMode == WorkMode::data) { //read data
          //process data here
          m_dataManager.add(responseChar);
          
          m_dataCounter--;
          if (m_dataCounter == 0) {
            Serial.print("\r\n----DATA----");
            changeWorkMode(WorkMode::dataNextHead);
          }
          m_msecTimeWork = millis();
        } else {
          addToCommandBuffer(responseChar);
          AtResult respondResult = checkResult();
          switch (respondResult) {
          case AtResult::ok_rst:
            callNext();
            break;
          case AtResult::ok_send:
            callNext();
            changeWorkMode(WorkMode::dataHead);
            break;
          case AtResult::ok_dataHead:
            headPos = strstr(m_commandBuffer, dataHeadSignature);
            if ( headPos != NULL) {
              dataLen = String(headPos + sizeof(dataHeadSignature));
              m_dataCounter = dataLen.toInt();
#ifdef DEBUG_SERIAL
              Serial.print("\r\n----HEAD----");
              Serial.println(m_dataCounter);
#endif
              changeWorkMode(WorkMode::data);
            }
            break;
          case AtResult::ok:
            if (!isLongCommand()) {
              callNext();
            } else {
              changeWorkMode(WorkMode::longCommand);
            }
            break;
          case AtResult::error:
            callRepeat();
            break;
          case AtResult::none:
            break;
          default:
#ifdef DEBUG_SERIAL
            Serial.println("\r\n----UNKNOWN RESULT----");
#endif
            finishWork(true);
          }
          checkForNewCommandLine();
        }
      }
    }
    checkForTimeout();
  }
  
protected:
  enum AtResult {none, ok, ok_rst, ok_send, ok_dataHead, error};
  enum WorkMode {command, longCommand, dataHead, dataNextHead, data};

  char m_commandBuffer[CM_COMMAND_BUFFER_SIZE];  //last '\0' for strstr func
  char * m_commandBufferPos;
  char const* m_commandBufferLast;
  bool m_finishedWork;

  void addToCommandBuffer(char c) {
    if (m_commandBufferPos < m_commandBufferLast) {
      *m_commandBufferPos = c;
      m_commandBufferPos++;
      *m_commandBufferPos = '\0';
    } else {
      finishWork(true);
    }
  }

  void changeWorkMode(WorkMode workMode) {
    m_workMode = workMode;
    m_commandBufferPos = m_commandBuffer;
    *m_commandBufferPos = '\0';
  }

  AtResult checkResult() {
    switch(m_workMode) {
    case WorkMode::dataHead:
    case WorkMode::dataNextHead:
      if (strstr(m_commandBuffer, ":") != NULL) {
        return AtResult::ok_dataHead;
      }
      break;
    case WorkMode::longCommand:
      if (strstr(m_commandBuffer, ">") != NULL) {
        return AtResult::ok_send;
      }
      if (strstr(m_commandBuffer, "ready\r\n") != NULL) {
        return AtResult::ok_rst;
      }
      break;
    default:
      if (strstr(m_commandBuffer, "OK\r\n") != NULL) {
        return AtResult::ok;
      }
      if (strstr(m_commandBuffer, "ERROR\r\n") != NULL) {
        return AtResult::error;
      }
    }
    return AtResult::none;
  }

  void call() {
    if (m_countForCall > 0) {
      Serial.println("\r\n--------");
      esp8266.write(m_commandArray[m_numCommandForCall]);
      esp8266.write("\r\n");
      m_msecTimeWork = millis();
      changeWorkMode(WorkMode::command);
    } else {
      finishWork(false);
    }
  }

  void callNext() {
    m_numCommandForCall++;
    m_countForCall--;
    m_callRepeatCount = 0;
    call();
  }

  void callRepeat() {
    m_callRepeatCount++;
    if (m_callRepeatCount > 2) {
#ifdef DEBUG_SERIAL
      Serial.println("\r\n----END----");
#endif
      finishWork(true);
    } else {
      call();
    }
  }

  bool isLongCommand() {
    if ((strstr(m_commandArray[m_numCommandForCall], "+CIPSEND") != NULL)) {
      return true;
    }
    return false;
  }

  void checkForTimeout() {
    if (!m_finishedWork) {
      //if timer start again and millis() return less value
      if (millis() < m_msecTimeWork) {
        m_msecTimeWork = millis();
      }
      if ((m_workMode == WorkMode::dataNextHead) || (m_workMode == WorkMode::data)) {
        if ((millis() - m_msecTimeWork) > CM_DATA_WAIT_MSEC) {
#ifdef DEBUG_SERIAL
          Serial.print("\r\n----DATA-END----");
          Serial.println(m_dataCounter);
#endif
          callNext();
        }
      } else {
        if ((millis() - m_msecTimeWork) > CM_TIMEOUT_MSEC) {
#ifdef DEBUG_SERIAL
          Serial.println("\r\n----TIMEOUT----");
#endif
          finishWork(true);
        }
      }
    }
  }

  void checkForNewCommandLine() {
    if (*(m_commandBufferPos - 1) == '\n') {      //new command line
      m_commandResultFunc(m_numCommandForCall, m_commandArray, m_commandBuffer);
      changeWorkMode(m_workMode);
    }
  }

  void finishWork(bool error) {
    m_finishedWork = true;
    m_commandsCompleteFunc(m_numCommandForCall, m_commandArray, error);
  }
  
private:
  const char** m_commandArray;
  uint8_t m_numCommandForCall;
  uint8_t m_countForCall;
  uint8_t m_callRepeatCount;
  uint16_t m_dataCounter;
  unsigned long m_msecTimeWork;
  WorkMode m_workMode;
  DataManager m_dataManager;
  CommandResultFunc m_commandResultFunc;
  CommandsCompleteFunc m_commandsCompleteFunc;
};

//***********************************************************************************************
//***********************************************************************************************
//***********************************************************************************************

const char* commandsStart[] = {(char*)4,
  "AT", 
  "AT+CWMODE_CUR=1", 
  "AT+CIPSTA_CUR?", 
  "AT+CIPSTATUS"
};

const char* commandsConnectToWifi[] = {(char*)2,
  "AT+CWJAP_CUR=\"*\",\"*\"",
  "AT+CIPSTA_CUR?"
};

const char* commandsCloseTCP[] = {(char*)1,
  "AT+CIPCLOSE"
};

const char* commandsGetData[] = {(char*)4,
  "AT+CIPSTART=\"TCP\",\"194.28.174.234\",80", 
  "AT+CIPSEND=67", 
  "GET /export/exchange_rate_cash.json HTTP/1.1\r\nHost: bank-ua.com\r\n", 
  "AT+CIPCLOSE"
};

bool isConnectedToWiFi = false;
bool isConnectedToTCP = false;
CommandManager cm(&dataCompleteFunc, &commandResultFunc, &commandsCompleteFunc);

#define MAX_FLOAT_STR_LENGTH 12

// "key":"value"
float getFloatJSONValue(char* json, const char* key) {
  float ret = 0.0;
  char* cStart = strstr(json, key);
  if ( cStart != NULL) {
    cStart += strlen(key) + sizeof("\":\"");
    char* cEnd = strchr(cStart, '\"');
    if ((cEnd != NULL) && ((cEnd - cStart) < MAX_FLOAT_STR_LENGTH)) {
      *cEnd = '\0';
      String s(cStart);
      ret = s.toFloat();
      *cEnd = '\"';
    }
  }
  return ret;
}

void dataCompleteFunc(char* dataStr) {
#ifdef DEBUG_SERIAL
  Serial.print("\r\nFIND DATA: ");
  Serial.println(dataStr);
#endif
  float buy = getFloatJSONValue(dataStr, "rateBuy");
  float sale = getFloatJSONValue(dataStr, "rateSale");
#ifdef DEBUG_SERIAL
  if ((buy > 0) && (sale > 0)) {
    Serial.print("\r\nFIND : ");
    Serial.print(sale);
    Serial.print(" ");
    Serial.println(buy);
  }
#endif
}

void commandResultFunc(uint8_t commandNum, const char** commandArray, char* commandResponse) {
#ifdef DEBUG_SERIAL
  Serial.print("\r\nCOMMAND: ");
  Serial.print(commandNum);
  Serial.print(" ");
  Serial.println(commandArray[commandNum]);
#endif
  if (strcmp("AT+CIPSTA_CUR?", commandArray[commandNum]) == 0) {
    if (strstr(commandResponse, "+CIPSTA_CUR:ip:\"") != NULL) {
      if (strstr(commandResponse, "\"0.0.0.0\"") == NULL) {
        isConnectedToWiFi = true;
#ifdef DEBUG_SERIAL
        Serial.println("ConnectedToWiFi");
#endif
      } else {
        isConnectedToWiFi = false;
      }
    }
  } else if (strcmp("AT+CIPSTATUS", commandArray[commandNum]) == 0) {
    if (strstr(commandResponse, "STATUS:3") != NULL) {
      isConnectedToTCP = true;
#ifdef DEBUG_SERIAL
        Serial.println("ConnectedToTCP");
#endif
    } else {
      isConnectedToTCP = false;
    }
  }
}

void commandsCompleteFunc(uint8_t commandNum, const char** commandArray, bool error) {
#ifdef DEBUG_SERIAL
  Serial.print("\r\n\r\nEND COMMANDS: ");
  Serial.print(commandNum);
  Serial.println(error?" error":" ok");
#endif
  if (!error) {
    if (commandArray == commandsStart || commandArray == commandsConnectToWifi || commandArray == commandsCloseTCP) {
      if (isConnectedToTCP) {
        cm.call(commandsCloseTCP);
      } else {
        if (isConnectedToWiFi) {
          cm.call(commandsGetData);
        } else {
          cm.call(commandsConnectToWifi);
        }
      }
    }
  } else {
    // Restart after error
    cm.call(commandsStart);
  }
}

const char* request = "GET /export/exchange_rate_cash.json HTTP/1.1\r\nHost: bank-ua.com\r\n";

#define SERIAL_BIT_RATE 9600

void setup() {
  // Open serial communications and wait for port to open:
  Serial.begin(SERIAL_BIT_RATE);
  while (!Serial) {
    ; // wait for serial port to connect. Needed for native USB port only
  }

  // set the data rate for the SoftwareSerial port
  esp8266.begin(SERIAL_BIT_RATE);
  // TODO: delete temporary command for reset after bad command using.
  //esp8266.write("AT+CIPCLOSE\r\n----------------------------------------------\r\n");
  cm.call(commandsStart);
}

void loop() {
  cm.process();
  if (Serial.available()) {
      char requestChar = Serial.read();
      esp8266.write(requestChar);
  }
}
