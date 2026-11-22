// Tests for the terminal key decoder (src/ui/keys.c): the bytes terminals send for keys, and
// what the prompt must get from them. Each case feeds bytes and compares the keys that come out,
// written as a short text: "a" a character, "<LEFT>" an editor key, "<ENTER>" and so on.
#include "check.h"
#include "../src/ui/keys.h"
#include "../src/ui/edit.h"

static const char* ed_name(int ed) {
    switch (ed) {
    case EK_BACK: return "BACK"; case EK_DEL: return "DEL";
    case EK_LEFT: return "LEFT"; case EK_RIGHT: return "RIGHT"; case EK_HOME: return "HOME"; case EK_END: return "END";
    case EK_WORD_LEFT: return "WLEFT"; case EK_WORD_RIGHT: return "WRIGHT";
    case EK_UP: return "UP"; case EK_DOWN: return "DOWN";
    case EK_KILL_START: return "KSTART"; case EK_KILL_END: return "KEND"; case EK_KILL_WORD: return "KWORD";
    case EK_CLEAR: return "ESC";
    }
    return "?";
}

static char got[4096];
static int gotn;

static void add(const char* s) {
    size_t n = strlen(s);
    if (gotn + n < sizeof got) { memcpy(got + gotn, s, n); gotn += (int)n; got[gotn] = 0; }
}

static void name(const kd_key_t* k) {
    char b[32];
    switch (k->kind) {
    case KD_EDIT:
        if (k->ed != EK_CHAR) { snprintf(b, sizeof b, "<%s>", ed_name(k->ed)); break; }
        if (k->ch >= 0x20 && k->ch < 0x7F) { b[0] = (char)k->ch; b[1] = 0; }
        else snprintf(b, sizeof b, "<U+%04X>", (unsigned)k->ch);
        break;
    case KD_ENTER: snprintf(b, sizeof b, "<ENTER>"); break;
    case KD_EOF: snprintf(b, sizeof b, "<EOF>"); break;
    case KD_INTR: snprintf(b, sizeof b, "<INTR>"); break;
    case KD_PASTE_BEGIN: snprintf(b, sizeof b, "<PASTE>"); break;
    case KD_PASTE_END: snprintf(b, sizeof b, "</PASTE>"); break;
    default: snprintf(b, sizeof b, "<kind %d>", k->kind);
    }
    add(b);
}

// A check on what came out, with both shown when they differ.
#define SAME(got_, want_) same((got_), (want_), __LINE__)
static void same(const char* g, const char* w, int line) {
    checks_run++;
    if (strcmp(g, w)) { checks_failed++; printf("FAIL %s:%d: got \"%s\", want \"%s\"\n", __FILE__, line, g, w); }
}

// Feed `bytes` (n of them) and say what came out. "|" in the result marks where kd_timeout was
// called because bytes stopped with a key part-way, which is what a caller does.
static const char* feed(kd_t* d, const char* bytes, int n) {
    gotn = 0;
    got[0] = 0;
    kd_key_t k;
    for (int i = 0; i < n; i++) if (kd_feed(d, (unsigned char)bytes[i], &k)) name(&k);
    if (kd_pending(d)) { add("|"); if (kd_timeout(d, &k)) name(&k); }
    return got;
}

#define KEYS(bytes, want) do { kd_t d_; kd_init(&d_); SAME(feed(&d_, bytes, (int)(sizeof bytes - 1)), want); } while (0)

