#include "user.h"

void main(void) {
    while(1) {
prompt:
        printf("*\(^o^)/* >> ");
        char cmdline[128]; //a single line of cmd can be of max 128 chars
        
        for(int i = 0; ; i++) {
            char ch = get_char();
            put_char(ch);
            if(i == sizeof(cmdline) - 1) {
                printf("command line too long (~_~;)\n");
                goto prompt;
            }
            else if(ch == '\r') { //newline char in debug console is '\r'. basically, this case means "if Enter is pressed on the keyboard"
                printf("\n");
                cmdline[i] = '\0';
                break;
            }
            else {cmdline[i] = ch;}
        }

        if(strcmp(cmdline, "hello") == 0) {
            printf("Konnichiwa! Welcome to UserLand. This is your trusty shell (^o^)\n");
        }
        else if(strcmp(cmdline, "exit") == 0) {exit();}
        else {printf("unknown command: %s\n", cmdline);}
    }
}
