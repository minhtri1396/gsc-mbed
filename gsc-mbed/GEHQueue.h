#ifndef GEHQUEUE_H_
#define GEHQUEUE_H_

#include "GEHImport.h"

namespace gelib {
    struct GEHMessage {
        size_t length;
        uint8_t *content;
    };
}

class GEHQueue {
private:
    QueueHandle_t msgQueue;
public:
    GEHQueue(int maxNumberMessage);
    bool isEmpty();
    bool isFull();
    bool push(const gelib::GEHMessage& msg);
    gelib::GEHMessage pop();
};

#endif // GEHQUEUE_H_