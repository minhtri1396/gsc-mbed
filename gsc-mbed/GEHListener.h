#ifndef GEHLISTENER_H_
#define GEHLISTENER_H_

class GEHListener {
public:
    virtual ~GEHListener() {};
    virtual void onMessage(const uint8_t *msg, const size_t length) = 0;
};

#endif // GEHLISTENER_H_