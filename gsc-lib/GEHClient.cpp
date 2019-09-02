#include "GEHClient.h"
#include "GEHErrorDefine.h"
#include <mbedtls/md.h>
#include <WiFi.h>
#include <pb_common.h>
#include <pb.h>
#include <pb_encode.h>
#include <pb_decode.h>
#include <gehub_message.pb.h>
#include <SPIFFS.h>


#define CHARACTER_END_STRING '\0'
#define MAX_LENGTH_READ_BUFFER 1600
#define MAX_LENGTH_WRITE_BUFFER 1600
#define MAX_NUMBER_MESSAGE 1024

#define VERSION "2.0"
#define FILE_CONFIG "/gsc-services.json"
#define URL_REGISTER "/v1/conn/register"
#define URL_RENAME_CONNECTION "/v1/conn/rename"

#define build_request(msgType, dest, encode_func, buffer, status)\
    gschub_Letter letter = gschub_Letter_init_default;\
    letter.type = msgType;\
    letter.data.funcs.encode = encode_func;\
    strcpy(letter.receiver, dest);\
    pb_ostream_t stream = pb_ostream_from_buffer(buffer, sizeof(buffer));\
    status = pb_encode(&stream, gschub_Letter_fields, &letter)


namespace gelib {
    namespace message {
        namespace {
            SemaphoreHandle_t xTableMutex;
            bool tableMessageID[MAX_NUMBER_MESSAGE];
        }

        void init() {
            if (xTableMutex != NULL) {
                return;
            }
            xTableMutex = xSemaphoreCreateMutex();
            xSemaphoreGive(xTableMutex);
        }

        uint16_t registerNext() {
            uint16_t msgID = MAX_NUMBER_MESSAGE;
            if (xSemaphoreTake(xTableMutex, (TickType_t) 100) != pdTRUE) { // Waiting 1500ms
                return msgID;
            }

            for (uint16_t idx = 0; idx < MAX_NUMBER_MESSAGE; ++idx) {
                if (tableMessageID[idx] == false) {
                    msgID = idx;
                    break;
                }
            }

            if (msgID < MAX_NUMBER_MESSAGE) {
                tableMessageID[msgID] = true;
            }
            xSemaphoreGive(xTableMutex);
            return msgID;
        }

        void unregister(uint16_t msgID) {
            if (xSemaphoreTake(xTableMutex, (TickType_t) 100) != pdTRUE) { // Waiting 1500ms
                return;
            }
            tableMessageID[msgID] = false;
            xSemaphoreGive(xTableMutex);
        }

        bool isRegistered(uint16_t msgID) {
            bool isRegistered = false;
            if (xSemaphoreTake(xTableMutex, (TickType_t) 100) != pdTRUE) { // Waiting 1500ms
                return isRegistered;
            }
            isRegistered = tableMessageID[msgID];
            xSemaphoreGive(xTableMutex);
            return isRegistered;
        }
    }

    GEHMessage g_RequestMessage;
    GEHMessage g_ReplyMessage;

    uint8_t *buildRequest(const uint8_t *content, size_t length) {
        uint8_t *msg = new uint8_t[length + 4]; // 4 bytes for value of length
        memcpy(msg + 4, content, length);
        for (int idx = 0; idx < 4; ++idx) {
            msg[idx] = (uint8_t)length;
            length >>= 8;
        }
        return msg;
    }

    gelib::GEHMessage buildResponse(const char *content, size_t length) {
        uint8_t *data = new uint8_t[length];
        memcpy(data, content, length);
        return gelib::GEHMessage {
            length,
            data
        };
    }

    JsonObject& buildParamsRegistration(const char *aliasName, const char *id, const char *password) {
        StaticJsonBuffer<512> req;
        JsonObject& params = req.createObject();
        params["Id"] = id;
        params["Token"] = password;
        params["AliasName"] = aliasName;
        params["Ver"] = VERSION;
        return params;
    }

    JsonObject& buildParamsRenameConnection(const char *aliasName, const char *id, const char *token) {
        StaticJsonBuffer<512> req;
        JsonObject& params = req.createObject();
        params["Id"] = id;
        params["Token"] = token;
        params["AliasName"] = aliasName;
        return params;
    }

    bool encodePingMessage(pb_ostream_t *stream, const pb_field_t *field, void * const *arg) {
        if (!pb_encode_tag_for_field(stream, field)) {
            return false;
        }
        return pb_encode_string(stream, (uint8_t*)"Ping", 4);
    }

