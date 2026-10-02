#include "user.h"

#define CMD_MAX 128
#define HIST_N 8
#define UNUSED __attribute__((unused))

char hist[HIST_N][CMD_MAX];
int hist_n;

struct cmd {
    const char *name;
    void (*fn)(int argc, char **a);
    const char *usage;
    const char *desc;
};

extern const struct cmd cmds[];
const char *progs[] = {"spinner", "ticker", "crash", "producer", "consumer", "evil", "recurse", NULL};

int complete(char *b, int n);

void prompt(void) {
    char cwd[CWD_MAX];
    if(getcwd(cwd, sizeof(cwd)) < 0) {cwd[0] = '?'; cwd[1] = '\0';}
    printf("\033[1;36m*\\(^o^)/*\033[0m \033[33m%s\033[0m \033[1;36m>>\033[0m ", cwd);
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
        if(ch == '\t') {
            int sp = 0;
            for(int i = 0; i < n; i++) {if(buf[i] == ' ') {sp = 1;}}
            if(use_hist && n > 0 && !sp) {
                buf[n] = '\0';
                n = complete(buf, n);
                printf("\r\033[K");
                pr();
                printf("%s", buf);
            }
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

int starts(const char *s, const char *p, int n) {
    for(int i = 0; i < n; i++) {if(s[i] != p[i]) {return 0;}}
    return 1;
}

int complete(char *b, int n) {
    const char *m[48];
    int c = 0;
    for(int i = 0; cmds[i].name; i++) {if(starts(cmds[i].name, b, n) && c < 48) {m[c++] = cmds[i].name;}}
    for(int i = 0; progs[i]; i++) {if(starts(progs[i], b, n) && c < 48) {m[c++] = progs[i];}}
    if(c == 0) {return n;}
    if(c == 1) {
        n = strlen(m[0]);
        memcpy(b, m[0], n);
        b[n++] = ' ';
        b[n] = '\0';
        return n;
    }
    int common = strlen(m[0]);
    for(int i = 1; i < c; i++) {
        int k = 0;
        while(k < common && m[i][k] == m[0][k]) {k++;}
        common = k;
    }
    if(common > n) {
        memcpy(b + n, m[0] + n, common - n);
        n = common;
        b[n] = '\0';
        return n;
    }
    printf("\n");
    for(int i = 0; i < c; i++) {printf("%s  ", m[i]);}
    printf("\n");
    return n;
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

void padn(int v, int w) {
    int l = 1;
    for(int t = v; t > 9; t /= 10) {l++;}
    printf("%d", v);
    while(l++ < w) {put_char(' ');}
}

void p2(int v) {
    printf("%d%d", v / 10, v % 10);
}

int joined(char **a, char *b) {
    int n = 0;
    for(int i = 0; a[i]; i++) {
        int l = strlen(a[i]);
        memcpy(b + n, a[i], l);
        n += l;
        if(a[i + 1]) {b[n++] = ' ';}
    }
    b[n++] = '\n';
    return n;
}

void c_hello(int argc UNUSED, char **a UNUSED) {
    printf("Konnichiwa! Welcome to UserLand. This is your trusty shell (^o^)\n");
}

void c_echo(int argc UNUSED, char **a) {
    bool new_line = true;
    int i = 1;
    if(a[1] != NULL && strcmp(a[1], "-n") == 0) {new_line = false; i++;}
    for(; a[i]; i++) {
        printf("%s", a[i]);
        if(a[i + 1] != NULL) {printf(" ");}
    }
    if(new_line) {printf("\n");}
}

void c_pwd(int argc UNUSED, char **a UNUSED) {
    char cwd[CWD_MAX];
    if(getcwd(cwd, sizeof(cwd)) < 0) {printf("pwd: getcwd failed\n");}
    else {printf("%s\n", cwd);}
}

void c_cd(int argc, char **a) {
    const char *p = argc > 1 ? a[1] : "/";
    if(chdir(p) < 0) {printf("cd: %s: no such directory\n", p);}
}

void c_ls(int argc, char **a) {
    const char *p = argc > 1 ? a[1] : ".";
    char nm[100];
    for(int i = 0; ; i++) {
        int r = readdir(p, i, nm);
        if(r == -2) {printf("ls: %s: no such directory\n", p); return;}
        if(r < 0) {break;}
        padr(nm, 20);
        if(nm[strlen(nm) - 1] == '/') {printf("<dir>\n");}
        else {printf("%d bytes\n", r);}
    }
}

void c_cat(int argc, char **a) {
    if(argc < 2) {printf("usage: cat <file>\n"); return;}
    char b[FILE_DATA_MAX + 1];
    int n = readfile(a[1], b, FILE_DATA_MAX);
    if(n < 0) {printf("cat: %s: no such file\n", a[1]); return;}
    b[n] = '\0';
    printf("%s", b);
    if(n > 0 && b[n - 1] != '\n') {printf("\n");}
}

void put(int argc, char **a, int append) {
    if(argc < 3) {printf("usage: %s <file> <text>\n", append ? "append" : "write"); return;}
    char b[CMD_MAX + 2];
    int n = joined(a + 2, b);
    int r = append ? appendfile(a[1], b, n) : writefile(a[1], b, n);
    if(r < 0) {printf("%s: could not write %s\n", append ? "append" : "write", a[1]);}
}

void c_write(int argc, char **a) {put(argc, a, 0);}
void c_append(int argc, char **a) {put(argc, a, 1);}

void c_rm(int argc, char **a) {
    if(argc < 2) {printf("usage: rm <file or empty directory>\n"); return;}
    int r = unlink(a[1]);
    if(r == -1) {printf("rm: %s: no such file or directory\n", a[1]);}
    else if(r == -2) {printf("rm: %s: directory not empty\n", a[1]);}
}

void c_mkdir(int argc, char **a) {
    if(argc < 2) {printf("usage: mkdir <directory>\n"); return;}
    int r = mkdir(a[1]);
    if(r == -2) {printf("mkdir: %s: already exists\n", a[1]);}
    else if(r < 0) {printf("mkdir: could not create %s\n", a[1]);}
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

void c_spawn(int argc, char **a) {
    if(argc < 2) {printf("usage: spawn <program>\n"); return;}
    run(a[1], 1);
}

void c_ps(int argc UNUSED, char **a UNUSED) {
    const char *st[] = {"unused", "ready", "exited", "running"};
    struct procinfo pi;
    printf("PID  STATE    PAGES  NAME\n");
    for(int i = 0; procinfo(i, &pi) == 0; i++) {
        if(pi.state == PS_UNUSED) {continue;}
        padn(pi.pid, 5);
        padr(st[pi.state], 9);
        padn(pi.pages, 7);
        printf("%s\n", pi.name);
    }
}

void c_top(int argc UNUSED, char **a UNUSED) {
    uint32_t t = gettime() - TICKS_PER_SEC;
    printf("\033[2J");
    while(1) {
        int ch = poll_char();
        if(ch == 'q' || ch == 'Q') {break;}
        if(gettime() - t >= TICKS_PER_SEC) {
            t = gettime();
            struct sysinfo si;
            sysinfo(&si);
            printf("\033[Htop - up %d s, %d process(es), %d of %d pages free   (q to quit)\033[K\n\033[K\n", si.uptime_s, si.procs, si.free_pages, si.total_pages);
            printf("PID  STATE    PAGES  NAME\033[K\n");
            struct procinfo pi;
            for(int i = 0; procinfo(i, &pi) == 0; i++) {
                if(pi.state == PS_UNUSED || pi.state == PS_EXITED) {continue;}
                padn(pi.pid, 5);
                padr(pi.state == PS_RUNNING ? "running" : "ready", 9);
                padn(pi.pages, 7);
                printf("%s\033[K\n", pi.name);
            }
            printf("\033[J");
        }
        yield();
    }
    printf("\033[2J\033[H");
}

void c_kill(int argc, char **a) {
    if(argc < 2) {printf("usage: kill <pid>\n"); return;}
    int pid = num(a[1]);
    if(kill(pid) < 0) {printf("kill: no such process: %s\n", a[1]);}
    else if(SPIN_COL(pid) >= 1) {printf("\0337\033[1;%dH \0338", SPIN_COL(pid));}
}

void c_sysinfo(int argc UNUSED, char **a UNUSED) {
    struct sysinfo si;
    sysinfo(&si);
    printf("uptime    : %d s\n", si.uptime_s);
    printf("memory    : %d of %d pages free (%d KB free)\n", si.free_pages, si.total_pages, si.free_pages * 4);
    printf("processes : %d\n", si.procs);
}

void c_uptime(int argc UNUSED, char **a UNUSED) {
    struct sysinfo si;
    sysinfo(&si);
    int s = si.uptime_s;
    printf("up ");
    if(s >= 3600) {printf("%d h ", s / 3600);}
    if(s >= 60) {printf("%d min ", (s % 3600) / 60);}
    printf("%d s\n", s % 60);
}

void c_date(int argc UNUSED, char **a UNUSED) {
    uint32_t t = getdate();
    uint32_t days = t / 86400;
    uint32_t rem = t % 86400;
    int z = days + 719468;
    int era = z / 146097;
    int doe = z - era * 146097;
    int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int y = yoe + era * 400;
    int doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int mp = (5 * doy + 2) / 153;
    int d = doy - (153 * mp + 2) / 5 + 1;
    int m = mp < 10 ? mp + 3 : mp - 9;
    if(m <= 2) {y++;}
    const char *wd[] = {"Thu", "Fri", "Sat", "Sun", "Mon", "Tue", "Wed"};
    printf("%s %d-", wd[days % 7], y);
    p2(m); printf("-"); p2(d); printf(" ");
    p2(rem / 3600); printf(":"); p2((rem % 3600) / 60); printf(":"); p2(rem % 60);
    printf(" UTC\n");
}

void c_reboot(int argc UNUSED, char **a UNUSED) {
    if(reboot() < 0) {printf("reboot: not supported\n");}
}

void c_shutdown(int argc UNUSED, char **a UNUSED) {shutdown();}
void c_exit(int argc UNUSED, char **a UNUSED) {exit();}
void c_clear(int argc UNUSED, char **a UNUSED) {printf("\033[2J\033[H");}

void c_guess(int argc UNUSED, char **a UNUSED) {
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

#define SW 30
#define SH 14

void cell(int x, int y, char c) {
    printf("\033[%d;%dH%c", y + 3, x + 2, c);
}

void place_food(uint32_t *seed, int *x, int *y, int n, int *fx, int *fy) {
    while(1) {
        *seed ^= *seed << 13; *seed ^= *seed >> 17; *seed ^= *seed << 5;
        *fx = *seed % SW;
        *seed ^= *seed << 13; *seed ^= *seed >> 17; *seed ^= *seed << 5;
        *fy = *seed % SH;
        int hit = 0;
        for(int i = 0; i < n; i++) {if(x[i] == *fx && y[i] == *fy) {hit = 1;}}
        if(!hit) {return;}
    }
}

void c_snake(int argc UNUSED, char **a UNUSED) {
    int x[SW * SH], y[SW * SH];
    int n = 3, dx = 1, dy = 0, score = 0, fx, fy, over = 0;
    uint32_t seed = gettime() | 1;
    for(int i = 0; i < n; i++) {x[i] = SW / 2 - i; y[i] = SH / 2;}
    place_food(&seed, x, y, n, &fx, &fy);

    printf("\033[2J\033[2;1H+");
    for(int i = 0; i < SW; i++) {put_char('-');}
    printf("+");
    for(int j = 0; j < SH; j++) {printf("\033[%d;1H|\033[%d;%dH|", j + 3, j + 3, SW + 2);}
    printf("\033[%d;1H+", SH + 3);
    for(int i = 0; i < SW; i++) {put_char('-');}
    printf("+\033[%d;1HScore: 0\033[%d;1Hwasd or the arrow keys steer, q quits", SH + 4, SH + 5);
    cell(fx, fy, '*');
    for(int i = 0; i < n; i++) {cell(x[i], y[i], i ? 'o' : '@');}

    uint32_t t = gettime();
    while(!over) {
        int ch;
        while((ch = poll_char()) >= 0) {
            if(ch == 0x1b) {
                int c2 = poll_char();
                int c3 = poll_char();
                ch = c2 != '[' ? 0 : c3 == 'A' ? 'w' : c3 == 'B' ? 's' : c3 == 'C' ? 'd' : c3 == 'D' ? 'a' : 0;
            }
            int ndx = dx, ndy = dy;
            if(ch == 'q') {over = 2;}
            else if(ch == 'w') {ndx = 0; ndy = -1;}
            else if(ch == 's') {ndx = 0; ndy = 1;}
            else if(ch == 'a') {ndx = -1; ndy = 0;}
            else if(ch == 'd') {ndx = 1; ndy = 0;}
            if(!(ndx == -dx && ndy == -dy)) {dx = ndx; dy = ndy;}
        }
        if(over) {break;}
        if(gettime() - t < TICKS_PER_SEC / 8) {yield(); continue;}
        t = gettime();

        int nx = x[0] + dx, ny = y[0] + dy;
        int eat = nx == fx && ny == fy;
        int lim = eat ? n : n - 1;
        if(nx < 0 || nx >= SW || ny < 0 || ny >= SH) {over = 1; break;}
        for(int i = 0; i < lim; i++) {if(x[i] == nx && y[i] == ny) {over = 1;}}
        if(over) {break;}

        int tx = x[n - 1], ty = y[n - 1];
        if(eat) {n++;}
        cell(x[0], y[0], 'o');
        for(int i = n - 1; i > 0; i--) {x[i] = x[i - 1]; y[i] = y[i - 1];}
        x[0] = nx;
        y[0] = ny;
        cell(nx, ny, '@');
        if(eat) {
            score++;
            place_food(&seed, x, y, n, &fx, &fy);
            cell(fx, fy, '*');
            printf("\033[%d;1HScore: %d", SH + 4, score);
        }
        else {cell(tx, ty, ' ');}
    }
    printf("\033[%d;1H\033[K%s score %d\n", SH + 6, over == 1 ? "game over!" : "bye!", score);
}

void c_help(int argc UNUSED, char **a UNUSED);

const struct cmd cmds[] = {
    {"hello", c_hello, "", "greet from user land"},
    {"echo", c_echo, "[-n] text...", "print text"},
    {"pwd", c_pwd, "", "print the working directory"},
    {"cd", c_cd, "[dir]", "change directory (no argument goes to /)"},
    {"ls", c_ls, "[dir]", "list a directory"},
    {"cat", c_cat, "<file>", "print a file"},
    {"write", c_write, "<file> <text>", "replace a file's contents (creates it)"},
    {"append", c_append, "<file> <text>", "add text to the end of a file"},
    {"rm", c_rm, "<path>", "remove a file or an empty directory"},
    {"mkdir", c_mkdir, "<dir>", "create a directory"},
    {"spawn", c_spawn, "<prog>", "run a program in the background"},
    {"ps", c_ps, "", "list processes"},
    {"top", c_top, "", "live process table (q to quit)"},
    {"kill", c_kill, "<pid>", "stop a process"},
    {"sysinfo", c_sysinfo, "", "uptime, free memory, process count"},
    {"mem", c_sysinfo, "", NULL},
    {"uptime", c_uptime, "", "how long the machine has been up"},
    {"date", c_date, "", "date and time from the hardware clock"},
    {"guess", c_guess, "", "number guessing game"},
    {"snake", c_snake, "", "snake (wasd or arrows)"},
    {"clear", c_clear, "", "clear the screen"},
    {"help", c_help, "", "this list"},
    {"reboot", c_reboot, "", "restart the machine"},
    {"shutdown", c_shutdown, "", "power off the machine"},
    {"exit", c_exit, "", "end this shell"},
    {NULL, NULL, NULL, NULL}
};

void c_help(int argc UNUSED, char **a UNUSED) {
    for(int i = 0; cmds[i].name; i++) {
        if(!cmds[i].desc) {continue;}
        int l = strlen(cmds[i].name) + 1 + strlen(cmds[i].usage);
        printf("%s %s", cmds[i].name, cmds[i].usage);
        while(l++ < 24) {put_char(' ');}
        printf("%s\n", cmds[i].desc);
    }
    printf("programs (spawn <name>, or just type the name): ");
    for(int i = 0; progs[i]; i++) {printf("%s ", progs[i]);}
    printf("\ntab completes command names, up and down walk the history\n");
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

        int found = 0;
        for(int i = 0; cmds[i].name; i++) {
            if(strcmp(args[0], cmds[i].name) == 0) {cmds[i].fn(argc, args); found = 1; break;}
        }
        if(!found) {run(args[0], 0);}
    }
}
