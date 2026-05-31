#include "Engine.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <windows.h>
#ifndef GetTickCount64
#define GetTickCount64() ((ULONGLONG)GetTickCount())
#endif
#include <stdint.h>
#include <stdbool.h>

static CRITICAL_SECTION g_engine_lock;
static bool g_lock_initialized = false;

typedef struct {
    EngineCore *engine;
    char gate_alias[32];
    bool input_active;
    ULONGLONG target_time;
    uint64_t id;
} TimerEvent;

static TimerEvent *g_timer_queue = NULL;
static int g_queue_count = 0;
static int g_queue_capacity = 0;
static uint64_t g_event_sequence = 0;

static HANDLE g_scheduler_thread = NULL;
static HANDLE g_scheduler_event = NULL;
static CRITICAL_SECTION g_scheduler_lock;
static bool g_scheduler_initialized = false;
static bool g_scheduler_running = true;

void engine_stabilize_internal(EngineCore *engine);
Gate* find_gate_by_alias(EngineCore *engine, const char *alias);
void gate_update_input(Gate *g, int port, bool signal);

static inline int get_gate_port_signal(Gate *g, int port_idx) {
    if (!g || !g->inputs) return 0;
    for (int i = 0; i < g->inputs_count; i++) {
        if (g->inputs[i].port == port_idx) {
            return g->inputs[i].signal ? 1 : 0;
        }
    }
    return 0;
}
static inline void trigger_trace(EngineCore *engine, const char *from, const char *to, int port, bool state) {
    if (engine->trace_mode && engine->on_trace_raw) {
        if (to == NULL || port < 0) {
            engine->on_trace_raw(from, NULL, state);
        } else {
            Gate *target_gate = find_gate_by_alias(engine, to);
            
            int p1_state = get_gate_port_signal(target_gate, 0);
            int p2_state = get_gate_port_signal(target_gate, 1);

            if (port == 0) p1_state = state ? 1 : 0;
            if (port == 1) p2_state = state ? 1 : 0;

            char target_buf[128];
            sprintf(target_buf, "%s@P1(%d),P2(%d)", to, p1_state, p2_state); 
            
            engine->on_trace_raw(from, target_buf, state);
        }
    }
}

DWORD WINAPI scheduler_thread_proc(LPVOID param) {
    while (g_scheduler_running) {
        ULONGLONG now = GetTickCount64();
        DWORD timeout = INFINITE;
        bool execute_now = false;
        TimerEvent ev;

        EnterCriticalSection(&g_scheduler_lock);
        if (g_queue_count > 0) {
            if (g_timer_queue[0].target_time <= now) {
                execute_now = true;
                ev = g_timer_queue[0];

                for (int i = 1; i < g_queue_count; i++) {
                    g_timer_queue[i - 1] = g_timer_queue[i];
                }
                g_queue_count--;
            } else {
                timeout = (DWORD)(g_timer_queue[0].target_time - now);
            }
        }
        LeaveCriticalSection(&g_scheduler_lock);

        if (execute_now) {
            if (g_lock_initialized) EnterCriticalSection(&g_engine_lock);
            
            Gate *g = find_gate_by_alias(ev.engine, ev.gate_alias);
            if (g) {
                g->state = ev.input_active;
                
                trigger_trace(ev.engine, g->alias, NULL, -1, g->state);

                for (int i = 0; i < g->conn_count; i++) {
                    Connection *conn = &g->outgoing_connections[i];
                    trigger_trace(ev.engine, g->alias, conn->to->alias, conn->to_port, g->state);
                    gate_update_input(conn->to, conn->to_port, g->state);
                }
                engine_stabilize_internal(ev.engine);
            }
            
            if (g_lock_initialized) LeaveCriticalSection(&g_engine_lock);
        } else {
            WaitForSingleObject(g_scheduler_event, timeout);
        }
    }
    return 0;
}

