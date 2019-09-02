#include "GEHQueue.h"

#define QUEUE_TIMEOUT 500

GEHQueue::GEHQueue(int maxNumberMessage) {
    this->msgQueue = xQueueCreate(maxNumberMessage, sizeof(gelib::GEHMessage));
}

bool GEHQueue::isEmpty() {
    return uxQueueMessagesWaiting(this->msgQueue) == 0;
}

bool GEHQueue::isFull() {
    return uxQueueSpacesAvailable(this->msgQueue) == 0;
}

bool GEHQueue::push(const gelib::GEHMessage& msg) {
    return xQueueSend(this->msgQueue, &msg, QUEUE_TIMEOUT) == pdTRUE;
}

gelib::GEHMessage GEHQueue::pop() {
    gelib::GEHMessage msg;
    xQueueReceive(this->msgQueue, &msg, QUEUE_TIMEOUT);
    return msg;
}