    bool encodeRequestMessage(pb_ostream_t *stream, const pb_field_t *field, void * const *arg) {
        if (!pb_encode_tag_for_field(stream, field)) {
            return false;
        }
        return pb_encode_string(stream, g_RequestMessage.content, g_RequestMessage.length);
    }

    bool decodeReplyMessage(pb_istream_t *stream, const pb_field_t *field, void **arg) {
        if (stream->bytes_left > MAX_LENGTH_READ_BUFFER) {
            return false;
        }
        size_t size = stream->bytes_left;
        gelib::g_ReplyMessage.length = size;
        return pb_read(stream, g_ReplyMessage.content, size);
    }
}

GEHClient *GEHClient::Shared = new GEHClient();

GEHClient* const GEHClient::Instance() {
    return GEHClient::Shared;
}

GEHClient::GEHClient() {
    gelib::message::init();

    this->listener = NULL;
    this->isOpened = false;
    this->lastErrorID = GEH_ERROR_NONE;
    this->recvQueue = new GEHQueue(MAX_NUMBER_MESSAGE);
    this->writeQueue = new GEHQueue(MAX_NUMBER_MESSAGE);
}

GEHClient::~GEHClient() {
    delete this->recvQueue;
    delete this->writeQueue;
    if (this->listener != NULL) {
        delete this->listener;
    }
}

void GEHClient::nextMessage() {
    vTaskDelay(1);
    if (this->listener == NULL || this->recvQueue->isEmpty()) {
        return;
    }
    gelib::GEHMessage msg = this->recvQueue->pop();
    this->listener->onMessage(msg.content, msg.length);
    delete []msg.content;
}

uint8_t GEHClient::getLastError() {
    return this->lastErrorID;
}

void GEHClient::setListener(GEHListener *listener) {
    this->listener = listener;
}

void GEHClient::open(const char *aliasName) {
    if (this->isOpened) {
        return;
    }

    String strConfig = this->readConfig();
    if (strConfig.length() == 0) {
        return;
    }

    this->isOpened = true;
    JsonObject& config = StaticJsonBuffer<512>().parse(strConfig);
    this->baseURL = String(config["host"].as<char *>());
    strcpy(this->info.id, config["id"].as<char *>());
    strcpy(this->info.password, config["token"].as<char *>());
    strcpy(this->info.aliasName, aliasName);
    xTaskCreatePinnedToCore(
        GEHClient::loopAction,
        "GEHClient::loopAction",
        5 * 1024, // 5 Kb
        GEHClient::Shared,
        1,
        &this->loopActionTask,
        0
    );
}

bool GEHClient::renameConnection(const char *aliasName) {
    char resBuffer[512];
    char reqBuffer[512];

    if (this->isOpened == false) {
        return false;
    }

    JsonObject& params = gelib::buildParamsRenameConnection(
        aliasName,
        this->clientInfo.id,
        this->clientInfo.token
    );
    params.printTo(reqBuffer, 512);

    HTTPClient http;
    http.begin(this->baseURL + URL_RENAME_CONNECTION);
    http.addHeader("Content-Type", "application/json");
    if (http.POST(reqBuffer) <= 0) {
        http.end();
        return false;
    }
    http.getString().toCharArray(resBuffer, 512);
    http.end();

    JsonObject& res = StaticJsonBuffer<512>().parse(resBuffer);
    if (res.success() == false || res.containsKey("Data") == false || res["ReturnCode"] < 1) {
        return false;
    }

    strcpy(this->info.aliasName, aliasName);
    return true;
}

uint16_t GEHClient::writeMessage(const char *receiver, uint8_t *content, size_t length) {
    if (WiFi.status() != WL_CONNECTED) {
        this->lastErrorID = GEH_ERROR_DISCONNECTED;
        return 0;
    }

    if (this->writeQueue->isFull()) {
        this->lastErrorID = GEH_ERROR_NOT_ENOUGH_QUEUE;
        return 0;
    }

    uint16_t msgID = gelib::message::registerNext();
    if (msgID == MAX_NUMBER_MESSAGE) {
        this->lastErrorID = GEH_ERROR_NOT_ENOUGH_QUEUE;
        return 0;
    }

    // Build message
    bool status;
    gelib::g_RequestMessage.length = length;
    gelib::g_RequestMessage.content = content;
    uint8_t buffer[MAX_LENGTH_WRITE_BUFFER];
    build_request(gschub_Letter_Type_Single, receiver, gelib::encodeRequestMessage, buffer, status);
    if (!status) {
        return 0;
    }

    // Prepare message
    length = stream.bytes_written;
    uint8_t *req = new uint8_t[length + 2];
    memcpy(req + 2, buffer, length);
    memcpy(req, &msgID, 2);

    // Push message to queue
    bool result = this->writeQueue->push(gelib::GEHMessage{
        length,
        req
    });

    if (result) {
        this->lastErrorID = GEH_ERROR_NONE;
        return msgID;
    }
    delete []req;
    gelib::message::unregister(msgID);
    this->lastErrorID = GEH_ERROR_NOT_ENOUGH_QUEUE;
    return 0;
}

