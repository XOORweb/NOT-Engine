#ifndef ENGINE_H
#define ENGINE_H

#include <stdbool.h>

typedef struct Connection Connection;

typedef struct {
    int port;
    bool signal;
} InputPair;

typedef struct {
    char id[40];
    char alias[32];
    char type[16];
    char value[128];
    bool state;
    bool pending_state;
    bool was_active;
    
    InputPair *inputs;
    int inputs_count;
    int inputs_capacity;
    
    Connection *outgoing_connections;
    int conn_count;
    int conn_capacity;
} Gate;

struct Connection {
    Gate *from;
    Gate *to;
    int to_port;
};

typedef struct {
    Gate *gates;
    int gates_count;
    int gates_capacity;
    
    void (*on_log)(const char *msg);
    void (*on_trace_raw)(const char *from, const char *to, bool state);
    
    bool trace_mode;
    bool is_running;
    bool is_processing;
} EngineCore;

void engine_init(EngineCore *engine);
void engine_free(EngineCore *engine);
void engine_compile(EngineCore *engine, const char *code);
void engine_stabilize(EngineCore *engine);
int engine_execute(EngineCore *engine, const char *input);
char* engine_get_buttons_raw(EngineCore *engine);

#endif