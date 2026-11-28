// Tests for the terminal markdown renderer (src/ui/md.c).
//
// The renderer writes bytes for a terminal: text and SGR escapes. The tests look at two views of
// it: the text alone (the escapes taken out), which is what a reader sees, and the raw bytes,
// where the styles are. A random run then feeds it markdown-shaped noise and checks what must
// hold for any reply: no escape but a style ever reaches the terminal, no row is wider than the
// terminal, the terminal is left unstyled, and no letter of the reply goes missing.
#include "check.h"
#include "../src/ui/md.h"
#include "../src/ui/edit.h"

static char out[1 << 17];
static size_t outn;

static void sink(void* ctx, const char* s, size_t n) {
    (void)ctx;
    if (outn + n > sizeof out) { printf("test output overflow\n"); exit(2); }
    memcpy(out + outn, s, n);
    outn += n;
}

static md_t md;

static const char* render_w(const char* s, int width, int ascii) {
    outn = 0;
    md_init(&md, sink, 0, width, ascii);
    md_render(&md, s, strlen(s));
    out[outn] = 0;
    return out;
}
static const char* render(const char* s) { return render_w(s, 40, 0); }

// The text without its escapes.
static char text[1 << 17];
static const char* plain(const char* s) {
    size_t k = 0;
    for (size_t i = 0; s[i];) {
        if (s[i] == 0x1B && s[i + 1] == '[') {
            i += 2;
            while (s[i] && !(s[i] >= 0x40 && s[i] <= 0x7E)) i++;
            if (s[i]) i++;
            continue;
        }
        text[k++] = s[i++];
    }
    text[k] = 0;
    return text;
}

#define SHOWS(md_in, want) CHECK_STR(str_z(plain(render(md_in))), (want))
#define RAW_HAS(md_in, needle) do { const char* r_ = render(md_in); checks_run++; \
    if (!strstr(r_, (needle))) { checks_failed++; printf("FAIL %s:%d: output of \"%s\" lacks \"%s\": %s\n", __FILE__, __LINE__, (md_in), (needle), r_); } } while (0)
#define RAW_LACKS(md_in, needle) do { const char* r_ = render(md_in); checks_run++; \
    if (strstr(r_, (needle))) { checks_failed++; printf("FAIL %s:%d: output of \"%s\" has \"%s\": %s\n", __FILE__, __LINE__, (md_in), (needle), r_); } } while (0)

static void test_text(void) {
    SHOWS("", "\n");
    SHOWS("hello", "hello\n");
    SHOWS("a\nb", "a\nb\n");
    SHOWS("a\r\nb\r\n", "a\nb\n\n");
    SHOWS("a\n\nb", "a\n\nb\n");
    // Wrapping at width - 1, on words.
    SHOWS("aaaa bbbb cccc dddd eeee ffff gggg hhhh iiii", "aaaa bbbb cccc dddd eeee ffff gggg hhhh\niiii\n");
    SHOWS("aaaa bbbb cccc dddd eeee ffff gggg hhhhh", "aaaa bbbb cccc dddd eeee ffff gggg\nhhhhh\n");
    // A word longer than a row starts a row of its own and breaks where that fills.
    SHOWS("x 0123456789012345678901234567890123456789ab",
          "x\n012345678901234567890123456789012345678\n9ab\n");
    SHOWS("trailing spaces   ", "trailing spaces\n");
    SHOWS("  indented", "  indented\n");
    // The narrowest wrap is 20 columns.
    CHECK_STR(str_z(plain(render_w("aaaa bbbb cccc dddd eeee", 5, 0))), "aaaa bbbb cccc dddd\neeee\n");
    // Wide characters take two columns.
    CHECK_STR(str_z(plain(render_w("\xE4\xB8\xAD\xE4\xB8\xAD\xE4\xB8\xAD\xE4\xB8\xAD\xE4\xB8\xAD\xE4\xB8\xAD\xE4\xB8\xAD\xE4\xB8\xAD\xE4\xB8\xAD\xE4\xB8\xAD x", 21, 0))),
              "\xE4\xB8\xAD\xE4\xB8\xAD\xE4\xB8\xAD\xE4\xB8\xAD\xE4\xB8\xAD\xE4\xB8\xAD\xE4\xB8\xAD\xE4\xB8\xAD\xE4\xB8\xAD\xE4\xB8\xAD\nx\n");
}