uint16_t GEHClient::writeMessage(const char *receiver, uint8_t *content, size_t length, uint16_t msgID) {
    if (msgID >= 0 && gelib::message::isRegistered(msgID)) {
        return msgID;
    }
    return this->writeMessage(receiver, content, length);
}

/////////////////////
// PRIVATE METHODS //
/////////////////////

void GEHClient::loopAction(void *param) {
    GEHClient *instance = (GEHClient *)param;
    while(1) {
        vTaskDelay(1);
        if (WiFi.status() != WL_CONNECTED) {
            continue;
        }

        if (instance->socket.connected() == false || instance->ping() == false) {
            instance->socket.stop();
            if (instance->connect() == false) {
                continue;
            }
        }

        instance->writeNextMessage();

        if (!instance->socket.available()) {
            continue;
        }
        gelib::GEHMessage msg = instance->readNextMessage();
        if (msg.length > 0 && instance->recvQueue->push(msg) == false) {
            delete []msg.content;
        }
    }
    vTaskDelete(&instance->loopActionTask);
}

bool GEHClient::connect() {
    char buffer[512];
    const char *id = this->info.id;
    const char *password = this->info.password;
    const char *aliasName = this->info.aliasName;

    if (this->registerConnection(aliasName, id, password, buffer) == false) {
        return false;
    }

    JsonObject& res = StaticJsonBuffer<512>().parse(buffer);
    if (res.success() == false || res.containsKey("Data") == false || res["ReturnCode"] < 1) {
        return false;
    }

    JsonObject& data = res["Data"];
    if (this->connectSocket(data["Host"].as<char *>()) == false) {
        return false;
    }

    return this->authenSocket(data["ID"].as<char *>(), data["Token"].as<char *>());
}

bool GEHClient::registerConnection(const char *aliasName, const char *id, const char *password, char *buffer) {
    char reqBuffer[512];

    JsonObject& params = gelib::buildParamsRegistration(aliasName, id, password);
    params.printTo(reqBuffer, 512);

    HTTPClient http;
    http.begin(this->baseURL + URL_REGISTER);
    http.addHeader("Content-Type", "application/json");
    if (http.POST(reqBuffer) <= 0) {
        http.end();
        return false;
    }

    http.getString().toCharArray(buffer, 512);
    http.end();
    return true;
}

bool GEHClient::connectSocket(const char *host) {
    try {
        char *socketIP = strtok((char *)host, ":");
        int socketPort = atoi(strtok (NULL, ":"));
        return this->socket.connect(socketIP, socketPort) == 1;
    }
    catch(const std::exception& e) {
        return false;
    }
}

bool GEHClient::authenSocket(const char *id, const char *token) {
    gschub_Ticket ticket = gschub_Ticket_init_default;
    strcpy(ticket.connID, id);
    strcpy(ticket.token, token);

    // Build message
    uint8_t buffer[gschub_Ticket_size];
    pb_ostream_t stream = pb_ostream_from_buffer(buffer, sizeof(buffer));
    bool status = pb_encode(&stream, gschub_Ticket_fields, &ticket);
    if (!status) {
        return false;
    }

    // Authenticate
    size_t len = stream.bytes_written;
    uint8_t *data = gelib::buildRequest((uint8_t *)buffer, len);
    const size_t numSendBytes = this->socket.write(data, len + 4);
    delete []data;

    if (numSendBytes != (len + 4)) {
        return false;
    }

    // Setup keys
    this->setupSecretKey();
    strcpy(this->clientInfo.id, id);
    strcpy(this->clientInfo.token, token);
    return true;
}