static void test_text(void) {
    KEYS("ab c", "ab c");
    KEYS("\t", "<U+0009>");                               // the editor makes it a space
    KEYS("\xC3\xA9", "<U+00E9>");                         // e acute
    KEYS("\xE2\x82\xAC\xF0\x9F\x98\x80", "<U+20AC><U+1F600>");
    KEYS("\xC3" "a", "a");                                // a broken character is dropped, not what follows
    KEYS("\x80\xBF" "z", "z");                            // continuation bytes on their own
    KEYS("\xE2\x82", "|");                                // cut off: given up at the timeout
    KEYS("\xFF" "q", "q");
    // What is not a real character is dropped: overlong forms (a way to sneak in a control byte),
    // surrogates, past U+10FFFF, and the C1 controls.
    KEYS("\xC0\x8A" "a\xE0\x80\x8D" "b\xF0\x80\x80\x9B" "c", "abc");
    KEYS("\xED\xA0\x80" "d\xF4\x90\x80\x80" "e\xC2\x9B" "f", "def");
    KEYS("\xE0\x82\xA9" "g\xF0\x80\x82\xA9" "h", "gh");   // U+00A9, a printable one, the long way round
    KEYS("\xC2\xA0\xF4\x8F\xBF\xBF\xE0\xA0\x80", "<U+00A0><U+10FFFF><U+0800>");   // the edges that are fine
}

static void test_controls(void) {
    KEYS("\x01\x02\x05\x06", "<HOME><LEFT><END><RIGHT>");
    KEYS("\x08\x7F", "<BACK><BACK>");                     // Backspace is one or the other
    KEYS("\x0B\x15\x17", "<KEND><KSTART><KWORD>");
    KEYS("\x10\x0E", "<UP><DOWN>");
    KEYS("\x03\x04", "<INTR><EOF>");
    KEYS("\x07\x0C\x1A\x00", "");                         // the rest mean nothing here
}

static void test_enter(void) {
    KEYS("\r", "<ENTER>");
    KEYS("\n", "<ENTER>");
    KEYS("\r\n", "<ENTER>");                              // one Enter, as Windows text has it
    KEYS("\r\r", "<ENTER><ENTER>");
    KEYS("\n\n", "<ENTER><ENTER>");
    KEYS("\n\r", "<ENTER><ENTER>");
    KEYS("\r\n\r\n", "<ENTER><ENTER>");
    KEYS("a\r\nb", "a<ENTER>b");
    KEYS("\rx\n", "<ENTER>x<ENTER>");                     // only an LF right after the CR goes with it
    kd_t d;                                               // CR at the end of one read, LF at the start of the next
    kd_init(&d);
    SAME(feed(&d, "a\r", 2), "a<ENTER>");
    SAME(feed(&d, "\nb", 2), "b");
}

static void test_sequences(void) {
    KEYS("\033[A\033[B\033[C\033[D", "<UP><DOWN><RIGHT><LEFT>");
    KEYS("\033OA\033OB\033OC\033OD", "<UP><DOWN><RIGHT><LEFT>");   // the application keypad's
    KEYS("\033[H\033[F\033OH\033OF", "<HOME><END><HOME><END>");
    KEYS("\033[1~\033[7~\033[4~\033[8~", "<HOME><HOME><END><END>");
    KEYS("\033[3~", "<DEL>");
    KEYS("\033[1;5C\033[1;5D", "<WRIGHT><WLEFT>");        // Ctrl
    KEYS("\033[1;3C\033[1;3D", "<WRIGHT><WLEFT>");        // Alt
    KEYS("\033[1;2C\033[1;2D", "<RIGHT><LEFT>");          // Shift is no different
    KEYS("\033[1;7D", "<WLEFT>");                         // Ctrl and Alt
    KEYS("\033[2~\033[5~\033[6~\033[15~\033[Z", "");      // Insert, page keys, F5, Shift-Tab: nothing
    KEYS("\033b\033f\033\x7F\033\x08", "<WLEFT><WRIGHT><KWORD><KWORD>");   // Alt-b, Alt-f, Alt-Backspace
    KEYS("\033x" "y", "y");                               // an Alt key with no meaning here
    KEYS("x\033[Dy", "x<LEFT>y");
}