static void test_inline(void) {
    SHOWS("**bold** and *it* and _it_ and __b__", "bold and it and it and b\n");
    RAW_HAS("**bold**", "\033[0;1mbold\033[0m");
    RAW_HAS("*it*", "\033[0;3mit\033[0m");
    RAW_HAS("~~gone~~", "\033[0;9mgone\033[0m");
    RAW_HAS("***both***", "\033[0;1;3mboth");
    RAW_HAS("`code`", "\033[0;36mcode\033[0m");
    SHOWS("`a*b*c`", "a*b*c\n");
    SHOWS("`` a`b ``", "a`b\n");
    SHOWS("x `` a`b `` y", "x a`b y\n");      // the padding inside the backticks goes
    SHOWS("``x`", "``x`\n");
    SHOWS("a * b * c", "a * b * c\n");
    SHOWS("2*3*4", "234\n");                 // as CommonMark has it: * works inside a word, _ does not
    SHOWS("snake_case_name", "snake_case_name\n");
    SHOWS("foo_bar_ baz", "foo_bar_ baz\n");  // an _ inside a word opens nothing
    SHOWS("*not closed", "*not closed\n");
    SHOWS("**not closed", "**not closed\n");
    SHOWS("\\*literal\\*", "*literal*\n");
    SHOWS("back\\slash \\d", "back\\slash \\d\n");
    SHOWS("~single~", "~single~\n");
    SHOWS("[the docs](https://x.org/a)", "the docs (https://x.org/a)\n");
    RAW_HAS("[the docs](https://x.org/a)", "\033[0;4mthe docs\033[0;2m (https://x.org/a)");
    SHOWS("[https://x.org](https://x.org)", "https://x.org\n");
    SHOWS("![a cat](cat.png)", "a cat (cat.png)\n");
    SHOWS("![](cat.png)", "image (cat.png)\n");
    SHOWS("[not a link] (x)", "[not a link] (x)\n");
    SHOWS("[a [nested] one](u)", "a [nested] one (u)\n");
    SHOWS("[t](f(x))", "t (f(x))\n");
    SHOWS("see <https://x.org> now", "see https://x.org now\n");
    RAW_HAS("<https://x.org>", "\033[0;4mhttps://x.org\033[0m");
    SHOWS("a <b> c", "a <b> c\n");
    SHOWS("!bang", "!bang\n");
    // Styles stop at the end of a line.
    RAW_HAS("**b**\nnext", "b\033[0m\nnext");
}

static void test_safe(void) {
    SHOWS("a\033[2Jb", "a^[[2Jb\n");
    RAW_LACKS("a\033[2Jb", "\033[2J");
    SHOWS("bell\a", "bell^G\n");
    SHOWS("del\x7F", "del^?\n");
    SHOWS("tab\there", "tab here\n");
    SHOWS("c1\xC2\x9B" "2J", "c1\xEF\xBF\xBD" "2J\n");
    SHOWS("```\n\033]0;title\a\n```", "```\n^[]0;title^G\n```\n");
    // md_safe on its own.
    outn = 0;
    md_safe(sink, 0, "x\r\ny\rz\t\033\xC2\x85\xC2\xA0", 12);
    CHECK_STR(str_of(out, outn), "x\ny^Mz\t^[\xEF\xBF\xBD\xC2\xA0");
}

static void test_blocks(void) {
    SHOWS("# Title", "Title\n");
    RAW_HAS("# Title", "\033[0;1;4mTitle");
    RAW_HAS("### Sub ###", "\033[0;1mSub\033[0m\n");
    SHOWS("### Sub ###", "Sub\n");
    SHOWS("## C# is a language", "C# is a language\n");
    SHOWS("#hashtag", "#hashtag\n");
    SHOWS("####### seven", "####### seven\n");
    SHOWS("---", "\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80"
                  "\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80"
                  "\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80"
                  "\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\n");
    CHECK_STR(str_z(plain(render_w("* * *", 25, 1))), "------------------------\n");
    SHOWS("- one\n- two", "\xE2\x80\xA2 one\n\xE2\x80\xA2 two\n");
    CHECK_STR(str_z(plain(render_w("+ one", 40, 1))), "- one\n");
    SHOWS("  - nested", "  \xE2\x80\xA2 nested\n");
    SHOWS("-not a list", "-not a list\n");
    SHOWS("--", "--\n");                       // two are not a rule
    SHOWS("12) aaaa bbbb cccc dddd eeee ffff gggg hhhh", "12) aaaa bbbb cccc dddd eeee ffff gggg\n    hhhh\n");
    // A hanging indent under the bullet.
    SHOWS("- aaaa bbbb cccc dddd eeee ffff gggg hhhh iiii",
          "\xE2\x80\xA2 aaaa bbbb cccc dddd eeee ffff gggg\n  hhhh iiii\n");
    SHOWS("1. first\n10) tenth", "1. first\n10) tenth\n");
    SHOWS("12. aaaa bbbb cccc dddd eeee ffff gggg hhhh", "12. aaaa bbbb cccc dddd eeee ffff gggg\n    hhhh\n");
    SHOWS("2024. was a year", "2024. was a year\n");
    SHOWS("> quoted", "\xE2\x94\x82 quoted\n");
    SHOWS(">> twice", "\xE2\x94\x82 \xE2\x94\x82 twice\n");
    SHOWS("> > spaced", "\xE2\x94\x82 \xE2\x94\x82 spaced\n");
    CHECK_STR(str_z(plain(render_w("> q", 40, 1))), "| q\n");
    SHOWS("> aaaa bbbb cccc dddd eeee ffff gggg hhhh",
          "\xE2\x94\x82 aaaa bbbb cccc dddd eeee ffff gggg\n\xE2\x94\x82 hhhh\n");
    RAW_HAS("> **b**", "\033[0;1mb");
}

