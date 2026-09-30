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

        //splitting the command line into args in-place, by turning the spaces into '\0'. args[0] is the command name, the rest are its arguments
        char *args[16]; //max 15 args, the last slot is always NULL (like the args array of execvp())
        int argc = 0;
        char *p = cmdline;
        while(*p) {
            while(*p == ' ') {*p++ = '\0';} //skipping spaces
            if(*p == '\0') {break;}
            if(argc == 15) {
                printf("too many arguments (~_~;)\n");
                goto prompt;
            }
            args[argc++] = p;
            while(*p && *p != ' ') {p++;}
        }
        args[argc] = NULL;
        if(argc == 0) {goto prompt;} //empty line, nothing to run

        if(strcmp(args[0], "hello") == 0) {
            printf("Konnichiwa! Welcome to UserLand. This is your trusty shell (^o^)\n");
        }
        else if (strcmp(args[0], "readfile") == 0) {
            char buf[128];
            int len = readfile("konnichiwa.txt", buf, sizeof(buf));
            buf[len] = '\0';
            printf("%s\n", buf);
        }
        else if (strcmp(args[0], "writefile") == 0) {
            writefile("konnichiwa.txt", "Konnichiwa! (^o^)\n", 19);
        }
        else if(strcmp(args[0], "echo") == 0) {
            bool new_line = true; //default echo ends with \n, but, not if -n passed as a flag
            int i = 1; //index of arg to read (used only to skip -n flag if inserted)
            if(args[1] != NULL && strcmp(args[1], "-n") == 0) {new_line = false; i++;}

            for(; args[i]; i++) {
                printf("%s", args[i]);
                if(args[i+1] != NULL) {printf(" ");}
            }
            if(new_line) {printf("\n");}
        }
        else if(strcmp(args[0], "pwd") == 0) {
            char cwd[CWD_MAX];
            if(getcwd(cwd, sizeof(cwd)) < 0) {printf("pwd: getcwd failed\n");}
            else {printf("%s\n", cwd);}
        }
        else if(strcmp(args[0], "exit") == 0) {exit();}
        else {printf("unknown command: %s\n", args[0]);}
    }
}
