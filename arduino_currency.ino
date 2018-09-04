//currency with esp8266

#include <SoftwareSerial.h>

#define SERIAL_BIT_RATE 9600
typedef void (*DataCompleteFunc)(const char* dataStr);
typedef void (*CommandsCompleteFunc)(uint8_t commandNum);

SoftwareSerial esp8266(2, 3);

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
  char m_dataBuffer[DM_DATA_BUFFER_SIZE + 1];
  
private:
  char* m_dataBufferPos;
  WorkMode m_workMode;
  DataCompleteFunc m_dataCompleteFunc;
};

class CommandManager {
#define CM_SHIFT_COMMAND_BUFFER_SIZE 16
#define CM_SHIFT_COMMAND_BUFFER_LAST (CM_SHIFT_COMMAND_BUFFER_SIZE - 1)
#define CM_TIMEOUT_MSEC 10000
#define CM_DATA_WAIT_MSEC 2000
public:
  CommandManager(const char** commandArray, DataCompleteFunc dFunc, CommandsCompleteFunc cFunc) : m_dataManager(dFunc) {
    m_commandArray = commandArray;
    m_finishedWork = true;
    m_dataCounter = 0;
    m_shiftCommandBuffer[CM_SHIFT_COMMAND_BUFFER_SIZE] = 0;
    m_msecTimeWork = 0;
    m_commandsCompleteFunc = cFunc;
  }
  
  void start(uint8_t startCommandForCall, uint8_t countForCall) {
    m_numCommandForCall = startCommandForCall;
    m_countForCall = countForCall;
    m_finishedWork = false;
    m_callRepeatCount = 0;
    call();
  }

  void process() {
    if (esp8266.available()) {
      char responceChar = esp8266.read();
      Serial.write(responceChar);
      char* headPos;
      String dataLen;
      if (!m_finishedWork) {
        if (m_workMode == WorkMode::data) { //read data
          //process data here
          m_dataManager.add(responceChar);
          
          m_dataCounter--;
          if (m_dataCounter == 0) {
            Serial.print("\r\n----DATA----");
            changeWorkMode(WorkMode::dataHead);
          }
        } else {
          addToShiftCommandBuffer(responceChar);
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
            headPos = strstr(m_shiftCommandBuffer, "+IPD,");
            if ( headPos != NULL) {
              dataLen = String(headPos + 5);
              m_dataCounter = dataLen.toInt();
              Serial.print("\r\n----HEAD----");
              Serial.println(m_dataCounter);
              changeWorkMode(WorkMode::data);
            }
            break;
          case AtResult::ok:
            if (!needNextCommand()) {
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
            Serial.println("\r\n----END----");
            finishWork(true);
          }
        }
      }
    }
    checkForTimeout();
    checkForDataTimeOut();
  }
  
protected:
  enum AtResult {none, ok, ok_rst, ok_send, ok_dataHead, error};
  enum WorkMode {command, longCommand, dataHead, data};

  char m_shiftCommandBuffer[CM_SHIFT_COMMAND_BUFFER_SIZE + 1];
  bool m_finishedWork;

  void addToShiftCommandBuffer(char c) {
    memcpy(m_shiftCommandBuffer, m_shiftCommandBuffer+1, CM_SHIFT_COMMAND_BUFFER_LAST);
    m_shiftCommandBuffer[CM_SHIFT_COMMAND_BUFFER_LAST] = c;
  }

  void changeWorkMode(WorkMode workMode) {
    m_workMode = workMode;
    //clearShiftCommandBuffer
    memset(m_shiftCommandBuffer, '_', CM_SHIFT_COMMAND_BUFFER_SIZE);
  }

  AtResult checkResult() {
    switch(m_workMode) {
    case WorkMode::dataHead:
      if (strstr(m_shiftCommandBuffer, ":") != NULL) {
        return AtResult::ok_dataHead;
      }
      break;
    case WorkMode::longCommand:
      if (strstr(m_shiftCommandBuffer, ">") != NULL) {
        return AtResult::ok_send;
      }
      if (strstr(m_shiftCommandBuffer, "ready\r\n") != NULL) {
        return AtResult::ok_rst;
      }
      break;
    default:
      if (strstr(m_shiftCommandBuffer, "OK\r\n") != NULL) {
        return AtResult::ok;
      }
      if (strstr(m_shiftCommandBuffer, "ERROR\r\n") != NULL) {
        return AtResult::error;
      }
    }
    return AtResult::none;
  }

  void call() {
    if (m_numCommandForCall < m_countForCall) {
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
    m_callRepeatCount = 0;
    call();
  }

  void callRepeat() {
    m_callRepeatCount++;
    if (m_callRepeatCount > 3) {
      Serial.println("\r\n----END----");
      finishWork(true);
    } else {
      call();
    }
  }

  bool needNextCommand() {
    if (strstr(m_commandArray[m_numCommandForCall], "+RST") != NULL) {
      return true;
    }
    if (strstr(m_commandArray[m_numCommandForCall], "+CIPSEND") != NULL) {
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
      if ((millis() - m_msecTimeWork) > CM_TIMEOUT_MSEC) {
        Serial.println("\r\n----TIMEOUT----");
        finishWork(true);
        m_msecTimeWork = millis();
      }
    }
  }

  void checkForDataTimeOut() {
    if (m_workMode == WorkMode::dataHead) {
      if ((millis() - m_msecTimeWork) > CM_DATA_WAIT_MSEC) {
        Serial.println("\r\n----DATA-END----");
        callNext();
      }
    }
  }

  void finishWork(bool error) {
    m_finishedWork = true;
    if (error) {
      m_commandsCompleteFunc(0);
    } else {
      m_commandsCompleteFunc(m_numCommandForCall);
    }
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
  CommandsCompleteFunc m_commandsCompleteFunc;
};

const char* commands[] = {"AT", 
                          /*"AT+RST", 
                          "AT+CWMODE_CUR=1", 
                          "AT+CWJAP_CUR=\"****\",\"****\"", */
                          "AT+CIPSTART=\"TCP\",\"194.28.174.234\",80", 
                          "AT+CIPSEND=67", //71 - 4
                          "GET /export/exchange_rate_cash.json HTTP/1.1\r\nHost: bank-ua.com\r\n", 
                          "AT+CIPCLOSE", 
                          "AT", 
                          "AT", 
                          "AT", 
                          "AT", 
                          "AT", 
                          "AT", 
                          "AT", 
                          "AT", 
                          "AT", 
                          "AT"};

void dataCompleteFunc(const char* dataStr) {
  Serial.println("\r\n\r\nFIND DATA!!!");
  Serial.println(dataStr);
}

void commandsCompleteFunc(uint8_t commandNum) {
  Serial.println("\r\n\r\nEND COMMANDS!!!");
  Serial.println(commandNum);
}

CommandManager cm(commands, &dataCompleteFunc, &commandsCompleteFunc);

const char* request = "GET /export/exchange_rate_cash.json HTTP/1.1\r\nHost: bank-ua.com\r\n";

void setup() {
  // Open serial communications and wait for port to open:
  Serial.begin(SERIAL_BIT_RATE);
  while (!Serial) {
    ; // wait for serial port to connect. Needed for native USB port only
  }

  Serial.println("Started");

  // set the data rate for the SoftwareSerial port
  esp8266.begin(SERIAL_BIT_RATE);
  // TODO: delete temporary command for reset after bad command using.
  esp8266.write("AT+CIPCLOSE\r\n----------------------------------------------\r\n");
  cm.start(0,8);
}

void loop() {
  cm.process();
}