static void test_fences(void) {
    SHOWS("```c\nint *p = **q;\n# not a heading\n```\nafter *it*", "```c\nint *p = **q;\n# not a heading\n```\nafter it\n");
    RAW_HAS("```\nx\n```", "\033[0;2m```\033[0m\n");
    // Long code lines are not wrapped (so they copy whole).
    SHOWS("```\naaaa bbbb cccc dddd eeee ffff gggg hhhh iiii\n```", "```\naaaa bbbb cccc dddd eeee ffff gggg hhhh iiii\n```\n");
    // The closing fence must be as long, and of the same kind.
    SHOWS("````\n```\n~~~\n````\n*x*", "````\n```\n~~~\n````\nx\n");
    SHOWS("~~~\n```\n~~~", "~~~\n```\n~~~\n");
    SHOWS("~~~\n```\n*x*\n~~~", "~~~\n```\n*x*\n~~~\n");
    SHOWS("``` not closed\n*x*", "``` not closed\n*x*\n");
    SHOWS("```inline``` code", "inline code\n");
    SHOWS("    ```\n*x*", "    ```\nx\n");
}

static void test_tables(void) {
    SHOWS("| a | bb |\n|---|---:|\n| 1 | 2 |\n| 333 | 4 |\nafter",
          "a   \xE2\x94\x82 bb\n"
          "\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\xBC\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\n"
          "1   \xE2\x94\x82  2\n"
          "333 \xE2\x94\x82  4\n"
          "after\n");
    CHECK_STR(str_z(plain(render_w("a|b\n:-:|-\nxxx|y", 40, 1))), " a  | b\n----+--\nxxx | y\n");
    RAW_HAS("| **h** |\n|---|\n| `c` |", "\033[0;1mh");
    RAW_HAS("| h |\n|---|\n| `c` |", "\033[0;36mc");
    // A pipe inside code or escaped is not a column.
    CHECK_STR(str_z(plain(render_w("x | y\n--|--\n`a|b` | c\\|d", 40, 1))), "x   | y\n----+----\na|b | c|d\n");
    // Missing cells are empty; extra ones are dropped.
    CHECK_STR(str_z(plain(render_w("a|b\n-|-\n|1\n1|2|3", 40, 1))), "a | b\n--+--\n1 | \n1 | 2\n");
    // The header's cells must match the delimiter row's, or it is no table.
    SHOWS("a | b\n---\nc", "a | b\n\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80"
                             "\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80"
                             "\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80"
                             "\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\nc\n");
    SHOWS("a | b\n|-x-|---|", "a | b\n|-x-|---|\n");
    SHOWS("a|b\n-|::", "a|b\n-|::\n");          // a delimiter cell needs a dash
    // Columns are counted in columns, not bytes.
    CHECK_STR(str_z(plain(render_w("\xC3\xA9|b\n-|-\nx|y", 40, 1))), "\xC3\xA9 | b\n--+--\nx | y\n");
    // A blank line, or a line with no pipe, ends the table.
    CHECK_STR(str_z(plain(render_w("a|b\n-|-\n1|2\n\n3|4", 40, 1))), "a | b\n--+--\n1 | 2\n\n3|4\n");
    CHECK_STR(str_z(plain(render_w("a|b\n-|-\n1|2\nno pipe", 40, 1))), "a | b\n--+--\n1 | 2\nno pipe\n");
    // Ending the reply.
    CHECK_STR(str_z(plain(render_w("a|b\n-|-", 40, 1))), "a | b\n--+--\n");
    CHECK_STR(str_z(plain(render_w("a|b\n-|-\n", 40, 1))), "a | b\n--+--\n\n");
    CHECK_STR(str_z(plain(render_w("a|b\n-|-\n1|2\n", 40, 1))), "a | b\n--+--\n1 | 2\n\n");
    // Too wide for the terminal: a record for each row.
    CHECK_STR(str_z(plain(render_w("| name | what it does |\n|---|---|\n| edit.c | the line editor, which draws the prompt |\n| md.c | markdown |", 30, 1))),
              "name: edit.c\n"
              "what it does: the line\n"
              "  editor, which draws the\n"
              "  prompt\n"
              "\n"
              "name: md.c\n"
              "what it does: markdown\n");
    RAW_HAS("| name | what it does is quite long here |\n|---|---|\n| edit.c | the line editor, which draws the prompt |", "\033[0;1mname\033[0m: edit.c");
    CHECK_STR(str_z(plain(render_w("| a long header | and another one | and a third |\n|---|---|---|", 30, 1))),
              "a long header | and another\none | and a third\n");
}