void schedule_timer_event(EngineCore *engine, const char *alias, bool input_active, int delay) {
    EnterCriticalSection(&g_scheduler_lock);

    if (g_queue_count >= g_queue_capacity) {
        g_queue_capacity = g_queue_capacity == 0 ? 16 : g_queue_capacity * 2;
        g_timer_queue = realloc(g_timer_queue, g_queue_capacity * sizeof(TimerEvent));
    }

    TimerEvent ev;
    ev.engine = engine;
    strcpy(ev.gate_alias, alias);
    ev.input_active = input_active;
    ev.target_time = GetTickCount64() + delay;
    ev.id = g_event_sequence++;

    int i = g_queue_count - 1;
    while (i >= 0) {
        if (g_timer_queue[i].target_time > ev.target_time ||
           (g_timer_queue[i].target_time == ev.target_time && g_timer_queue[i].id > ev.id)) {
            g_timer_queue[i + 1] = g_timer_queue[i];
            i--;
        } else {
            break;
        }
    }
    g_timer_queue[i + 1] = ev;
    g_queue_count++;

    LeaveCriticalSection(&g_scheduler_lock);
    SetEvent(g_scheduler_event);
}

void generate_guid(char *buf) {
    static int counter = 0;
    sprintf(buf, "%08x-%04x-%04x-%04x-%08x%04x",
            (unsigned int)GetTickCount(), (unsigned int)GetCurrentProcessId(),
            (unsigned int)GetCurrentThreadId(), counter++,
            (unsigned int)rand(), (unsigned int)rand());
}

void gate_update_input(Gate *g, int port, bool signal) {
    for (int i = 0; i < g->inputs_count; i++) {
        if (g->inputs[i].port == port) {
            g->inputs[i].signal = signal;
            return;
        }
    }
    if (g->inputs_count >= g->inputs_capacity) {
        g->inputs_capacity = g->inputs_capacity == 0 ? 4 : g->inputs_capacity * 2;
        g->inputs = realloc(g->inputs, g->inputs_capacity * sizeof(InputPair));
    }
    g->inputs[g->inputs_count].port = port;
    g->inputs[g->inputs_count].signal = signal;
    g->inputs_count++;
}

void gate_add_connection(Gate *g, Connection conn) {
    if (g->conn_count >= g->conn_capacity) {
        g->conn_capacity = g->conn_capacity == 0 ? 4 : g->conn_capacity * 2;
        g->outgoing_connections = realloc(g->outgoing_connections, g->conn_capacity * sizeof(Connection));
    }
    g->outgoing_connections[g->conn_count++] = conn;
}

__declspec(dllexport) void engine_init(EngineCore *engine) {
    engine->gates = NULL;
    engine->gates_count = 0;
    engine->gates_capacity = 0;
    engine->on_log = NULL;
    engine->on_trace_raw = NULL;
    engine->trace_mode = false;
    engine->is_running = true;
    engine->is_processing = false;
    
    if (!g_lock_initialized) {
        InitializeCriticalSection(&g_engine_lock);
        g_lock_initialized = true;
    }

    if (!g_scheduler_initialized) {
        InitializeCriticalSection(&g_scheduler_lock);
        g_scheduler_event = CreateEvent(NULL, FALSE, FALSE, NULL);
        g_scheduler_running = true;
        g_scheduler_initialized = true;
        g_scheduler_thread = CreateThread(NULL, 0, scheduler_thread_proc, NULL, 0, NULL);
    }

    srand((unsigned int)GetTickCount());
}

void free_gate_internals(Gate *g) {
    if (g->inputs) free(g->inputs);
    if (g->outgoing_connections) free(g->outgoing_connections);
}

__declspec(dllexport) void engine_free(EngineCore *engine) {
    if (g_lock_initialized) EnterCriticalSection(&g_engine_lock);
    
    for (int i = 0; i < engine->gates_count; i++) {
        free_gate_internals(&engine->gates[i]);
    }
    if (engine->gates) free(engine->gates);
    engine->gates = NULL;
    engine->gates_count = 0;
    engine->gates_capacity = 0;
    
    if (g_lock_initialized) LeaveCriticalSection(&g_engine_lock);
}

