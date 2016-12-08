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

CGPRS_SIM800::CGPRS_SIM800(Stream *serial, Stream *debug, int8_t reset_pin,
                           int8_t enable_pin, int8_t dtr_pin)
{
    m_httpState = HTTP_DISABLED;
    m_serial = serial;
    m_debug = debug;
    m_reset_pin = reset_pin;
    m_enable_pin = enable_pin;
    m_dtr_pin = dtr_pin;
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

byte CGPRS_SIM800::setup(const char* apn)
{
    bool success = false;
    for (byte n = 0; n < 30; n++) {
        if (sendCommand("AT+CREG?", 2000)) {
            char *p = strstr(m_buffer, "0,");
            if (p) {
                char mode = *(p + 2);
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

    if (!success)
        return 1;

    if (!sendCommand("AT+CGATT?"))
        return 2;

    if (!sendCommand("AT+SAPBR=3,1,\"Contype\",\"GPRS\""))
        return 3;

    m_serial->print("AT+SAPBR=3,1,\"APN\",\"");
    m_serial->print(apn);
    m_serial->println('\"');
    if (!sendCommand(0))
        return 4;

    sendCommand("AT+SAPBR=1,1", 10000);
    sendCommand("AT+SAPBR=2,1", 10000);

    sendCommand("AT+CMGF=1");    // sets the SMS mode to text
    sendCommand("AT+CPMS=\"SM\",\"SM\",\"SM\""); // selects the memory

    if (!success)
        return 5;

    return 0;
}

bool CGPRS_SIM800::powerdown(void)
{
    httpUninit();
    return sendCommand("AT+SAPBR=0,1");
}

bool CGPRS_SIM800::getOperatorName()
{
    // display operator name
    if (sendCommand("AT+COPS?", "OK\r", "ERROR\r") == 1) {
        char *p = strstr(m_buffer, ",\"");
        if (p) {
            p += 2;
            char *s = strchr(p, '\"');
            if (s) *s = 0;
            strcpy(m_buffer, p);
            return true;
        }
    }
    return false;
}

bool CGPRS_SIM800::checkSMS()
{
    if (sendCommand("AT+CMGR=1", "+CMGR:", "ERROR") == 1) {
        // reads the data of the SMS
        sendCommand(0, 100, "\r\n");
        if (sendCommand(0)) {
            // remove the SMS
            sendCommand("AT+CMGD=1");
            return true;
        }
    }
    return false;
}

int CGPRS_SIM800::getSignalQuality()
{
    sendCommand("AT+CSQ");
    char *p = strstr(m_buffer, "CSQ: ");
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
    if (sendCommand("AT+CIPGSMLOC=1,1", 10000)) do {
        char *p;
        if (!(p = strchr(m_buffer, ':'))) break;
        if (!(p = strchr(p, ','))) break;
        loc->lon = atof(++p);
        if (!(p = strchr(p, ','))) break;
        loc->lat = atof(++p);
        if (!(p = strchr(p, ','))) break;
        loc->year = atoi(++p) - 2000;
        if (!(p = strchr(p, '/'))) break;
        loc->month = atoi(++p);
        if (!(p = strchr(p, '/'))) break;
        loc->day = atoi(++p);
        if (!(p = strchr(p, ','))) break;
        loc->hour = atoi(++p);
        if (!(p = strchr(p, ':'))) break;
        loc->minute = atoi(++p);
        if (!(p = strchr(p, ':'))) break;
        loc->second = atoi(++p);
        return true;
    } while(0);
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

bool CGPRS_SIM800::httpGET(const char* url, const char* args)
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
    m_bytesRecv = 0;
    m_checkTimer = millis();
    return true;
}

bool CGPRS_SIM800::httpsPOST(const char *url, const char *payload,
                             const char length, const char *mimetype)
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
    m_bytesRecv = 0;
    m_checkTimer = millis();
    return true;
}

// check if HTTP connection is established
// return 0 for in progress, 1 for success, 2 for error
byte CGPRS_SIM800::httpIsConnected()
{
    byte ret = checkbuffer("0,200", "0,60", 10000);
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
    m_bytesRecv = 0;
    m_checkTimer = millis();
}

// check if HTTP connection is established
// return 0 for in progress, -1 for error, number of http payload bytes on success
int CGPRS_SIM800::httpIsRead()
{
    byte ret = checkbuffer("+HTTPREAD: ", "Error", 10000) == 1;
    if (ret == 1) {
        m_bytesRecv = 0;
        // read the rest data
        sendCommand(0, 100, "\r\n");
        int bytes = atoi(m_buffer);
        sendCommand(0);
        bytes = min(bytes, sizeof(m_buffer) - 1);
        m_buffer[bytes] = 0;
        return bytes;
    } else if (ret >= 2) {
        m_httpState = HTTP_ERROR;
        return -1;
    }
    return 0;
}

byte CGPRS_SIM800::sendCommand(const char* cmd, unsigned int timeout,
                               const char* expected)
{
    if (cmd) {
        purgeSerial();
        if (m_debug) {
            m_debug->print('>');
            m_debug->println(cmd);
        }
        m_serial->println(cmd);
    }
    uint32_t t = millis();
    byte n = 0;
    do {
        if (m_serial->available()) {
            char c = m_serial->read();
            if (n >= sizeof(m_buffer) - 1) {
                // buffer full, discard first half
                n = sizeof(m_buffer) / 2 - 1;
                memcpy(m_buffer, m_buffer + sizeof(m_buffer) / 2, n);
            }
            m_buffer[n++] = c;
            m_buffer[n] = 0;
            if (strstr(m_buffer, expected ? expected : "OK\r")) {
                if (m_debug) {
                    m_debug->print("[1]");
                    m_debug->println(m_buffer);
                }
                return n;
            }
        }
    } while (millis() - t < timeout);

    if (m_debug) {
        m_debug->print("[0]");
        m_debug->println(m_buffer);
    }
    return 0;
}

byte CGPRS_SIM800::sendCommand(const char* cmd, const char* expected1,
                               const char* expected2, unsigned int timeout)
{
    if (cmd) {
        purgeSerial();
        if (m_debug) {
            m_debug->print('>');
            m_debug->println(cmd);
        }
        m_serial->println(cmd);
    }
    uint32_t t = millis();
    byte n = 0;
    do {
        if (m_serial->available()) {
            char c = m_serial->read();
            if (n >= sizeof(m_buffer) - 1) {
                // buffer full, discard first half
                n = sizeof(m_buffer) / 2 - 1;
                memcpy(m_buffer, m_buffer + sizeof(m_buffer) / 2, n);
            }
            m_buffer[n++] = c;
            m_buffer[n] = 0;
            if (strstr(m_buffer, expected1)) {
                if (m_debug) {
                    m_debug->print("[1]");
                    m_debug->println(m_buffer);
                }
                return 1;
            }
            if (strstr(m_buffer, expected2)) {
                if (m_debug) {
                    m_debug->print("[2]");
                    m_debug->println(m_buffer);
                }
                return 2;
            }
        }
    } while (millis() - t < timeout);

    if (m_debug) {
        m_debug->print("[0]");
        m_debug->println(m_buffer);
    }
    return 0;
}

byte CGPRS_SIM800::checkbuffer(const char* expected1, const char* expected2,
                               unsigned int timeout)
{
    while (m_serial->available()) {
        char c = m_serial->read();
        if (m_bytesRecv >= sizeof(m_buffer) - 1) {
            // buffer full, discard first half
            m_bytesRecv = sizeof(m_buffer) / 2 - 1;
            memcpy(m_buffer, m_buffer + sizeof(m_buffer) / 2, m_bytesRecv);
        }
        m_buffer[m_bytesRecv++] = c;
        m_buffer[m_bytesRecv] = 0;
        if (strstr(m_buffer, expected1)) {
            return 1;
        }
        if (expected2 && strstr(m_buffer, expected2)) {
            return 2;
        }
    }
    return (millis() - m_checkTimer < timeout) ? 0 : 3;
}

void CGPRS_SIM800::purgeSerial()
{
    if (m_serial) {
        while (m_serial->available()) m_serial->read();
    }
}

// vim:ts=4:sw=4:ai:et:si:sts=4