bool GEHClient::ping() {
    static volatile uint64_t lastTimePing = 0;
    uint64_t currentTime = millis();
    if (lastTimePing + 1000 > currentTime) { // Only ping every 1 seconds
        return true;
    }

    // Build message
    bool status;
    uint8_t buffer[512];
    build_request(gschub_Letter_Type_Ping, "", gelib::encodePingMessage, buffer, status);
    if (!status) {
        return false;
    }

    // Ping
    size_t len = stream.bytes_written;
    uint8_t *data = gelib::buildRequest((uint8_t *)buffer, len);
    const size_t numSendBytes = this->socket.write(data, len + 4);
    delete []data;
    if (numSendBytes != (len + 4)) {
        lastTimePing = currentTime + 1000;
        return false;
    }
    lastTimePing = currentTime;
    return true;
}

void GEHClient::setupSecretKey() {
    uint8_t header[4];
    size_t length;
    this->socket.readBytes(header, 4);
    memcpy(&length, header, 4);
    this->socket.readBytes(this->info.secretKey, length);
    this->info.secretKey[length] = CHARACTER_END_STRING;
}

void GEHClient::writeNextMessage() {
    static size_t pendingLength = -1;
    static uint8_t *pendingData = NULL;
    static uint16_t msgID = 0;

    // Get next message
    if (pendingData == NULL) {
        if (this->writeQueue->isEmpty()) {
            return;
        }
        gelib::GEHMessage msg = this->writeQueue->pop();
        pendingData = gelib::buildRequest(msg.content + 2, msg.length);
        memcpy(&msgID, msg.content, 2);
        pendingLength = msg.length + 4;
        delete []msg.content;
    }

    // Send message
    const size_t numSendBytes = this->socket.write(pendingData, pendingLength);
    if (numSendBytes != pendingLength) {
        return;
    }
    delete []pendingData;
    pendingLength = -1;
    pendingData = NULL;
    gelib::message::unregister(msgID);
}

gelib::GEHMessage GEHClient::readNextMessage() {
    uint8_t header[4];
    size_t length;
    this->socket.readBytes(header, 4);
    memcpy(&length, header, 4);

    uint8_t *body = new uint8_t[length];
    this->socket.readBytes(body, length);
    gelib::GEHMessage msg = this->parseReceivedMessage(body, length);
    delete []body;
    return msg;
}

gelib::GEHMessage GEHClient::parseReceivedMessage(const uint8_t *content, size_t length) {
    gelib::g_ReplyMessage = gelib::GEHMessage{
        0,
        new uint8_t[length]
    };

    // Parse message
    gschub_Reply reply = gschub_Reply_init_default;
    reply.data.funcs.decode = gelib::decodeReplyMessage;
    pb_istream_t istream = pb_istream_from_buffer(content, length);
    if (!pb_decode(&istream, gschub_Reply_fields, &reply)) {
        delete []gelib::g_ReplyMessage.content;
        return gelib::GEHMessage{
            0,
            NULL
        };
    }

    // Validate message
    if (!this->validateMessage(
        gelib::g_ReplyMessage.content,
        gelib::g_ReplyMessage.length,
        reply.HMAC.bytes
    )) {
        delete []gelib::g_ReplyMessage.content;
        return gelib::GEHMessage{
            0,
            NULL
        };
    }
    return gelib::g_ReplyMessage;
}

bool GEHClient::validateMessage(const uint8_t *content, size_t length, const uint8_t *expectedHMAC) {
    byte hmacResult[32];
    mbedtls_md_context_t ctx;
    mbedtls_md_type_t md_type = MBEDTLS_MD_SHA256;
    mbedtls_md_init(&ctx);
    mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(md_type), 1);
    mbedtls_md_hmac_starts(&ctx, (const unsigned char *) this->info.secretKey, strlen(this->info.secretKey));
    mbedtls_md_hmac_update(&ctx, content, length);
    mbedtls_md_hmac_finish(&ctx, hmacResult);
    mbedtls_md_free(&ctx);

    for(int idx = 0; idx < sizeof(hmacResult); idx++){
        if (hmacResult[idx] != expectedHMAC[idx]) {
            return false;
        }
    }
    return true;
}

String GEHClient::readConfig() {
    if (SPIFFS.begin() == false) {
        return "";
    }

    File file = SPIFFS.open(FILE_CONFIG, FILE_READ);
    if(!file){
        return "";
    }
    auto content = file.readStringUntil('}');
    content += '}';
    file.close();
    return content;
}