static void test_limits(void) {
    // A line too long to style is shown as it is, still made safe.
    static char big[MD_LINE + 100];
    memset(big, 'x', sizeof big - 1);
    big[0] = '*';
    big[1] = '*';
    big[100] = 0x1B;
    big[sizeof big - 3] = '*';
    big[sizeof big - 2] = '*';
    big[sizeof big - 1] = 0;
    const char* r = render(big);
    CHECK(r[0] == '*' && r[1] == '*');
    CHECK(strstr(r, "^[") != 0);
    CHECK(!strchr(r, 0x1B));
    CHECK_INT(strlen(r), sizeof big + 1);   // two more: the ^, and the newline
    // A table too big to keep is shown as plain lines.
    static char tbl[MD_ROWS * 40 + 100];
    size_t k = 0;
    k += (size_t)sprintf(tbl + k, "a|b\n-|-\n");
    for (int i = 0; i < MD_ROWS + 5; i++) k += (size_t)sprintf(tbl + k, "%d|*%d*\n", i, i);
    k += (size_t)sprintf(tbl + k, "after");
    const char* p = plain(render(tbl));
    CHECK(!strncmp(p, "a|b\n-|-\n0|0\n1|1\n", 16));
    CHECK(strstr(p, "\n68|68\nafter\n") != 0);
    // A row with more cells than a table may have.
    CHECK_STR(str_z(plain(render_w("a|b\n-|-\n1|2|3|4|5|6|7|8|9|10|11|12|13|14|15", 40, 1))), "a | b\n--+--\n1 | 2\n");
    // Thirteen columns are too many: no table.
    SHOWS("a|b|c|d|e|f|g|h|i|j|k|l|m\n-|-|-|-|-|-|-|-|-|-|-|-|-", "a|b|c|d|e|f|g|h|i|j|k|l|m\n-|-|-|-|-|-|-|-|-|-|-|-|-\n");
    // Cells too big for the table's store.
    static char wide[MD_TABLE + 200];
    k = (size_t)sprintf(wide, "h|i\n-|-\n");
    for (int i = 0; i < 4; i++) {
        memset(wide + k, 'y', MD_TABLE / 4);
        k += MD_TABLE / 4;
        k += (size_t)sprintf(wide + k, "|z\n");
    }
    wide[k] = 0;
    p = plain(render_w(wide, 40, 1));
    CHECK(!strncmp(p, "h|i\n-|-\nyyy", 11));
}

// ---- random replies ----

static uint32_t seed = 12345;
static uint32_t rnd(void) { seed = seed * 1103515245u + 12345u; return (seed >> 8) & 0xFFFFFF; }

static const char* pieces[] = {
    "*", "**", "_", "__", "~~", "`", "``", "```", "~~~", "[", "]", "(", ")", "![", "<", ">", "<https://a.b>",
    "\\", "\\*", "#", "# ", "## ", "- ", "* ", "+ ", "1. ", "12) ", "> ", ">> ", "|", " | ", "|---|", "---", ":--", "--:",
    " ", " ", " ", "  ", "\n", "\n", "\n\n", "\r\n", "\t", "\033", "\a", "\x7F", "\xC2\x9B", "\xC3\xA9", "\xE4\xB8\xAD",
    "\xF0\x9F\x98\x80", "\xCC\x81", "word", "a", "bc", "xyz", "longerwordthanmost", "0123456789012345678901234567890123456789",
};

