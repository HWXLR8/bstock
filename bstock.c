/* Command line stock quote viewer backed by Yahoo Finance. */

#include <curl/curl.h>
#include <ctype.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define UA "Mozilla/5.0"
#define URL_MAX 512
static const char *GREEN = "\033[38;2;0;255;0m", *RED = "\033[38;2;255;0;0m", *RESET = "\033[0m";
static const char *BAND = "\033[48;2;39;39;39m";
static const char *FG = "\033[39m";
static const char *BOLD = "\033[1m";
static const char *DIM = "\033[2m";

static volatile sig_atomic_t stopping;

typedef struct {
    char *buf;
    size_t len, cap;
} Body;

typedef struct {
    char sym[32];
    double shares;
} Hold;

typedef struct {
    Hold *v;
    size_t n, cap;
} Book;

typedef struct {
    double px, diff, ah_px, ah_diff;
    int ah, stale;
    char cur[4];
} Quote;

static size_t collect(void *p, size_t s, size_t n, void *ud) {
    Body *b = ud;
    size_t add = s * n;
    if (b->len + add + 1 > b->cap) {
        size_t cap = (b->cap ? b->cap * 2 : 8192);
        while (cap < b->len + add + 1) cap *= 2;
        if (!(b->buf = realloc(b->buf, cap))) return 0;
        b->cap = cap;
    }
    memcpy(b->buf + b->len, p, add);
    b->len += add;
    b->buf[b->len] = '\0';
    return add;
}

static int fetch(const char *sym, Body *b) {
    char url[URL_MAX];
    long code = 0;
    snprintf(url, sizeof url,
             "https://query1.finance.yahoo.com/v8/finance/chart/%s"
             "?interval=1m&range=1d&includePrePost=true",
             sym);
    CURL *c = curl_easy_init();
    if (!c) return -1;
    struct curl_slist *hdr = curl_slist_append(NULL, "User-Agent: " UA);
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdr);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, collect);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, b);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 15L);
    CURLcode rc = curl_easy_perform(c);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(hdr);
    curl_easy_cleanup(c);
    if (rc == CURLE_OK && code == 200) return 0;
    if (rc != CURLE_OK) fprintf(stderr, "%s: %s\n", sym, curl_easy_strerror(rc));
    else fprintf(stderr, "%s: HTTP %ld\n", sym, code);
    return -1;
}

static const char *json_at(const char *json, const char *key) {
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\":", key);
    const char *p = strstr(json, pat);
    if (!p) return NULL;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

static int get_num(const char *json, const char *key, double *out) {
    const char *p = json_at(json, key);
    if (!p) return -1;
    char *end;
    double v = strtod(p, &end);
    if (end == p || !isfinite(v)) return -1;
    *out = v;
    return 0;
}

static int get_str(const char *json, const char *key, char *out, size_t n) {
    const char *p = json_at(json, key);
    if (!p || *p != '"') return -1;
    p++;
    const char *e = strchr(p, '"');
    if (!e || (size_t)(e - p) >= n) return -1;
    memcpy(out, p, (size_t)(e - p));
    out[e - p] = '\0';
    return 0;
}

static void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    exit(1);
}

static void push(Book *bk, const char *sym, double shares) {
    if (bk->n == bk->cap) {
        size_t cap = bk->cap ? bk->cap * 2 : 16;
        Hold *v = realloc(bk->v, cap * sizeof *v);
        if (!v) die("out of memory\n");
        bk->v = v;
        bk->cap = cap;
    }
    Hold *h = &bk->v[bk->n++];
    size_t i = 0;
    for (; i + 1 < sizeof h->sym && sym[i]; i++) h->sym[i] = (char)toupper((unsigned char)sym[i]);
    h->sym[i] = '\0';
    h->shares = shares;
}

static void load_config(const char *path, Book *bk) {
    FILE *f = fopen(path, "r");
    if (!f) die("cannot open %s\n", path);
    if (fseek(f, 0, SEEK_END) != 0) die("cannot read %s\n", path);
    long sz = ftell(f);
    if (sz < 0) die("cannot read %s\n", path);
    rewind(f);
    char *js = malloc((size_t)sz + 1);
    if (!js) die("out of memory\n");
    size_t got = fread(js, 1, (size_t)sz, f);
    fclose(f);
    js[got] = '\0';
    if (got != (size_t)sz) die("cannot read %s\n", path);

    const char *p = strstr(js, "\"tickers\"");
    if (!p || !(p = strchr(p, '['))) die("%s: no \"tickers\" array\n", path);
    while ((p = strchr(p + 1, '{'))) {
        const char *end = strchr(p, '}');
        if (!end) break;
        size_t len = (size_t)(end - p);
        char obj[128];
        if (len >= sizeof obj) { p = end; continue; }
        memcpy(obj, p, len);
        obj[len] = '\0';

        char sym[32];
        double shares = 0;
        if (get_str(obj, "symbol", sym, sizeof sym) == 0) {
            get_num(obj, "shares", &shares);
            push(bk, sym, shares);
        }
        p = end;
    }
    free(js);
}

