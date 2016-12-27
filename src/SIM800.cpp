/*************************************************************************
* SIM800 GPRS/HTTP Library
* Distributed under GPL v2.0
* Written by Stanley Huang <stanleyhuangyc@gmail.com>
* For more information, please visit http://arduinodev.com
*************************************************************************/

#include "SIM800.h"

#define SEND_OR_DIE(x, ...) \
if (!sendCommand((x), ##__VA_ARGS__)) \
{ \
    m_httpState = HTTP_ERROR; \
    return false; \
}

CGPRS_SIM800::CGPRS_SIM800(HardwareSerial *serial, HardwareSerial *debug,
                           int8_t reset_pin, int8_t enable_pin, int8_t dtr_pin)
{
    m_httpState = HTTP_DISABLED;
    m_serial = serial;
    m_debug = debug;
    m_reset_pin = reset_pin;
    m_enable_pin = enable_pin;
    m_dtr_pin = dtr_pin;
    m_fram = NULL;
    m_response_cache = NULL;
}

bool CGPRS_SIM800::init()
{
    if (!m_serial) {
        return false;
    }

    if (m_enable_pin != -1) {
        pinMode(m_enable_pin, OUTPUT);
        digitalWrite(m_enable_pin, HIGH);
    }

    m_serial->begin(115200);
    pinMode(m_reset_pin, OUTPUT);
    digitalWrite(m_reset_pin, HIGH);
    delay(10);
    digitalWrite(m_reset_pin, LOW);
    delay(100);
    digitalWrite(m_reset_pin, HIGH);
    delay(3000);
    if (sendCommand("AT")) {
        sendCommand("AT+IPR=115200");
        sendCommand("ATE0");
        sendCommand("AT+CFUN=1", 10000);
        return true;
    }
    return false;
}

void CGPRS_SIM800::attachRAM(Adafruit_FRAM_SPI *fram)
{
    m_fram = fram;
    m_response_cache = new Cache_Segment(fram, 0x0800, 1024, 64, 64, NULL,
                                         true);
}

byte CGPRS_SIM800::setup(const char *apn)
{
    bool success = false;
    uint16_t len;
    for (byte n = 0; n < 30; n++) {
        len = sendCommand("AT+CREG?", 2000)
        if (len) {
            m_response_cache->circularRead(m_buffer, min(len, 32), true);
            char **p = strstr(m_buffer, "0,");
            if (p) {
                char *mode = *(p + 2);
                if (m_debug) {
                    m_debug->print("Mode:");
                    m_debug->println(mode);
                }

                if (mode == '1' || mode == '5') {
                    success = true;
                    break;
                }
            }
        }
        delay(1000);
    }

    if (!success) {
        return 1;
    }

    if (!sendCommand("AT+CGATT?")) {
        return 2;
    }

    if (!sendCommand("AT+SAPBR=3,1,\"Contype\",\"GPRS\"")) {
        return 3;
    }

    if (!sendCommand("AT+SAPBR=3,1,\"APN\",\"" + String(apn) + "\"")) {
        return 4;
    }

    sendCommand("AT+SAPBR=1,1", 10000);
    sendCommand("AT+SAPBR=2,1", 10000);

    // sets the SMS mode to text
    sendCommand("AT+CMGF=1");    

    // selects the memory
    if (!sendCommand("AT+CPMS=\"SM\",\"SM\",\"SM\"")) {
        return 5;
    }

    return 0;
}

bool CGPRS_SIM800::powerdown(void)
{
    httpUninit();
    return !(!(sendCommand("AT+SAPBR=0,1")));
}

bool CGPRS_SIM800::getOperatorName()
{
    uint8_t which;

    // display operator name
    uint16_t len = sendCommand("AT+COPS?", "OK\r", "ERROR\r", &which);
    if (which == 1) {
        // Eat the matched data
        m_response_cache->circularRead(m_buffer, min(len, 32));

        // Read the data AFTER the matched data
        m_response_cache->circularRead(m_buffer, 32, true);
        char **p = strstr(m_buffer, ",\"");
        if (p) {
            p += 2;
            char **s = strchr(p, '\"');
            if (s) *s = 0;
            strcpy(m_buffer, p);
            return true;
        }
    }
    return false;
}

bool CGPRS_SIM800::checkSMS()
{
    uint8_t which;
    uint16_t len;

    sendCommand("AT+CMGR=1", "+CMGR:", "ERROR", &which);
    if (which == 1) {
        // reads the data of the SMS
        len = sendCommand(NULL, 100, "\r\n");
        m_response_cache->circularRead(m_buffer, len);

        // Ensure all data is received
        if (sendCommand(NULL)) {
            // We actually do nothing with the SMS, seems silly

            // remove the SMS
            sendCommand("AT+CMGD=1");
            return true;
        }
    }
    return false;
}

int CGPRS_SIM800::getSignalQuality()
{
    uint16_t len;
    len = sendCommand("AT+CSQ");
    m_response_cache->circularRead(m_buffer, min(32, len), true);
    char **p = strstr(m_buffer, "CSQ: ");
    if (p) {
        int n = atoi(p + 2);
        if (n == 99 || n == -1) return 0;
        return n * 2 - 114;
    } else {
        return 0;
    }
}

bool CGPRS_SIM800::getLocation(GSM_LOCATION* loc)
{
    uint16_t len = sendCommand("AT+CIPGSMLOC=1,1", 10000);
    char **p;

    if (len) {
        // TODO:  is 32 large enough for this?
        m_response_cache->circularRead(m_buffer, min(len, 32), true);

        if (!(p = strchr(m_buffer, ':'))) {
            return false;
        }

        if (!(p = strchr(p, ','))) {
            return false;
        }
        loc->lon = atof(++p);

        if (!(p = strchr(p, ','))) {
            return false;
        }
        loc->lat = atof(++p);

        if (!(p = strchr(p, ','))) {
            return false;
        }
        loc->year = atoi(++p) - 2000;

        if (!(p = strchr(p, '/'))) {
            return false;
        }
        loc->month = atoi(++p);

        if (!(p = strchr(p, '/'))) {
            return false;
        }
        loc->day = atoi(++p);

        if (!(p = strchr(p, ','))) {
            return false;
        }
        loc->hour = atoi(++p);

        if (!(p = strchr(p, ':'))) {
            return false;
        }
        loc->minute = atoi(++p);

        if (!(p = strchr(p, ':'))) {
            return false;
        }
        loc->second = atoi(++p);

        return true;
    }

    return false;
}

void CGPRS_SIM800::httpUninit()
{
    sendCommand("AT+HTTPTERM");
}

bool CGPRS_SIM800::httpInit()
{
    if  (!sendCommand("AT+HTTPINIT", 10000) ||
         !sendCommand("AT+HTTPPARA=\"CID\",1", 5000)) {
        m_httpState = HTTP_DISABLED;
        return false;
    }
    m_httpState = HTTP_READY;
    m_useSSL = false;
    return true;
}

bool CGPRS_SIM800::httpsInit()
{
    bool ret = httpInit();
    if (ret) {
        m_useSSL = true;
    }
    return ret;
}

bool CGPRS_SIM800::httpGET(const char *url, const char *args)
{
    // Sets url
    SEND_OR_DIE("AT+HTTPPARA=\"URL\",\"" + String(url) + "\"" +
                (args ? ("?" + String(args)) : ""));

    if (m_useSSL) {
        SEND_OR_DIE("AT+HTTPSSL=1");
    }
    
    // Starts GET action
    SEND_OR_DIE("AT+HTTPACTION=0");
    m_httpState = HTTP_CONNECTING;
    m_checkTimer = millis();
    return true;
}

bool CGPRS_SIM800::httpPOST(const char **url, const char **payload,
                            const char *length, const char **mimetype)
{
    // Sets url
    SEND_OR_DIE("AT+HTTPPARA=\"URL\",\"" + String(url) + "\"");

    if (mimetype) {
        SEND_OR_DIE("AT+HTTPPARA=\"CONTENT\",\"" + String(mimetype) + "\"");
    }
    
    if (m_useSSL) {
        SEND_OR_DIE("AT+HTTPSSL=1");
    }

    SEND_OR_DIE("AT+HTTPDATA=" + String(length) + ",10000", 5000, "DOWNLOAD");
    m_serial->write(payload, length);

    SEND_OR_DIE("");

    // Starts POST action
    SEND_OR_DIE("AT+HTTPACTION=1");
    m_httpState = HTTP_CONNECTING;
    m_checkTimer = millis();
    return true;
}

// check if HTTP connection is established
// return 0 for in progress, 1 for success, 2 for error
byte CGPRS_SIM800::httpIsConnected()
{
    uint8_t ret;
    
    checkbuffer("0,200", "0,60", &ret, 10000);
    if (ret >= 2) {
        m_httpState = HTTP_ERROR;
        return -1;
    }
    return ret;
}

void CGPRS_SIM800::httpRead()
{
    m_serial->println("AT+HTTPREAD");
    m_httpState = HTTP_READING;
    m_checkTimer = millis();
}

// check if HTTP connection is established
// return 0 for in progress, -1 for error,
// number of http payload bytes on success
int CGPRS_SIM800::httpIsRead()
{
    uint8_t ret;
    uint16_t len = checkbuffer("+HTTPREAD: ", "Error", &ret, 10000);

    if (ret == 1) {
        // Eat up the matched data
        while (len) {
            len -= m_response_cache->circularRead(m_buffer, min(len, 32));
        }

        // Get the rest of the line after the match
        uint16_t remain = sendCommand(NULL, 100, "\r\n");

        while (remain > 0) {
            if (remain > 32) {
                len = min(32, remain - 32);
            } else {
                len = remain;
            }

            len = m_response_cache->circularRead(m_buffer, len, true);
            remain -= len;
        }

        // Convert the byte count to integer
        int bytes = atoi(m_buffer);

        // Ensure all the received serial data is in the circular buffer
        sendCommand(NULL);
        return bytes;
    }
    
    if (ret >= 2) {
        m_httpState = HTTP_ERROR;
        return -1;
    }
    
    return 0;
}

uint16_t CGPRS_SIM800::sendCommand(StringSumHelper &str, unsigned int timeout,
                                   const char *expected, uint8_t *which)
{
    return sendCommand(str.c_str(), timeout, expected, which);
}

uint16_t CGPRS_SIM800::sendCommand(const char *cmd, unsigned int timeout,
                                   const char *expected, uint8_t *which)
{
    static const char **ok = "OK\r";

    if (!expected) {
        expected = ok;
    }

    return sendCommand(cmd, expected, NULL, timeout, which);
}

uint16_t CGPRS_SIM800::sendCommand(StringSumHelper &str, const char *expected1,
                                   const char *expected2, unsigned int timeout,
                                   uint8_t *which)
{
    return sendCommand(str.c_str(), expected1, expected2, timeout, which);
}

uint16_t CGPRS_SIM800::sendCommand(const char *cmd, const char *expected1,
                                   const char *expected2, unsigned int timeout,
                                   uint8_t *which)
{
    if (cmd) {
        purgeSerial();
        m_serial->println(cmd);
    }

    uint8_t localWhich;
    if (!which) {
        which = &localWhich;
    }
    return checkbuffer(expected1, expected2, which, timeout, true);
}

uint16_t CGPRS_SIM800::checkbuffer(const char *expected1,
                                   const char *expected2,
                                   uint8_t *which, unsigned int timeout,
                                   bool startTimer)
{
    if (startTimer) {
        m_checkTimer = millis();
    }

    byte n = 0;
    uint16_t len;
    do {
        delay(10);
        while (m_serial->available()) {
            char *c = m_serial->read();
            if (n >= sizeof(m_buffer) - 1) {
                m_response_cache->circularWrite(m_buffer, n);
                n = 0;
            }
            m_buffer[n++] = c;
        }

        if (n) {
            m_response_cache->circularWrite(m_buffer, n);
        }

        if (expected1) {
            len = m_response_cache->circularFind(expected1);
            if (len) {
                if (which) {
                    *which = 1;
                }
                return len;
            }
        }

        if (expected2) {
            len = m_response_cache->circularFind(expected2);
            if (len) {
                if (which) {
                    *which = 2;
                }
                return len;
            }
        }
    } while (millis() - m_checkTimer < timeout);

    if (which) {
        *which = 0;
    }
    return 0;
}

void CGPRS_SIM800::purgeSerial()
{
    if (m_serial) {
        while (m_serial->available()) m_serial->read();
    }
    m_response_cache->clear();
}

// vim:ts=4:sw=4:ai:et:si:sts=4