Gate* find_gate_by_alias(EngineCore *engine, const char *alias) {
    for (int i = 0; i < engine->gates_count; i++) {
        if (strcmp(engine->gates[i].alias, alias) == 0) return &engine->gates[i];
    }
    return NULL;
}

int compare_inputs(const void *a, const void *b) {
    return ((InputPair*)a)->port - ((InputPair*)b)->port;
}

void engine_recompute(EngineCore *engine, Gate *g) {
    if (strcmp(g->type, "BUTTON") == 0) return;
    if (g->inputs_count > 1) {
        qsort(g->inputs, g->inputs_count, sizeof(InputPair), compare_inputs);
    }
    int count = g->inputs_count;
    bool *signals = malloc(count * sizeof(bool) + 1);
    for (int i = 0; i < count; i++) signals[i] = g->inputs[i].signal;
    
    if (strcmp(g->type, "LOG") == 0) {
        bool active = count > 0 && signals[0];
        if (active && !g->was_active) {
            if (engine->on_log) engine->on_log(strlen(g->value) > 0 ? g->value : "SIGNAL");
        }
        g->was_active = active; g->state = active;
    }
    else if (strcmp(g->type, "NOT") == 0) g->state = count > 0 ? !signals[0] : true;
    else if (strcmp(g->type, "AND") == 0) {
        bool all = count >= 2;
        for (int i = 0; i < count; i++) if (!signals[i]) all = false;
        g->state = all;
    }
    else if (strcmp(g->type, "OR") == 0) {
        bool any = false;
        for (int i = 0; i < count; i++) if (signals[i]) any = true;
        g->state = any;
    }
    else if (strcmp(g->type, "XOR") == 0) {
        int tc = 0; for (int i = 0; i < count; i++) if (signals[i]) tc++;
        g->state = (tc % 2 != 0);
    }
    else if (strcmp(g->type, "NOR") == 0) {
        bool any = false; for (int i = 0; i < count; i++) if (signals[i]) any = true;
        g->state = !any;
    }
    else if (strcmp(g->type, "NAND") == 0) {
        bool all = count >= 2; for (int i = 0; i < count; i++) if (!signals[i]) all = false;
        g->state = !all;
    }
    else if (strcmp(g->type, "XNOR") == 0) {
        int tc = 0; for (int i = 0; i < count; i++) if (signals[i]) tc++;
        g->state = count > 0 && (tc % 2 == 0);
    }
    else if (strcmp(g->type, "TIMER") == 0) {
        bool input_active = count > 0 && signals[0];
        if (input_active != g->pending_state) {
            g->pending_state = input_active;
            int delay = atoi(g->value);
            if (delay < 1) delay = 1000;

            schedule_timer_event(engine, g->alias, input_active, delay);
        }
    }
    free(signals);
}

void engine_stabilize_internal(EngineCore *engine) {
    bool changed = true;
    while (changed) {
        changed = false;
        for (int i = 0; i < engine->gates_count; i++) {
            Gate *g = &engine->gates[i];
            if (strcmp(g->type, "BUTTON") == 0) continue;
            
            bool old_state = g->state;
            engine_recompute(engine, g);
            
            if (g->state != old_state) {
                changed = true;
                
                trigger_trace(engine, g->alias, NULL, -1, g->state);
                
                for (int j = 0; j < g->conn_count; j++) {
                    Connection *conn = &g->outgoing_connections[j];
                    trigger_trace(engine, g->alias, conn->to->alias, conn->to_port, g->state);
                    gate_update_input(conn->to, conn->to_port, g->state);
                }
            }
        }
    }
}

__declspec(dllexport) void engine_stabilize(EngineCore *engine) {
    if (g_lock_initialized) EnterCriticalSection(&g_engine_lock);
    engine_stabilize_internal(engine);
    if (g_lock_initialized) LeaveCriticalSection(&g_engine_lock);
}