static const char *money(double v, char *buf, size_t n, int sign) {
    char raw[512];
    snprintf(raw, sizeof raw, "%.2f", v < 0 ? -v : v);
    char *dot = strchr(raw, '.');
    int ip = (int)(dot - raw);
    size_t need = strlen(dot) + (size_t)ip + (size_t)((ip - 1) / 3) + (v < 0 || sign);
    if (need >= n) return strcpy(buf, "");
    char *o = buf;
    if (v < 0) *o++ = '-';
    else if (sign) *o++ = '+';
    for (int i = 0; i < ip; i++) {
        if (i && (ip - i) % 3 == 0) *o++ = ',';
        *o++ = raw[i];
    }
    strcpy(o, dot);
    return buf;
}

static int fetch_price(const char *sym, double *px) {
    Body b = {0};
    int rc = fetch(sym, &b) != 0 ? -1 : get_num(b.buf, "regularMarketPrice", px);
    free(b.buf);
    return rc;
}

static int fetch_quote(const char *sym, Quote *q) {
    Body b = {0};
    int rc = -1;
    double prev, last;
    *q = (Quote){0};
    if (fetch(sym, &b) != 0) goto done;
    if (get_num(b.buf, "regularMarketPrice", &q->px) ||
        get_num(b.buf, "chartPreviousClose", &prev)) {
        fprintf(stderr, "%s: no price data\n", sym);
        goto done;
    }
    q->diff = q->px - prev;
    if (get_num(b.buf, "fulldayPrice", &last) == 0 && last > 0 && last != q->px) {
        q->ah_px = last;
        q->ah_diff = last - q->px;
        q->ah = 1;
    }
    if (get_str(b.buf, "currency", q->cur, sizeof q->cur) != 0) strcpy(q->cur, "USD");
    rc = 0;
done:
    free(b.buf);
    return rc;
}

static void row(const Hold *h, const Quote *q, int ah) {
    int live = !ah || q->ah;
    double px = ah ? q->ah_px : q->px;
    double diff = ah ? q->ah_diff : q->diff;
    double base = px - diff;
    double pct = base ? diff / base * 100 : 0;
    const char *fg = live ? (diff >= 0 ? GREEN : RED) : "";
    const char *clr = live ? FG : "";
    const char *band = live && fabs(pct) >= 2 ? BAND : "";
    const char *dim = q->stale ? DIM : "", *off = q->stale ? RESET : "";
    char pc[32] = "", pl[32] = "", vl[32];
    money(px * h->shares, vl, sizeof vl, 0);
    if (live) {
        char t[32];
        snprintf(pc, sizeof pc, "%s%%", money(pct, t, sizeof t, 1));
        money(diff * h->shares, pl, sizeof pl, 1);
    }
    printf("%s%s%-10s %9.2f  %s%9s%s  %s%12s%s  %13s %s%s%s\n",
           dim, band, h->sym, px, fg, pc, clr, fg, pl, clr, vl, q->cur,
           *band ? RESET : "", off);
}

static void total(double day, double value) {
    double base = value - day;
    char d[32], v[32];
    printf("%s%-10s %9s  %s%+8.2f%%%s  %s%12s%s  %13s CAD%s\n", BOLD, "TOTAL", "",
           day >= 0 ? GREEN : RED, base ? day / base * 100 : 0.0, FG,
           day >= 0 ? GREEN : RED, money(day, d, sizeof d, 1), FG,
           money(value, v, sizeof v, 0), RESET);
}