static int alnum(unsigned char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

// Every escape is a style (ESC [ digits and semicolons m); every row fits; the end is unstyled.
static void check_output(const char* in, const char* o, size_t on, int width, int wrapped) {
    int bad = 0;
    for (size_t i = 0; i < on; i++) {
        if (o[i] != 0x1B) continue;
        size_t j = i + 1;
        if (j >= on || o[j] != '[') { bad = 1; break; }
        j++;
        while (j < on && ((o[j] >= '0' && o[j] <= '9') || o[j] == ';')) j++;
        if (j >= on || o[j] != 'm') { bad = 1; break; }
    }
    CHECK(!bad);
    CHECK(on > 0 && o[on - 1] == '\n');
    // The last style set is a reset, if there was one.
    const char* last = 0;
    for (size_t i = 0; i + 1 < on; i++) if (o[i] == 0x1B) last = o + i;
    CHECK(!last || !strncmp(last, "\033[0m", 4));
    // Columns of each row, once the escapes are out.
    const char* p = plain(o);
    if (wrapped) {
        int col = 0, maxcol = 0;
        for (size_t i = 0; p[i];) {
            if (p[i] == '\n') { col = 0; i++; continue; }
            int len;
            uint32_t cp;
            unsigned char u = (unsigned char)p[i];
            if (u < 0x80) { cp = u; len = 1; }
            else if ((u & 0xE0) == 0xC0 && p[i + 1]) { cp = ((uint32_t)(u & 0x1F) << 6) | (p[i + 1] & 0x3F); len = 2; }
            else if ((u & 0xF0) == 0xE0 && p[i + 1] && p[i + 2]) { cp = ((uint32_t)(u & 0x0F) << 12) | ((uint32_t)(p[i + 1] & 0x3F) << 6) | (p[i + 2] & 0x3F); len = 3; }
            else if (p[i + 1] && p[i + 2] && p[i + 3]) { cp = ((uint32_t)(u & 0x07) << 18) | ((uint32_t)(p[i + 1] & 0x3F) << 12) | ((uint32_t)(p[i + 2] & 0x3F) << 6) | (p[i + 3] & 0x3F); len = 4; }
            else { cp = '?'; len = 1; }
            col += ed_cols(cp);
            if (col > maxcol) maxcol = col;
            i += (size_t)len;
        }
        int W = width - 1 < 20 ? 20 : width - 1;
        checks_run++;
        if (maxcol > W) { checks_failed++; printf("FAIL: a row of %d columns at width %d for:\n%s\n---\n%s\n", maxcol, width, in, p); }
    }
}

static void test_random(void) {
    static char in[4096];
    for (int round = 0; round < 40000; round++) {
        size_t k = 0;
        int np = 1 + (int)(rnd() % 40);
        for (int i = 0; i < np; i++) {
            const char* pc = pieces[rnd() % (sizeof pieces / sizeof *pieces)];
            size_t l = strlen(pc);
            if (k + l >= sizeof in) break;
            memcpy(in + k, pc, l);
            k += l;
        }
        in[k] = 0;
        int width = 20 + (int)(rnd() % 60), ascii = (int)(rnd() % 2);
        render_w(in, width, ascii);
        // Code is not wrapped, and a code line may be wider than the terminal.
        int wrapped = !strstr(in, "```") && !strstr(in, "~~~");
        check_output(in, out, outn, width, wrapped);
        // Every letter and digit of the reply is shown, in order, unless a table repeats its
        // headers or an image without a description gets one.
        if (!strchr(in, '|') && !strstr(in, "![")) {
            const char* p = plain(out);
            size_t j = 0, missing = 0;
            for (size_t i = 0; in[i]; i++) {
                if (!alnum((unsigned char)in[i])) continue;
                while (p[j] && p[j] != in[i]) j++;
                if (!p[j]) { missing = 1; break; }
                j++;
            }
            checks_run++;
            if (missing) { checks_failed++; printf("FAIL: letters lost from:\n%s\n---\n%s\n", in, p); }
        }
        if (checks_failed > 20) return;
    }
}

int main(void) {
    test_text();
    test_inline();
    test_safe();
    test_blocks();
    test_fences();
    test_tables();
    test_limits();
    test_random();
    return checks_report("test_md");
}
