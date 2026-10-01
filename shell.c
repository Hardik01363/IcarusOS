#include "user.h"

#define CMD_MAX 128
#define HIST_N 8

char hist[HIST_N][CMD_MAX];
int hist_n;

void prompt(void) {
    printf("\033[1;36m*\\(^o^)/* >>\033[0m ");
}

void hist_add(const char *s) {
    if(hist_n > 0 && strcmp(hist[hist_n - 1], s) == 0) {return;}
    if(hist_n == HIST_N) {
        for(int i = 0; i < HIST_N - 1; i++) {strcpy(hist[i], hist[i + 1]);}
        hist_n--;
    }
    strcpy(hist[hist_n++], s);
}

int readline(char *buf, int max, int use_hist, void (*pr)(void)) {
    int n = 0;
    int h = hist_n;
    while(1) {
        int ch = get_char();
        if(ch == '\r' || ch == '\n') {
            printf("\n");
            buf[n] = '\0';
            return n;
        }
        if(ch == 0x7f || ch == '\b') {
            if(n > 0) {n--; printf("\b \b");}
            continue;
        }
        if(ch == 0x1b) {
            if(get_char() != '[') {continue;}
            int k = get_char();
            if(!use_hist) {continue;}
            if(k == 'A' && h > 0) {h--;}
            else if(k == 'B' && h < hist_n) {h++;}
            else {continue;}
            n = 0;
            if(h < hist_n) {n = strlen(hist[h]); memcpy(buf, hist[h], n);}
            buf[n] = '\0';
            printf("\r\033[K");
            pr();
            printf("%s", buf);
            continue;
        }
        if(ch < 32 || ch > 126) {continue;}
        if(n == max - 1) {put_char('\a'); continue;}
        buf[n++] = ch;
        put_char(ch);
    }
}

int num(const char *s) {
    int v = 0;
    while(*s >= '0' && *s <= '9') {v = v * 10 + (*s - '0'); s++;}
    return v;
}

void padr(const char *s, int w) {
    int l = strlen(s);
    printf("%s", s);
    while(l++ < w) {put_char(' ');}
}

void guess(void) {
    uint32_t x = gettime() | 1;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    int secret = x % 100 + 1;
    int tries = 0;
    char b[16];
    printf("i'm thinking of a number from 1 to 100. type q to give up.\n");
    while(1) {
        printf("guess> ");
        readline(b, sizeof(b), 0, NULL);
        if(b[0] == 'q') {printf("it was %d\n", secret); return;}
        if(b[0] < '0' || b[0] > '9') {printf("numbers only (~_~;)\n"); continue;}
        int g = num(b);
        tries++;
        if(g < secret) {printf("too low\n");}
        else if(g > secret) {printf("too high\n");}
        else {printf("got it in %d tries *\\(^o^)/*\n", tries); return;}
    }
}

void help(void) {
    printf("hello                 greet from user land\n");
    printf("echo [-n] text...     print text\n");
    printf("pwd                   print working directory\n");
    printf("ls                    list files on the disk\n");
    printf("cat <file>            print a file\n");
    printf("write <file> <text>   write text to a file (creates it)\n");
    printf("spawn <prog>          run a program in the background (spinner, ticker, crash)\n");
    printf("ps                    list processes\n");
    printf("kill <pid>            stop a process\n");
    printf("sysinfo               uptime, memory, process count\n");
    printf("guess                 number guessing game\n");
    printf("clear                 clear the screen\n");
    printf("shutdown              power off the machine\n");
    printf("exit                  end this shell\n");
}

void ps(void) {
    const char *st[] = {"unused", "ready", "exited", "running"};
    struct procinfo pi;
    printf("PID  STATE    NAME\n");
    for(int i = 0; procinfo(i, &pi) == 0; i++) {
        if(pi.state == PS_UNUSED) {continue;}
        printf("%d%s", pi.pid, pi.pid < 10 ? "    " : "   ");
        padr(st[pi.state], 9);
        printf("%s\n", pi.name);
    }
}

void run(const char *name, int explicit) {
    if(strcmp(name, "shell") == 0) {printf("a second shell would fight for the keyboard (~_~;)\n"); return;}
    int r = spawn(name);
    if(r == -2) {printf("spawn: no free process slots\n");}
    else if(r < 0) {
        if(explicit) {printf("spawn: no such program: %s\n", name);}
        else {printf("unknown command: %s (type help)\n", name);}
    }
}