static int render(const Book *bk, Quote *q, double fx_day, double fx_ah) {
    double value = 0, day = 0, ah_day = 0, ah_value = 0;
    int owned = 0, ah_any = 0;
    for (size_t i = 0; i < bk->n; i++) {
        double sh = bk->v[i].shares;
        if (sh <= 0 || q[i].px <= 0) continue;
        double r = !strcmp(q[i].cur, "CAD") ? 1 : fx_day;
        double ra = !strcmp(q[i].cur, "CAD") ? 1 : fx_ah;
        value += q[i].px * sh * r;
        day += q[i].diff * sh * r;
        if (q[i].ah) ah_any = 1;
        else q[i].ah_px = q[i].px;
        ah_value += q[i].ah_px * sh * ra;
        owned = 1;
    }
    ah_day = ah_value - value;
    if (!owned) return 0;
    for (size_t i = 0; i < bk->n; i++)
        if (bk->v[i].shares > 0 && q[i].px > 0) row(&bk->v[i], &q[i], 0);
    total(day, value);
    if (ah_any) {
        printf("\n%sAFTER HOURS%s\n", BOLD, RESET);
        for (size_t i = 0; i < bk->n; i++)
            if (bk->v[i].shares > 0 && q[i].px > 0) row(&bk->v[i], &q[i], 1);
        total(ah_day, ah_value);
    }
    return 1;
}

static void on_stop(int sig) {
    (void)sig;
    stopping = 1;
}

static void leave(void) {
    fputs("\033[?25h\033[?1049l", stdout);
    fflush(stdout);
}

static void nap(double sec) {
    for (double t = 0; t < sec && !stopping; t += 0.1) usleep(100000);
}

static void refresh(const Book *bk, Quote *q, size_t i) {
    Quote fresh;
    if (fetch_quote(bk->v[i].sym, &fresh) != 0 || fresh.px <= 0) {
        fresh = q[i];
        fresh.stale = 1;
    }
    q[i] = fresh;
}

static void live(Book *bk, Quote *q, double fx_day, double fx_ah, double delay) {
    signal(SIGINT, on_stop);
    signal(SIGTERM, on_stop);
    atexit(leave);
    fputs("\033[?1049h\033[?25l", stdout);
    while (!stopping) {
        double r;
        if (fetch_price("CAD=X", &r) == 0 && r > 0) fx_ah = r;
        for (size_t i = 0; i < bk->n && !stopping; i++) {
            if (bk->v[i].shares <= 0) continue;
            refresh(bk, q, i);
            int open = 1;
            for (size_t j = 0; j < bk->n; j++)
                if (bk->v[j].shares > 0 && q[j].px > 0 && q[j].ah) open = 0;
            if (open) fx_day = fx_ah;
            fputs("\033[H\033[J", stdout);
            if (!render(bk, q, fx_day, fx_ah)) fputs("no price data\n", stdout);
            fflush(stdout);
            nap(delay);
        }
    }
}

int main(int argc, char **argv) {
    int loop = isatty(STDOUT_FILENO);
    double delay = 2;
    int c;
    while ((c = getopt(argc, argv, "i:")) != -1) {
        if (c == 'i') delay = atof(optarg);
        else die("usage: bstock [-i seconds]\n");
    }
    if (optind < argc || delay <= 0) die("usage: bstock [-i seconds]\n");
    if (!loop) GREEN = RED = RESET = BAND = FG = BOLD = DIM = "";
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0) die("curl init failed\n");

    Book bk = {0};
    const char *home = getenv("BSTOCK_HOME");
    char cfg[512];
    snprintf(cfg, sizeof cfg, "%s/bstock.json", home ? home : ".");
    load_config(cfg, &bk);

    Quote *q = calloc(bk.n ? bk.n : 1, sizeof *q);
    if (!q) die("out of memory\n");

    int owned = 0, need_fx = 0;
    for (size_t i = 0; i < bk.n; i++) {
        if (bk.v[i].shares <= 0 || fetch_quote(bk.v[i].sym, &q[i]) != 0 || q[i].px <= 0) continue;
        owned = 1;
        if (!strcmp(q[i].cur, "CAD")) continue;
        if (strcmp(q[i].cur, "USD")) die("%s: unsupported currency %s\n", bk.v[i].sym, q[i].cur);
        need_fx = 1;
    }

    double fx = 0, fx_ah = 0;
    if (owned && need_fx && (fetch_price("CAD=X", &fx) != 0 || fx <= 0))
        die("no CAD rate\n");
    fx_ah = fx;

    if (loop) live(&bk, q, fx, fx_ah, delay);
    else render(&bk, q, fx, fx);

    free(q);
    free(bk.v);
    curl_global_cleanup();
    return bk.n ? 0 : 1;
}
