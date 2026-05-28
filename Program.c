#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "Engine.h"

void set_color(WORD color) {
    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    SetConsoleTextAttribute(hConsole, color);
}

bool initial_output_done = false; 
bool prompt_printed = false;

void clear_prompt() {
    if (!prompt_printed) return;
    
    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    GetConsoleScreenBufferInfo(hConsole, &csbi);
    
    csbi.dwCursorPosition.X = 0;
    SetConsoleCursorPosition(hConsole, csbi.dwCursorPosition);
    
    DWORD written;
    FillConsoleOutputCharacter(hConsole, ' ', csbi.dwSize.X, csbi.dwCursorPosition, &written);
    prompt_printed = false;
}

void reprint_prompt() {
    if (prompt_printed) return;
    
    set_color(FOREGROUND_INTENSITY);
    printf("> ");
    set_color(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
    fflush(stdout);
    prompt_printed = true;
}

void on_engine_log(const char *msg) {
    clear_prompt();
    set_color(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
    printf("[LOG]: %s\n", msg);
    if (initial_output_done) {
        reprint_prompt();
    }
}

void on_engine_trace_raw(const char *from, const char *to, bool state) {
    clear_prompt();
    set_color(FOREGROUND_GREEN | FOREGROUND_BLUE);
    if (to == NULL || strlen(to) == 0) {
        printf("%s : %s\n", from, state ? "1" : "0");
    } else {
        printf("%s -> %s (%s)\n", from, to, state ? "1" : "0");
    }
    if (initial_output_done) {
        reprint_prompt();
    }
}

void print_logo() {
    set_color(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY);
    printf("\n"
           "      \xDB\xDB\xDB\xDB\xDB\xDB\xDB       \n"
           "          \xDB\xDB\xDB\xDB\xDB     \n"
           "     \xDB\xDB\xDB\xDB\xDB\xDB\xDB\xDB\xDB\xDB     \n"
           "     \xDB\xDB\xDB\xDB\xDB          \n"
           "       \xDB\xDB\xDB\xDB\xDB\xDB\xDB      \n\n"
           " NOT Engine v1.4.2\n\n");
    set_color(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
}

char* read_all_text(const char *filename) {
    FILE *f = fopen(filename, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(size + 1);
    fread(buf, 1, size, f);
    buf[size] = '\0';
    fclose(f);
    return buf;
}

int main(int argc, char *argv[]) {
    if (argc < 2) return 0;

    print_logo(); 
    
    char *code = read_all_text(argv[1]);
    if (!code) return 0;
    
    EngineCore engine;
    engine_init(&engine);
    
    engine.on_log = on_engine_log;
    engine.on_trace_raw = on_engine_trace_raw;
    
    engine_compile(&engine, code); 
    free(code);
    
    char *raw_buttons = engine_get_buttons_raw(&engine);
    if (strlen(raw_buttons) > 0) {
        printf("Available Buttons:\n");
        char *btn = strtok(raw_buttons, ";");
        while (btn != NULL) {
            printf("- %s\n", btn);
            btn = strtok(NULL, ";");
        }
    }
    free(raw_buttons);

    initial_output_done = true;
    char input_buf[256];
    reprint_prompt();
    
    while (engine.is_running) {
        if (fgets(input_buf, sizeof(input_buf), stdin) == NULL) continue;
        input_buf[strcspn(input_buf, "\r\n")] = 0;
        
        prompt_printed = false;
        
        int code_res = engine_execute(&engine, input_buf);
        
        switch (code_res) {
            case 1:  printf("Trace Mode: ON\n");  break;
            case 2:  printf("Trace Mode: OFF\n"); break;
            case -1:
                set_color(FOREGROUND_RED | FOREGROUND_INTENSITY);
                printf("Unknown Command\n");
                set_color(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
                break;
        }
        reprint_prompt();
    }
    
    engine_free(&engine);
    return 0;
}