void main(void) {
    while(1) {
        char cmdline[CMD_MAX];
        prompt();
        readline(cmdline, sizeof(cmdline), 1, prompt);
        if(cmdline[0]) {hist_add(cmdline);}

        char *args[16];
        int argc = 0;
        char *p = cmdline;
        int bad = 0;
        while(*p) {
            while(*p == ' ') {*p++ = '\0';}
            if(*p == '\0') {break;}
            if(argc == 15) {printf("too many arguments (~_~;)\n"); bad = 1; break;}
            args[argc++] = p;
            while(*p && *p != ' ') {p++;}
        }
        args[argc] = NULL;
        if(bad || argc == 0) {continue;}

        if(strcmp(args[0], "hello") == 0) {
            printf("Konnichiwa! Welcome to UserLand. This is your trusty shell (^o^)\n");
        }
        else if(strcmp(args[0], "echo") == 0) {
            bool new_line = true;
            int i = 1;
            if(args[1] != NULL && strcmp(args[1], "-n") == 0) {new_line = false; i++;}
            for(; args[i]; i++) {
                printf("%s", args[i]);
                if(args[i + 1] != NULL) {printf(" ");}
            }
            if(new_line) {printf("\n");}
        }
        else if(strcmp(args[0], "pwd") == 0) {
            char cwd[CWD_MAX];
            if(getcwd(cwd, sizeof(cwd)) < 0) {printf("pwd: getcwd failed\n");}
            else {printf("%s\n", cwd);}
        }
        else if(strcmp(args[0], "ls") == 0) {
            char nm[100];
            for(int i = 0; ; i++) {
                int sz = listfiles(i, nm, sizeof(nm));
                if(sz < 0) {break;}
                padr(nm, 20);
                printf("%d bytes\n", sz);
            }
        }
        else if(strcmp(args[0], "cat") == 0) {
            if(argc < 2) {printf("usage: cat <file>\n");}
            else {
                char b[FILE_DATA_MAX + 1];
                int n = readfile(args[1], b, FILE_DATA_MAX);
                if(n < 0) {printf("cat: %s: no such file\n", args[1]);}
                else {
                    b[n] = '\0';
                    printf("%s", b);
                    if(n > 0 && b[n - 1] != '\n') {printf("\n");}
                }
            }
        }
        else if(strcmp(args[0], "write") == 0) {
            if(argc < 3) {printf("usage: write <file> <text>\n");}
            else {
                char b[CMD_MAX + 2];
                int n = 0;
                for(int i = 2; args[i]; i++) {
                    int l = strlen(args[i]);
                    memcpy(b + n, args[i], l);
                    n += l;
                    if(args[i + 1]) {b[n++] = ' ';}
                }
                b[n++] = '\n';
                if(writefile(args[1], b, n) < 0) {printf("write: could not write %s\n", args[1]);}
            }
        }
        else if(strcmp(args[0], "spawn") == 0) {
            if(argc < 2) {printf("usage: spawn <program>\n");}
            else {run(args[1], 1);}
        }
        else if(strcmp(args[0], "ps") == 0) {ps();}
        else if(strcmp(args[0], "kill") == 0) {
            if(argc < 2) {printf("usage: kill <pid>\n");}
            else {
                int pid = num(args[1]);
                if(kill(pid) < 0) {printf("kill: no such process: %s\n", args[1]);}
                else if(SPIN_COL(pid) >= 1) {printf("\0337\033[1;%dH \0338", SPIN_COL(pid));}
            }
        }
        else if(strcmp(args[0], "sysinfo") == 0 || strcmp(args[0], "mem") == 0) {
            struct sysinfo si;
            sysinfo(&si);
            printf("uptime    : %d s\n", si.uptime_s);
            printf("memory    : %d of %d pages free (%d KB free)\n", si.free_pages, si.total_pages, si.free_pages * 4);
            printf("processes : %d\n", si.procs);
        }
        else if(strcmp(args[0], "guess") == 0) {guess();}
        else if(strcmp(args[0], "help") == 0) {help();}
        else if(strcmp(args[0], "clear") == 0) {printf("\033[2J\033[H");}
        else if(strcmp(args[0], "shutdown") == 0) {shutdown();}
        else if(strcmp(args[0], "exit") == 0) {exit();}
        else {run(args[0], 0);}
    }
}