char* trim_whitespace(char *str) {
    char *end;
    while (isspace((unsigned char)*str)) str++;
    if (*str == 0) return str;
    end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) end--;
    end[1] = '\0';
    return str;
}

__declspec(dllexport) void engine_compile(EngineCore *engine, const char *code) {
    if (g_lock_initialized) EnterCriticalSection(&g_engine_lock);
    engine_free(engine);
    
    if (!g_lock_initialized) {
        InitializeCriticalSection(&g_engine_lock);
        g_lock_initialized = true;
        EnterCriticalSection(&g_engine_lock);
    }
    
    char *code_copy = malloc(strlen(code) + 1);
    strcpy(code_copy, code);
    char *line = strtok(code_copy, "\n\r");
    while (line != NULL) {
        char *comment = strstr(line, "//"); if (comment) *comment = '\0';
        char *trimmed = trim_whitespace(line);
        if (strlen(trimmed) == 0 || strchr(trimmed, '>') != NULL || strstr(trimmed, "IN)") != NULL) {
            line = strtok(NULL, "\n\r");
            continue;
        }
        
        char type_buf[16] = {0}, num_buf[16] = {0}, val_buf[128] = {0};
        int ptr = 0;
        if (!isalpha((unsigned char)trimmed[ptr])) { line = strtok(NULL, "\n\r"); continue; }
        while (isalpha((unsigned char)trimmed[ptr]) && ptr < 15) { type_buf[ptr] = trimmed[ptr]; ptr++; }
        type_buf[ptr] = '\0';
        
        int num_ptr = 0;
        while (isdigit((unsigned char)trimmed[ptr]) && num_ptr < 15) { num_buf[num_ptr] = trimmed[ptr]; ptr++; num_ptr++; }
        num_buf[num_ptr] = '\0';
        
        if (ptr > 0 && num_ptr > 0) {
            if (trimmed[ptr] == '(') {
                ptr++; int val_ptr = 0;
                while (trimmed[ptr] != ')' && trimmed[ptr] != '\0' && val_ptr < 127) val_buf[val_ptr++] = trimmed[ptr++];
                val_buf[val_ptr] = '\0';
            }
            
            if (engine->gates_count >= engine->gates_capacity) {
                engine->gates_capacity = engine->gates_capacity == 0 ? 16 : engine->gates_capacity * 2;
                engine->gates = realloc(engine->gates, engine->gates_capacity * sizeof(Gate));
            }
            Gate *g = &engine->gates[engine->gates_count++];
            memset(g, 0, sizeof(Gate));
            generate_guid(g->id);
            sprintf(g->alias, "%s%s", type_buf, num_buf);
            strcpy(g->type, type_buf); strcpy(g->value, val_buf);
        }
        line = strtok(NULL, "\n\r");
    }
    
    free(code_copy); code_copy = malloc(strlen(code) + 1); strcpy(code_copy, code);
    line = strtok(code_copy, "\n\r");
    while (line != NULL) {
        char *comment = strstr(line, "//"); if (comment) *comment = '\0';
        char *trimmed = trim_whitespace(line);
        char *arrow = strchr(trimmed, '>');
        char *open_paren = strchr(trimmed, '(');
        char *close_paren = strchr(trimmed, ')');
        
        if (arrow && open_paren && close_paren && open_paren < close_paren && close_paren < arrow) {
            char from_buf[32] = {0}; char to_buf[32] = {0}; int port_num = 0;
            int from_len = open_paren - trimmed;
            if (from_len > 31) from_len = 31;
            strncpy(from_buf, trimmed, from_len);
            sscanf(open_paren + 1, "%d", &port_num);
            char *dest = arrow + 1;
            while (*dest && isspace((unsigned char)*dest)) dest++;
            strncpy(to_buf, dest, 31);
            
            Gate *from_gate = find_gate_by_alias(engine, trim_whitespace(from_buf));
            Gate *to_gate = find_gate_by_alias(engine, trim_whitespace(to_buf));
            if (from_gate && to_gate) {
                Connection conn; conn.from = from_gate; conn.to = to_gate; conn.to_port = port_num - 1;
                gate_add_connection(from_gate, conn);
            }
        }
        line = strtok(NULL, "\n\r");
    }
    free(code_copy);
    engine_stabilize_internal(engine);
    if (g_lock_initialized) LeaveCriticalSection(&g_engine_lock);
}

