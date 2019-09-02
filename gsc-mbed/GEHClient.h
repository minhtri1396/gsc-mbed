#ifndef GEHCLIENT_H_
#define GEHCLIENT_H_

#include "GEHImport.h"
#include "GEHQueue.h"
#include "GEHListener.h"

struct GEHConnInfo {
    char id[26];
    char password[257];
    char aliasName [26];
    char secretKey[13];
};

struct GEHClientInfo {
    char id[26];
    char token[257];
};

class GEHClient {
private:
    static GEHClient *Shared;
    static void loopAction(void *param); // task loop

    TaskHandle_t loopActionTask;
    WiFiClient socket;

    bool isOpened;
    uint8_t lastErrorID;
    String baseURL;

    GEHQueue *recvQueue;
    GEHQueue *writeQueue;
    GEHListener *listener;
    GEHConnInfo info;
    GEHClientInfo clientInfo;

    GEHClient();
    bool connect();
    bool registerConnection(const char *aliasName, const char *id, const char *password, char *buffer);
    bool authenSocket(const char *id, const char *token);
    bool connectSocket(const char *host);

    bool ping();
    void setupSecretKey();
    void writeNextMessage();
    gelib::GEHMessage readNextMessage();
    gelib::GEHMessage parseReceivedMessage(const uint8_t *content, size_t length);
    bool validateMessage(const uint8_t *content, size_t length, const uint8_t *expectedHMAC);
    String readConfig();
public:
    static GEHClient* const Instance();
    ~GEHClient();

    uint8_t getLastError();
    void setListener(GEHListener *listener);
    void open(const char *aliasName);
    bool renameConnection(const char *aliasName);
    uint16_t writeMessage(const char *receiver, uint8_t *content, size_t length);
    uint16_t writeMessage(const char *receiver, uint8_t *content, size_t length, uint16_t msgID);

    // Invoke this method in main loop
    void nextMessage();
};

#endif // GEHCLIENT_H_