static void test_escape(void) {
    KEYS("\033", "|<ESC>");                               // a lone Escape, known only when nothing follows
    KEYS("\033\033", "<ESC>|<ESC>");
    KEYS("\033\033[D", "<ESC><LEFT>");
    KEYS("\033[", "|");                                   // a sequence cut off is dropped, not typed
    KEYS("\033[1;", "|");
    KEYS("\033\r", "<ENTER>");                            // a control byte breaks a sequence off, and counts
    KEYS("\033[1\ra", "<ENTER>a");
    KEYS("\033[1\033[C", "<RIGHT>");
    KEYS("\033[1\x01" "b\033\x15", "<HOME>b<KSTART>");
    KEYS("\033\xC3\xA9", "<U+00E9>");
    // A sequence too long to be a real one is read to its end and dropped, not typed.
    KEYS("\033[11111111111111111111111111111111111111111111111Ax", "x");
    KEYS("\033[9999999999;99999D", "<WLEFT>");           // big numbers do not overflow
    KEYS("\033[1;4294967297D", "<WLEFT>");               // (2^32 + 1: wrapped, it would be 1, no modifier)
}

static void test_paste(void) {
    KEYS("\033[200~hi\033[201~", "<PASTE>hi</PASTE>");
    KEYS("\033[200~one\rtwo\r\n\nthree\033[201~", "<PASTE>one<ENTER>two<ENTER><ENTER>three</PASTE>");
    // Inside a paste, control characters and keys' sequences are not commands.
    KEYS("\033[200~a\x01\x03\x04\x15\x7F" "b\033[D\033[3~\033b" "c\033[201~", "<PASTE>abc</PASTE>");
    KEYS("\033[200~a\tb\033[201~", "<PASTE>a<U+0009>b</PASTE>");
    KEYS("\033[200~\xC3\xA9\033[201~", "<PASTE><U+00E9></PASTE>");
    KEYS("\033[200~\033", "<PASTE>|");                    // no Escape key from inside a paste
    KEYS("\033[200~\033\033[201~x", "<PASTE></PASTE>x");
    KEYS("\033[200~x\033[201~\x01", "<PASTE>x</PASTE><HOME>");   // after it, keys are keys again
    KEYS("\033[201~a", "a");                              // an end with no start is nothing
    kd_t d;                                               // a paste split across reads, even its end marker
    kd_init(&d);
    SAME(feed(&d, "\033[200~ab\033[20", 10), "<PASTE>ab|");
    kd_init(&d);
    const char* p = "\033[200~ab\033[201~c";
    gotn = 0;
    got[0] = 0;
    kd_key_t k;
    for (int i = 0; p[i]; i++) if (kd_feed(&d, (unsigned char)p[i], &k)) name(&k);   // byte by byte, no timeouts
    SAME(got, "<PASTE>ab</PASTE>c");
}

// Random bytes: every key that comes out is one the prompt knows, with a real character.
static void test_random(void) {
    uint32_t seed = 12345;
    kd_t d;
    kd_init(&d);
    int keys = 0, bad = 0;
    for (int i = 0; i < 400000; i++) {
        seed = seed * 1103515245u + 12345u;
        unsigned r = (seed >> 16) & 0xFFFF;
        unsigned char b;
        if (r % 7 == 0) b = 0x1B;
        else if (r % 11 == 0) b = (unsigned char)"[O~;0123456789ABCDHF"[(r >> 4) % 20];
        else b = (unsigned char)(r >> 3);
        kd_key_t k;
        if (kd_feed(&d, b, &k)) {
            keys++;
            if (k.kind < KD_EDIT || k.kind > KD_PASTE_END) bad++;
            if (k.kind == KD_EDIT && (k.ed < EK_CHAR || k.ed > EK_CLEAR)) bad++;
            if (k.kind == KD_EDIT && k.ed == EK_CHAR && (k.ch > 0x10FFFF || (k.ch < 0x20 && k.ch != '\t'))) bad++;
            if (k.kind != KD_EDIT && k.ed) bad++;
        }
        if (r % 97 == 0 && kd_pending(&d) && kd_timeout(&d, &k)) keys++;
        if (d.seqn < 0 || d.seqn > (int)sizeof d.seq || d.need < 0 || d.need > 3) bad++;
    }
    CHECK(keys > 10000);
    CHECK_INT(bad, 0);
}

int main(void) {
    test_text();
    test_controls();
    test_enter();
    test_sequences();
    test_escape();
    test_paste();
    test_random();
    return checks_report("test_keys");
}