__declspec(dllexport) char* engine_get_buttons_raw(EngineCore *engine) {
    if (g_lock_initialized) EnterCriticalSection(&g_engine_lock);
    char *buf = malloc(4096); buf[0] = '\0'; bool first = true;
    for (int i = 0; i < engine->gates_count; i++) {
        Gate *g = &engine->gates[i];
        if (strcmp(g->type, "BUTTON") == 0) {
            if (!first) strcat(buf, ";");
            strcat(buf, strlen(g->value) > 0 ? g->value : g->alias);
            first = false;
        }
    }
    if (g_lock_initialized) LeaveCriticalSection(&g_engine_lock);
    return buf;
}

__declspec(dllexport) int engine_execute(EngineCore *engine, const char *input) {
    char cmd[128]; strncpy(cmd, input, 127); cmd[127] = '\0';
    char *trimmed = trim_whitespace(cmd);
    for (int i = 0; trimmed[i]; i++) trimmed[i] = tolower((unsigned char)trimmed[i]);
    
    if (strcmp(trimmed, "exit") == 0) { engine->is_running = false; return 0; }
    if (strcmp(trimmed, "trace") == 0) { engine->trace_mode = !engine->trace_mode; return engine->trace_mode ? 1 : 2; }
    
    if (g_lock_initialized) EnterCriticalSection(&g_engine_lock);
    
    Gate *btn = NULL;
    for (int i = 0; i < engine->gates_count; i++) {
        Gate *g = &engine->gates[i];
        if (strcmp(g->type, "BUTTON") == 0) {
            if (stricmp(g->value, input) == 0 || stricmp(g->alias, input) == 0) { btn = g; break; }
        }
    }
    if (btn != NULL) {
        btn->state = true;
        trigger_trace(engine, btn->alias, NULL, -1, true);
        
        for (int i = 0; i < btn->conn_count; i++) {
            Connection *conn = &btn->outgoing_connections[i];
            trigger_trace(engine, btn->alias, conn->to->alias, conn->to_port, true);
            gate_update_input(conn->to, conn->to_port, true);
        }
        engine_stabilize_internal(engine);
        
        btn->state = false;
        trigger_trace(engine, btn->alias, NULL, -1, false);
        
        for (int i = 0; i < btn->conn_count; i++) {
            Connection *conn = &btn->outgoing_connections[i];
            trigger_trace(engine, btn->alias, conn->to->alias, conn->to_port, false);
            gate_update_input(conn->to, conn->to_port, false);
        }
        engine_stabilize_internal(engine);
        
        if (g_lock_initialized) LeaveCriticalSection(&g_engine_lock);
        return 3;
    }
    if (g_lock_initialized) LeaveCriticalSection(&g_engine_lock);
    return -1;
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    if (fdwReason == DLL_PROCESS_DETACH) {
        if (g_scheduler_initialized) {
            g_scheduler_running = false;
            SetEvent(g_scheduler_event);
            WaitForSingleObject(g_scheduler_thread, 500);
            
            CloseHandle(g_scheduler_thread);
            CloseHandle(g_scheduler_event);
            DeleteCriticalSection(&g_scheduler_lock);
            if (g_timer_queue) free(g_timer_queue);
            g_scheduler_initialized = false;
        }
        if (g_lock_initialized) {
            DeleteCriticalSection(&g_engine_lock);
            g_lock_initialized = false;
        }
    }
    return TRUE;
}