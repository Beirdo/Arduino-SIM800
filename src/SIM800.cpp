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

CGPRS_SIM800::CGPRS_SIM800(HardwareSerial *serial, int8_t reset_pin,
                           int8_t enable_pin, int8_t dtr_pin)
{
    m_httpState = HTTP_DISABLED;
    m_serial = serial;
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
    if (sendCommand(F("AT"))) {
        sendCommand(F("AT+IPR=115200"));
        sendCommand(F("ATE0"));
        sendCommand(F("AT+CFUN=1"), 10000);
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

byte CGPRS_SIM800::setup(Cache_Segment *apn_cache)
{
    bool success = false;
    uint16_t len;
    for (byte n = 0; n < 30; n++) {
        len = sendCommand(F("AT+CREG?"), 2000);
        if (len) {
            m_response_cache->circularRead(m_buffer, min(len, 32), true);
            char *p = (char *)strstr((char *)m_buffer, "0,");
            if (p) {
                char mode = *(p + 2);
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

    if (!sendCommand(F("AT+CGATT?"))) {
        return 2;
    }

    if (!sendCommand(F("AT+SAPBR=3,1,\"Contype\",\"GPRS\""))) {
        return 3;
    }

    m_serial->print(F("AT+SAPBR=3,1,\"APN\",\""));

    uint16_t i = 0;
    char ch;
    do {
        ch = apn_cache->read(i);
        if (ch) {
            m_serial->print(ch);
        }
    } while(ch);
    if (!sendCommand("\"")) {
        return 4;
    }

    sendCommand(F("AT+SAPBR=1,1"), 10000);
    sendCommand(F("AT+SAPBR=2,1"), 10000);

    // sets the SMS mode to text
    sendCommand(F("AT+CMGF=1"));

    // selects the memory
    if (!sendCommand(F("AT+CPMS=\"SM\",\"SM\",\"SM\""))) {
        return 5;
    }

    return 0;
}

bool CGPRS_SIM800::powerdown(void)
{
    httpUninit();
    return !(!(sendCommand(F("AT+SAPBR=0,1"))));
}

bool CGPRS_SIM800::getOperatorName()
{
    uint8_t which;

    // display operator name
    uint16_t len = sendCommand(F("AT+COPS?"), "OK\r", "ERROR\r", 2000,
                               &which);
    if (which == 1) {
        // Eat the matched data
        m_response_cache->circularRead(m_buffer, min(len, 32));

        // Read the data AFTER the matched data
        m_response_cache->circularRead(m_buffer, 32, true);
        char *p = (char *)strstr((char *)m_buffer, ",\"");
        if (p) {
            p += 2;
            char *s = (char *)strchr(p, '\"');
            if (s) {
                *s = 0;
            }
            strcpy((char *)m_buffer, (const char *)p);
            return true;
        }
    }
    return false;
}

bool CGPRS_SIM800::checkSMS()
{
    uint8_t which;
    uint16_t len;

    sendCommand(F("AT+CMGR=1"), "+CMGR:", "ERROR", 2000, &which);
    if (which == 1) {
        // reads the data of the SMS
        len = sendCommand((char *)NULL, 100, "\r\n");
        m_response_cache->circularRead(m_buffer, len);

        // Ensure all data is received
        if (sendCommand((char *)NULL)) {
            // We actually do nothing with the SMS, seems silly

            // remove the SMS
            sendCommand(F("AT+CMGD=1"));
            return true;
        }
    }
    return false;
}

int CGPRS_SIM800::getSignalQuality()
{
    uint16_t len;
    len = sendCommand(F("AT+CSQ"));
    m_response_cache->circularRead(m_buffer, min(32, len), true);
    char *p = (char *)strstr((char *)m_buffer, "CSQ: ");
    if (p) {
        int n = atoi(p + 2);
        if (n == 99 || n == -1) {
            return 0;
        }
        return n * 2 - 114;
    }

    return 0;
}

bool CGPRS_SIM800::getLocation(char *loc, uint8_t maxlen)
{
    uint16_t len = sendCommand(F("AT+CIPGSMLOC=1,1"), 10000);
    uint16_t count;
    char *p;

    if (len >= maxlen) {
        return false;
    }

    if (len) {
        count = m_response_cache->circularRead(m_buffer, min(len, 32), true);

        // Toss the +CIPGSMLOC:
        if (!(p = (char *)strchr((char *)m_buffer, ':'))) {
            return false;
        }

        // Skip the space
        p++;
        // the firtt term is 0 on success, else it's a failure
        if (*p != '0') {
            return false;
        }

        // Skip the comma
        while (*p && *p != ',') {
            p++;
        }

        if (!*p) {
            return false;
        }
        p++;

        // Copy the rest of the buffer
        count -= (p - (char *)m_buffer);
        strncpy(loc, p, count);

        len -= count;

        // pull the rest of the circular buffer, up to maxlen
        while (len) {
            count = m_response_cache->circularRead(m_buffer, min(len, 32),
                                                   true);
            strncat(loc, (const char *)m_buffer, count);
            len -= count;
        }

        return true;
    }

    return false;
}

void CGPRS_SIM800::httpUninit()
{
    sendCommand(F("AT+HTTPTERM"));
}

bool CGPRS_SIM800::httpInit()
{
    if  (!sendCommand(F("AT+HTTPINIT"), 10000) ||
         !sendCommand(F("AT+HTTPPARA=\"CID\",1"), 5000)) {
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

bool CGPRS_SIM800::httpGET(Cache_Segment *url_cache, const char *args)
{
    uint16_t i;
    char ch;

    // Sets url
    m_serial->print(F("AT+HTTPPARA=\"URL\",\""));
    i = 0;
    do {
        ch = url_cache->read(i);
        if (ch) {
            m_serial->print(ch);
        }
    } while(ch);
    if (args) {
        m_serial->print("?");
        m_serial->print(args);
    }
    SEND_OR_DIE("\"");

    if (m_useSSL) {
        SEND_OR_DIE(F("AT+HTTPSSL=1"));
    }

    // Starts GET action
    SEND_OR_DIE(F("AT+HTTPACTION=0"));
    m_httpState = HTTP_CONNECTING;
    m_checkTimer = millis();
    return true;
}

bool CGPRS_SIM800::httpPOST(Cache_Segment *url_cache,
                            Cache_Segment *payload_cache,
                            Cache_Segment *mime_cache)
{
    uint16_t i;
    char ch;
    uint16_t length;

    // Sets url
    m_serial->print(F("AT+HTTPPARA=\"URL\",\""));
    i = 0;
    do {
        ch = url_cache->read(i);
        if (ch) {
            m_serial->print(ch);
        }
    } while(ch);
    SEND_OR_DIE("\"");

    if (mime_cache) {
        m_serial->print(F("AT+HTTPPARA=\"CONTENT\",\""));
        i = 0;
        do {
            ch = mime_cache->read(i);
            if (ch) {
                m_serial->print(ch);
            }
        } while(ch);
        SEND_OR_DIE("\"");
    }

    if (m_useSSL) {
        SEND_OR_DIE(F("AT+HTTPSSL=1"));
    }

    length = payload_cache->circularReadAvailable();
    m_serial->print(F("AT+HTTPDATA="));
    m_serial->print(length, DEC);
    SEND_OR_DIE(F(",10000"), 5000, "DOWNLOAD");

    uint16_t count = 0;

    for (i = 0; i < length; i += count) {
        count = payload_cache->circularRead(m_buffer, min(32, length - i));
        if (!count) {
            break;
        }
        m_serial->write((char *)m_buffer, count);
    }

    SEND_OR_DIE("");

    // Starts POST action
    SEND_OR_DIE(F("AT+HTTPACTION=1"));
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
    m_serial->println(F("AT+HTTPREAD"));
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
        uint16_t remain = sendCommand((char *)NULL, 100, "\r\n");

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
        int bytes = atoi((char *)m_buffer);

        // Ensure all the received serial data is in the circular buffer
        sendCommand((char *)NULL);
        return bytes;
    }

    if (ret >= 2) {
        m_httpState = HTTP_ERROR;
        return -1;
    }

    return 0;
}

uint16_t CGPRS_SIM800::sendCommand(const char *cmd, unsigned int timeout,
                                   const char *expected, uint8_t *which)
{
    static const char *ok = "OK\r";

    if (!expected) {
        expected = ok;
    }

    return sendCommand(cmd, expected, NULL, timeout, which);
}

uint16_t CGPRS_SIM800::sendCommand(const __FlashStringHelper *cmd,
                                   unsigned int timeout,
                                   const char *expected, uint8_t *which)
{
    static const char *ok = "OK\r";

    if (!expected) {
        expected = ok;
    }

    return sendCommand(cmd, expected, NULL, timeout, which);
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

uint16_t CGPRS_SIM800::sendCommand(const __FlashStringHelper *cmd,
                                   const char *expected1,
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
            char c = m_serial->read();
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
