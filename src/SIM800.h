/*************************************************************************
* SIM800 GPRS/HTTP Library
* Distributed under GPL v2.0
* Written by Stanley Huang <stanleyhuangyc@gmail.com>
* For more information, please visit http://arduinodev.com
*************************************************************************/

#ifndef SIM800_H__
#define SIM800_H__

#include <Arduino.h>

typedef enum {
    HTTP_DISABLED = 0,
    HTTP_READY,
    HTTP_CONNECTING,
    HTTP_READING,
    HTTP_ERROR,
} HTTP_STATES;

typedef struct {
  float lat;
  float lon;
  uint8_t year; /* year past 2000, e.g. 15 for 2015 */
  uint8_t month;
  uint8_t day;
  uint8_t hour;
  uint8_t minute;
  uint8_t second;
} GSM_LOCATION;

class CGPRS_SIM800 {
public:
    CGPRS_SIM800(HardwareSerial *serial, HardwareSerial *debug,
                 int8_t reset_pin, int8_t enable_pin, int8_t dtr_pin);

    // initialize the module
    bool init();

    // setup network
    byte setup(const char* apn);

    // power off
    bool powerdown(void);

    // get network operator name
    bool getOperatorName();

    // check for incoming SMS
    bool checkSMS();

    // get signal quality level (in dB)
    int getSignalQuality();

    // get GSM location and network time
    bool getLocation(GSM_LOCATION* loc);

    // initialize HTTP connection
    bool httpInit();

    // initialize HTTPS connection
    bool httpsInit();

    // terminate HTTP(s) connection
    void httpUninit();

    // connect to HTTP(s) server, do GET
    bool httpGET(const char* url, const char* args = 0);

    // connect to HTTP(s) server, do POST
    bool httpPOST(const char *url, const char *payload, const char length,
                  const char *mimetype);

    // check if HTTP(s) connection is established
    // return 0 for in progress, 1 for success, 2 for error
    byte httpIsConnected();

    // read data from HTTP(s) connection
    void httpRead();

    // check if HTTP(s) connection is established
    // return 0 for in progress, -1 for error, bytes of http payload on success
    int httpIsRead();

    // send AT command and check for expected response
    byte sendCommand(const char* cmd, unsigned int timeout = 2000,
                     const char* expected = 0);

    // send AT command and check for two possible responses
    byte sendCommand(const char* cmd, const char* expected1,
                     const char* expected2, unsigned int timeout = 2000);

    byte sendCommand(StringSumHelper &str, unsigned int timeout = 2000,
                     const char* expected = 0);

    byte sendCommand(StringSumHelper &str, const char* expected1,
                     const char* expected2, unsigned int timeout = 2000);


    // toggle low-power mode
    bool sleep(bool enabled)
    {
        return sendCommand(enabled ? "AT+CFUN=0" : "AT+CFUN=1");
    }

    // check if there is available serial data
    bool available() { return m_serial ? m_serial->available() : false; };

    byte httpState() { return m_httpState; };
    char buffer() { return &m_buffer[0]; };
    void purgeSerial();
private:
    byte checkbuffer(const char* expected1, const char* expected2 = 0,
                     unsigned int timeout = 2000);

    char m_buffer[256];
    byte m_httpState;
    bool m_useSSL;
    byte m_bytesRecv;
    uint32_t m_checkTimer;

    HardwareSerial *m_serial;
    HardwareSerial *m_debug;

    int8_t m_reset_pin;
    int8_t m_enable_pin;
    int8_t m_dtr_pin;
};

#endif

// vim:ts=4:sw=4:ai:et:si:sts=4
