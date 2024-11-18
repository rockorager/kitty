/*
 * line.c
 * Copyright (C) 2016 Kovid Goyal <kovid at kovidgoyal.net>
 *
 * Distributed under terms of the GPL3 license.
 */

#include "cleanup.h"
#define EXTRA_INIT register_at_exit_cleanup_func(LINE_CLEANUP_FUNC, cleanup_module);

#include "state.h"
#include "unicode-data.h"
#include "lineops.h"
#include "charsets.h"
#include "control-codes.h"

extern PyTypeObject Cursor_Type;
static_assert(sizeof(char_type) == sizeof(Py_UCS4), "Need to perform conversion to Py_UCS4");

static void
dealloc(Line* self) {
    if (self->needs_free) {
        PyMem_Free(self->cpu_cells);
        PyMem_Free(self->gpu_cells);
    }
    tc_decref(self->text_cache);
    Py_TYPE(self)->tp_free((PyObject*)self);
}

static unsigned
nonnegative_integer_as_utf32(unsigned num, ANSIBuf *output) {
    unsigned num_digits = 0;
    if (!num) num_digits = 1;
    else {
        unsigned temp = num;
        while (temp > 0) {
            temp /= 10;
            num_digits++;
        }
    }
    ensure_space_for(output, buf, output->buf[0], output->len + num_digits, capacity, 2048, false);
    if (!num) output->buf[output->len++] = '0';
    else {
        char_type *result = output->buf + output->len;
        unsigned i = num_digits - 1;
        do {
            uint32_t digit = num % 10;
            result[i--] = '0' + digit;
            num /= 10;
            output->len++;
        } while (num > 0);
    }
    return num_digits;
}

static unsigned
write_multicell_ansi_prefix(MultiCellData mcd, ANSIBuf *output) {
    unsigned pos = output->len;
    ensure_space_for(output, buf, output->buf[0], output->len + 128, capacity, 2048, false);
#define w(x) output->buf[output->len++] = x
    w(0x1b); w(']');
    for (unsigned i = 0; i < sizeof(xstr(TEXT_SIZE_CODE)) - 1; i++) w(xstr(TEXT_SIZE_CODE)[i]);
    w(';');
    if (mcd.width > 1) {
        w('w'); w('='); nonnegative_integer_as_utf32(mcd.width, output); w(':');
    }
    if (mcd.scale > 1) {
        w('s'); w('='); nonnegative_integer_as_utf32(mcd.scale, output); w(':');
    }
    if (mcd.subscale) {
        w('S'); w('='); nonnegative_integer_as_utf32(mcd.subscale, output); w(':');
    }
    if (output->buf[output->len - 1] == ':') output->len--;
    w(';');
#undef w
    return output->len - pos;
}

static unsigned
text_in_cell_ansi(const CPUCell *c, TextCache *tc, ANSIBuf *output) {
    if (c->ch_is_idx) {
        if (!c->is_multicell) return tc_chars_at_index_ansi(tc, c->ch_or_idx, output);
        if (c->x || c->y) return 0;
        MultiCellData mcd = cell_multicell_data(c, tc);
        unsigned n = write_multicell_ansi_prefix(mcd, output);
        n += tc_chars_at_index_ansi(tc, c->ch_or_idx, output);
        output->buf[output->len] = '\a';
        return n;
    }
    ensure_space_for(output, buf, output->buf[0], output->len + 1, capacity, 2048, false);
    output->buf[output->len++] = c->ch_or_idx;
    return 1;
}


unsigned int
line_length(Line *self) {
    index_type last = self->xnum - 1;
    for (index_type i = 0; i < self->xnum; i++) {
        if (!cell_is_char(self->cpu_cells + last - i, BLANK_CHAR)) return self->xnum - i;
    }
    return 0;
}

// URL detection {{{

static bool
is_hostname_char(char_type ch) {
    return ch == '[' || ch == ']' || is_url_char(ch);
}

static bool
is_hostname_lc(const ListOfChars *lc) {
    for (size_t i = 0; i < lc->count; i++) if (!is_hostname_char(lc->chars[i])) return false;
    return true;
}

static bool
is_url_lc(const ListOfChars *lc) {
    for (size_t i = 0; i < lc->count; i++) if (!is_url_char(lc->chars[i])) return false;
    return true;
}


static index_type
find_colon_slash(Line *self, index_type x, index_type limit, ListOfChars *lc) {
    // Find :// at or before x
    index_type pos = MIN(x, self->xnum - 1);
    enum URL_PARSER_STATES {ANY, FIRST_SLASH, SECOND_SLASH};
    enum URL_PARSER_STATES state = ANY;
    limit = MAX(2u, limit);
    if (pos < limit) return 0;
    do {
        const CPUCell *c = self->cpu_cells + pos;
        text_in_cell(c, self->text_cache, lc);
        if (!is_hostname_lc(lc)) return false;
        if (pos == x) {
            if (cell_is_char(c, ':')) {
                if (pos + 2 < self->xnum && cell_is_char(self->cpu_cells + pos + 1, '/') && cell_is_char(self->cpu_cells + pos + 2, '/')) state = SECOND_SLASH;
            } else if (cell_is_char(c, '/')) {
                if (pos + 1 < self->xnum && cell_is_char(self->cpu_cells + pos + 1, '/')) state = FIRST_SLASH;
            }
        }
        switch(state) {
            case ANY:
                if (cell_is_char(c, '/')) state = FIRST_SLASH;
                break;
            case FIRST_SLASH:
                state = cell_is_char(c, '/') ? SECOND_SLASH : ANY;
                break;
            case SECOND_SLASH:
                if (cell_is_char(c, ':')) return pos;
                state = cell_is_char(c, '/') ? SECOND_SLASH : ANY;
                break;
        }
        pos--;
    } while(pos >= limit);
    return 0;
}

static bool
prefix_matches(Line *self, index_type at, const char_type* prefix, index_type prefix_len) {
    if (prefix_len > at) return false;
    index_type p, i;
    for (p = at - prefix_len, i = 0; i < prefix_len && p < self->xnum; i++, p++) {
        if (!cell_is_char(self->cpu_cells + p, prefix[i])) return false;
    }
    return i == prefix_len;
}

static bool
has_url_prefix_at(Line *self, index_type at, index_type min_prefix_len, index_type *ans) {
    for (size_t i = 0; i < OPT(url_prefixes.num); i++) {
        index_type prefix_len = OPT(url_prefixes.values[i].len);
        if (at < prefix_len || prefix_len < min_prefix_len) continue;
        if (prefix_matches(self, at, OPT(url_prefixes.values[i].string), prefix_len)) { *ans = at - prefix_len; return true; }
    }
    return false;
}

#define MIN_URL_LEN 5

static bool
has_url_beyond_colon_slash(Line *self, index_type x, ListOfChars *lc) {
    unsigned num_of_slashes = 0;
    for (index_type i = x; i < MIN(x + MIN_URL_LEN + 3, self->xnum); i++) {
        const CPUCell *c = self->cpu_cells + i;
        text_in_cell(c, self->text_cache, lc);
        if (num_of_slashes < 3) {
            if (!is_hostname_lc(lc)) return false;
            if (lc->count == 1 && lc->chars[0] == '/') num_of_slashes++;
        }
        else {
            for (size_t n = 0; n < lc->count; n++) if (!is_url_char(lc->chars[n])) return false;
        }
    }
    return true;
}

index_type
line_url_start_at(Line *self, index_type x) {
    // Find the starting cell for a URL that contains the position x. A URL is defined as
    // known-prefix://url-chars. If no URL is found self->xnum is returned.
    if (x >= self->xnum || self->xnum <= MIN_URL_LEN + 3) return self->xnum;
    index_type ds_pos = 0, t;
    RAII_ListOfChars(lc);
    // First look for :// ahead of x
    ds_pos = find_colon_slash(self, x + OPT(url_prefixes).max_prefix_len + 3, x < 2 ? 0 : x - 2, &lc);
    if (ds_pos != 0 && has_url_beyond_colon_slash(self, ds_pos, &lc)) {
        if (has_url_prefix_at(self, ds_pos, ds_pos > x ? ds_pos - x: 0, &t)) return t;
    }
    ds_pos = find_colon_slash(self, x, 0, &lc);
    if (ds_pos == 0 || self->xnum < ds_pos + MIN_URL_LEN + 3 || !has_url_beyond_colon_slash(self, ds_pos, &lc)) return self->xnum;
    if (has_url_prefix_at(self, ds_pos, 0, &t)) return t;
    return self->xnum;
}

static bool
is_pos_ok_for_url(Line *self, index_type x, bool in_hostname, index_type last_hostname_char_pos, ListOfChars *lc) {
    if (x >= self->xnum) return false;
    text_in_cell(self->cpu_cells + x, self->text_cache, lc);
    if (in_hostname && x <= last_hostname_char_pos) return is_hostname_lc(lc);
    return is_url_lc(lc);
}

index_type
line_url_end_at(Line *self, index_type x, bool check_short, char_type sentinel, bool next_line_starts_with_url_chars, bool in_hostname, index_type last_hostname_char_pos) {
    index_type ans = x;
    if (x >= self->xnum || (check_short && self->xnum <= MIN_URL_LEN + 3)) return 0;
    RAII_ListOfChars(lc);
#define pos_ok(x) is_pos_ok_for_url(self, x, in_hostname, last_hostname_char_pos, &lc)
    if (sentinel) { while (ans < self->xnum && !cell_is_char(self->cpu_cells + ans, sentinel) && pos_ok(ans)) ans++; }
    else { while (ans < self->xnum && pos_ok(ans)) ans++; }
    if (ans) ans--;
    if (ans < self->xnum - 1 || !next_line_starts_with_url_chars) {
        while (ans > x && !self->cpu_cells[ans].ch_is_idx && can_strip_from_end_of_url(self->cpu_cells[ans].ch_or_idx)) ans--;
    }
#undef pos_ok
    return ans;
}

bool
line_startswith_url_chars(Line *self, bool in_hostname) {
    RAII_ListOfChars(lc);
    text_in_cell(self->cpu_cells, self->text_cache, &lc);
    if (in_hostname) return is_hostname_lc(&lc);
    return is_url_lc(&lc);
}


static PyObject*
url_start_at(Line *self, PyObject *x) {
#define url_start_at_doc "url_start_at(x) -> Return the start cell number for a URL containing x or self->xnum if not found"
    return PyLong_FromUnsignedLong((unsigned long)line_url_start_at(self, PyLong_AsUnsignedLong(x)));
}

static PyObject*
url_end_at(Line *self, PyObject *args) {
#define url_end_at_doc "url_end_at(x) -> Return the end cell number for a URL containing x or 0 if not found"
    unsigned int x, sentinel = 0;
    int next_line_starts_with_url_chars = 0;
    if (!PyArg_ParseTuple(args, "I|Ip", &x, &sentinel, &next_line_starts_with_url_chars)) return NULL;
    return PyLong_FromUnsignedLong((unsigned long)line_url_end_at(self, x, true, sentinel, next_line_starts_with_url_chars, false, self->xnum));
}

// }}}

static PyObject*
text_at(Line* self, Py_ssize_t xval) {
#define text_at_doc "[x] -> Return the text in the specified cell"
    if ((unsigned)xval >= self->xnum) { PyErr_SetString(PyExc_IndexError, "Column number out of bounds"); return NULL; }
    const CPUCell *cell = self->cpu_cells + xval;
    if (cell->ch_is_idx) {
        RAII_ListOfChars(lc);
        tc_chars_at_index(self->text_cache, cell->ch_or_idx, &lc);
        if (cell->is_multicell) {
            if (cell->x || cell->y || !lc.count) return PyUnicode_FromKindAndData(PyUnicode_4BYTE_KIND, lc.chars, 0);
            return PyUnicode_FromKindAndData(PyUnicode_4BYTE_KIND, lc.chars + 1, lc.count - 1);
        }
        return PyUnicode_FromKindAndData(PyUnicode_4BYTE_KIND, lc.chars, lc.count);
    }
    Py_UCS4 ch = cell->ch_or_idx;
    return PyUnicode_FromKindAndData(PyUnicode_4BYTE_KIND, &ch, 1);
}

size_t
cell_as_unicode_for_fallback(const ListOfChars *lc, Py_UCS4 *buf) {
    size_t n = 1;
    buf[0] = lc->chars[0] ? lc->chars[0] : ' ';
    if (buf[0] != '\t') {
        for (unsigned i = 1; i < lc->count; i++) {
            if (lc->chars[i] != VS15 && lc->chars[i] != VS16) buf[n++] = lc->chars[i];
        }
    } else buf[0] = ' ';
    return n;
}

size_t
cell_as_utf8_for_fallback(const ListOfChars *lc, char *buf) {
    char_type ch = lc->chars[0] ? lc->chars[0] : ' ';
    bool include_cc = true;
    if (ch == '\t') { ch = ' '; include_cc = false; }
    size_t n = encode_utf8(ch, buf);
    if (include_cc) {
        for (unsigned i = 1; i < lc->count; i++) {
            char_type ch = lc->chars[i];
            if (ch != VS15 && ch != VS16) n += encode_utf8(ch, buf + n);
        }
    }
    buf[n] = 0;
    return n;
}

static ListOfChars global_unicode_in_range_buf = {0};

PyObject*
unicode_in_range(const Line *self, const index_type start, const index_type limit, const bool include_cc, const bool add_trailing_newline, const bool skip_zero_cells) {
    size_t n = 0;
    ListOfChars lc;
    for (index_type i = start; i < limit; i++) {
        lc.chars = global_unicode_in_range_buf.chars + n; lc.capacity = global_unicode_in_range_buf.capacity - n;
        while (!text_in_cell_without_alloc(self->cpu_cells + i, self->text_cache, &lc)) {
            size_t ns = MAX(4096u, 2 * global_unicode_in_range_buf.capacity);
            char_type *np = realloc(global_unicode_in_range_buf.chars, ns);
            if (!np) return PyErr_NoMemory();
            global_unicode_in_range_buf.capacity = ns; global_unicode_in_range_buf.chars = np;
            lc.chars = global_unicode_in_range_buf.chars + n; lc.capacity = global_unicode_in_range_buf.capacity - n;
        }
        if (lc.is_multicell && !lc.is_topleft) continue;
        if (!lc.chars[0]) {
            if (skip_zero_cells) continue;
            lc.chars[0] = ' ';
        }
        if (lc.chars[0] == '\t') {
            n++;
            unsigned num_cells_to_skip_for_tab = lc.count > 1 ? lc.chars[1] : 0;
            while (num_cells_to_skip_for_tab && i + 1 < limit && cell_is_char(self->cpu_cells+i+1, ' ')) {
                i++;
                num_cells_to_skip_for_tab--;
            }
        } else n += include_cc ? lc.count : 1;
    }
    if (add_trailing_newline && !self->cpu_cells[self->xnum-1].next_char_was_wrapped && n < global_unicode_in_range_buf.capacity) global_unicode_in_range_buf.chars[n++] = '\n';
    return PyUnicode_FromKindAndData(PyUnicode_4BYTE_KIND, global_unicode_in_range_buf.chars, n);
}

PyObject *
line_as_unicode(Line* self, bool skip_zero_cells) {
    return unicode_in_range(self, 0, xlimit_for_line(self), true, false, skip_zero_cells);
}

static PyObject*
sprite_at(Line* self, PyObject *x) {
#define sprite_at_doc "[x] -> Return the sprite in the specified cell"
    unsigned long xval = PyLong_AsUnsignedLong(x);
    if (xval >= self->xnum) { PyErr_SetString(PyExc_IndexError, "Column number out of bounds"); return NULL; }
    GPUCell *c = self->gpu_cells + xval;
    return Py_BuildValue("HHH", c->sprite_x, c->sprite_y, c->sprite_z);
}

static void
write_sgr(const char *val, ANSIBuf *output) {
#define W(c) output->buf[output->len++] = c
    W(0x1b); W('[');
    for (size_t i = 0; val[i] != 0 && i < 122; i++) W(val[i]);
    W('m');
#undef W
}

static void
write_hyperlink(hyperlink_id_type hid, ANSIBuf *output) {
#define W(c) output->buf[output->len++] = c
    const char *key = hid ? get_hyperlink_for_id(output->hyperlink_pool, hid, false) : NULL;
    if (!key) hid = 0;
    output->active_hyperlink_id = hid;
    W(0x1b); W(']'); W('8');
    if (!hid) {
        W(';'); W(';');
    } else {
        const char* partition = strstr(key, ":");
        W(';');
        if (partition != key) {
            W('i'); W('d'); W('=');
            while (key != partition) W(*(key++));
        }
        W(';');
        while(*(++partition))  W(*partition);
    }
    W(0x1b); W('\\');
#undef W
}

static void
write_mark(const char *mark, ANSIBuf *output) {
#define W(c) output->buf[output->len++] = c
    W(0x1b); W(']'); W('1'); W('3'); W('3'); W(';');
    for (size_t i = 0; mark[i] != 0 && i < 32; i++) W(mark[i]);
    W(0x1b); W('\\');
#undef W

}

bool
line_as_ansi(Line *self, ANSIBuf *output, const GPUCell** prev_cell, index_type start_at, index_type stop_before, char_type prefix_char) {
#define ENSURE_SPACE(extra) ensure_space_for(output, buf, output->buf[0], output->len + extra, capacity, 2048, false);
#define WRITE_SGR(val) { ENSURE_SPACE(128); escape_code_written = true; write_sgr(val, output); }
#define WRITE_CH(val) { ENSURE_SPACE(1); output->buf[output->len++] = val; }
#define WRITE_HYPERLINK(val) { ENSURE_SPACE(2256); escape_code_written = true; write_hyperlink(val, output); }
#define WRITE_MARK(val) { ENSURE_SPACE(64); escape_code_written = true; write_mark(val, output); }
    bool escape_code_written = false;
    output->len = 0;
    index_type limit = MIN(stop_before, xlimit_for_line(self));
    if (prefix_char) { WRITE_CH(prefix_char); }

    switch (self->attrs.prompt_kind) {
        case UNKNOWN_PROMPT_KIND:
            break;
        case PROMPT_START:
            WRITE_MARK("A");
            break;
        case SECONDARY_PROMPT:
            WRITE_MARK("A;k=s");
            break;
        case OUTPUT_START:
            WRITE_MARK("C");
            break;
    }
    if (limit <= start_at) return escape_code_written;

    static const GPUCell blank_cell = { 0 };
    GPUCell *cell;
    if (*prev_cell == NULL) *prev_cell = &blank_cell;
    const CellAttrs mask_for_sgr = {.val=SGR_MASK};

#define CMP_ATTRS (cell->attrs.val & mask_for_sgr.val) != ((*prev_cell)->attrs.val & mask_for_sgr.val)
#define CMP(x) cell->x != (*prev_cell)->x

    for (index_type pos=start_at; pos < limit; pos++) {
        if (output->hyperlink_pool) {
            hyperlink_id_type hid = self->cpu_cells[pos].hyperlink_id;
            if (hid != output->active_hyperlink_id) {
                WRITE_HYPERLINK(hid);
            }
        }
        cell = &self->gpu_cells[pos];
        if (CMP_ATTRS || CMP(fg) || CMP(bg) || CMP(decoration_fg)) {
            const char *sgr = cell_as_sgr(cell, *prev_cell);
            if (*sgr) WRITE_SGR(sgr);
        }

        unsigned n = text_in_cell_ansi(self->cpu_cells + pos, self->text_cache, output);
        if (output->buf[output->len - n] == 0) {
            output->buf[output->len - n] = ' ';
        }

        if (output->buf[output->len - n] == '\t') {
            unsigned num_cells_to_skip_for_tab = 0;
            if (n > 1) {
                num_cells_to_skip_for_tab = output->buf[output->len - n + 1];
                output->len -= n - 1;
            }
            while (num_cells_to_skip_for_tab && pos + 1 < limit && cell_is_char(self->cpu_cells + pos + 1, ' ')) {
                num_cells_to_skip_for_tab--; pos++;
            }
        }
        *prev_cell = cell;
    }
    return escape_code_written;
#undef CMP_ATTRS
#undef CMP
#undef WRITE_SGR
#undef WRITE_CH
#undef ENSURE_SPACE
#undef WRITE_HYPERLINK
#undef WRITE_MARK
}

static PyObject*
as_ansi(Line* self, PyObject *a UNUSED) {
#define as_ansi_doc "Return the line's contents with ANSI (SGR) escape codes for formatting"
    const GPUCell *prev_cell = NULL;
    ANSIBuf output = {0};
    line_as_ansi(self, &output, &prev_cell, 0, self->xnum, 0);
    PyObject *ans = PyUnicode_FromKindAndData(PyUnicode_4BYTE_KIND, output.buf, output.len);
    free(output.buf);
    return ans;
}

static PyObject*
last_char_has_wrapped_flag(Line* self, PyObject *a UNUSED) {
#define last_char_has_wrapped_flag_doc "Return True if the last cell of this line has the wrapped flags set"
    if (self->cpu_cells[self->xnum - 1].next_char_was_wrapped) { Py_RETURN_TRUE; }
    Py_RETURN_FALSE;
}

static PyObject*
__repr__(Line* self) {
    PyObject *s = line_as_unicode(self, false);
    if (s == NULL) return NULL;
    PyObject *ans = PyObject_Repr(s);
    Py_CLEAR(s);
    return ans;
}

static PyObject*
__str__(Line* self) {
    return line_as_unicode(self, false);
}


static PyObject*
width(Line *self, PyObject *val) {
#define width_doc "width(x) -> the width of the character at x"
    unsigned long x = PyLong_AsUnsignedLong(val);
    if (x >= self->xnum) { PyErr_SetString(PyExc_ValueError, "Out of bounds"); return NULL; }
    const CPUCell *c = self->cpu_cells + x;
    if (!cell_has_text(c)) return 0;
    unsigned long ans = 1;
    if (c->is_multicell) ans = c->x || c->y ? 0 : cell_multicell_data(c, self->text_cache).width;
    return PyLong_FromUnsignedLong(ans);
}

static PyObject*
add_combining_char(Line* self, PyObject *args) {
#define add_combining_char_doc "add_combining_char(x, ch) -> Add the specified character as a combining char to the specified cell."
    int new_char;
    unsigned int x;
    if (!PyArg_ParseTuple(args, "IC", &x, &new_char)) return NULL;
    if (x >= self->xnum) {
        PyErr_SetString(PyExc_ValueError, "Column index out of bounds");
        return NULL;
    }
    CPUCell *cell = self->cpu_cells + x;
    if (cell->is_multicell) { PyErr_SetString(PyExc_IndexError, "cannot set combining char in a multicell"); return NULL; }
    RAII_ListOfChars(lc);
    text_in_cell(cell, self->text_cache, &lc);
    ensure_space_for_chars(&lc, lc.count + 1);
    lc.chars[lc.count++] = new_char;
    cell->ch_or_idx = tc_get_or_insert_chars(self->text_cache, &lc);
    cell->ch_is_idx = true;
    Py_RETURN_NONE;
}


static PyObject*
set_text(Line* self, PyObject *args) {
#define set_text_doc "set_text(src, offset, sz, cursor) -> Set the characters and attributes from the specified text and cursor"
    PyObject *src;
    Py_ssize_t offset, sz, limit;
    Cursor *cursor;
    int kind;
    void *buf;

    if (!PyArg_ParseTuple(args, "UnnO!", &src, &offset, &sz, &Cursor_Type, &cursor)) return NULL;
    if (PyUnicode_READY(src) != 0) {
        PyErr_NoMemory();
        return NULL;
    }
    kind = PyUnicode_KIND(src);
    buf = PyUnicode_DATA(src);
    limit = offset + sz;
    if (PyUnicode_GET_LENGTH(src) < limit) {
        PyErr_SetString(PyExc_ValueError, "Out of bounds offset/sz");
        return NULL;
    }
    CellAttrs attrs = cursor_to_attrs(cursor);
    color_type fg = (cursor->fg & COL_MASK), bg = cursor->bg & COL_MASK;
    color_type dfg = cursor->decoration_fg & COL_MASK;

    for (index_type i = cursor->x; offset < limit && i < self->xnum; i++, offset++) {
        self->cpu_cells[i].val = 0;
        self->cpu_cells[i].ch_or_idx = PyUnicode_READ(kind, buf, offset);
        self->gpu_cells[i].attrs = attrs;
        self->gpu_cells[i].fg = fg;
        self->gpu_cells[i].bg = bg;
        self->gpu_cells[i].decoration_fg = dfg;
    }

    Py_RETURN_NONE;
}

static PyObject*
cursor_from(Line* self, PyObject *args) {
#define cursor_from_doc "cursor_from(x, y=0) -> Create a cursor object based on the formatting attributes at the specified x position. The y value of the cursor is set as specified."
    unsigned int x, y = 0;
    Cursor* ans;
    if (!PyArg_ParseTuple(args, "I|I", &x, &y)) return NULL;
    if (x >= self->xnum) {
        PyErr_SetString(PyExc_ValueError, "Out of bounds x");
        return NULL;
    }
    ans = alloc_cursor();
    if (ans == NULL) { PyErr_NoMemory(); return NULL; }
    ans->x = x; ans->y = y;
    attrs_to_cursor(self->gpu_cells[x].attrs, ans);
    ans->fg = self->gpu_cells[x].fg; ans->bg = self->gpu_cells[x].bg;
    ans->decoration_fg = self->gpu_cells[x].decoration_fg & COL_MASK;

    return (PyObject*)ans;
}

void
line_clear_text(Line *self, unsigned int at, unsigned int num, char_type ch) {
    const CPUCell cc = {.ch_or_idx=ch};
    if (at + num > self->xnum) num = self->xnum > at ? self->xnum - at : 0;
    memset_array(self->cpu_cells + at, cc, num);
}

static PyObject*
clear_text(Line* self, PyObject *args) {
#define clear_text_doc "clear_text(at, num, ch=BLANK_CHAR) -> Clear characters in the specified range, preserving formatting."
    unsigned int at, num;
    int ch = BLANK_CHAR;
    if (!PyArg_ParseTuple(args, "II|C", &at, &num, &ch)) return NULL;
    line_clear_text(self, at, num, ch);
    Py_RETURN_NONE;
}

void
line_apply_cursor(Line *self, const Cursor *cursor, unsigned int at, unsigned int num, bool clear_char) {
    GPUCell gc = cursor_as_gpu_cell(cursor);
    if (clear_char) {
#if BLANK_CHAR != 0
#error This implementation is incorrect for BLANK_CHAR != 0
#endif
        if (at + num > self->xnum) { num = at < self->xnum ? self->xnum - at : 0; }
        memset(self->cpu_cells + at, 0, num * sizeof(CPUCell));
        memset_array(self->gpu_cells + at, gc, num);
    } else {
        for (index_type i = at; i < self->xnum && i < at + num; i++) {
            gc.attrs.mark = self->gpu_cells[i].attrs.mark;
            gc.sprite_x = self->gpu_cells[i].sprite_x; gc.sprite_y = self->gpu_cells[i].sprite_y; gc.sprite_z = self->gpu_cells[i].sprite_z;
            memcpy(self->gpu_cells + i, &gc, sizeof(gc));
        }
    }
}

static PyObject*
apply_cursor(Line* self, PyObject *args) {
#define apply_cursor_doc "apply_cursor(cursor, at=0, num=1, clear_char=False) -> Apply the formatting attributes from cursor to the specified characters in this line."
    Cursor* cursor;
    unsigned int at=0, num=1;
    int clear_char = 0;
    if (!PyArg_ParseTuple(args, "O!|IIp", &Cursor_Type, &cursor, &at, &num, &clear_char)) return NULL;
    line_apply_cursor(self, cursor, at, num, clear_char & 1);
    Py_RETURN_NONE;
}

static color_type
resolve_color(const ColorProfile *cp, color_type val, color_type defval) {
    switch(val & 0xff) {
        case 1:
            return cp->color_table[(val >> 8) & 0xff];
        case 2:
            return val >> 8;
        default:
            return defval;
    }
}

bool
colors_for_cell(Line *self, const ColorProfile *cp, index_type *x, color_type *fg, color_type *bg, bool *reversed) {
    if (*x >= self->xnum) return false;
    while (self->cpu_cells[*x].is_multicell && self->cpu_cells[*x].x && *x) (*x)--;
    *fg = resolve_color(cp, self->gpu_cells[*x].fg, *fg);
    *bg = resolve_color(cp, self->gpu_cells[*x].bg, *bg);
    if (self->gpu_cells[*x].attrs.reverse) {
        color_type t = *fg;
        *fg = *bg;
        *bg = t;
        *reversed = true;
    }
    return true;
}

char_type
line_get_char(Line *self, index_type at) {
    if (self->cpu_cells[at].ch_is_idx) {
        RAII_ListOfChars(lc);
        text_in_cell(self->cpu_cells + at, self->text_cache, &lc);
        if (lc.is_multicell && !lc.is_topleft) return 0;
        return lc.chars[0];
    } else return self->cpu_cells[at].ch_or_idx;
}


void
line_set_char(Line *self, unsigned int at, uint32_t ch, Cursor *cursor, hyperlink_id_type hyperlink_id) {
    GPUCell *g = self->gpu_cells + at;
    if (cursor != NULL) {
        g->attrs = cursor_to_attrs(cursor);
        g->fg = cursor->fg & COL_MASK;
        g->bg = cursor->bg & COL_MASK;
        g->decoration_fg = cursor->decoration_fg & COL_MASK;
    }
    CPUCell *c = self->cpu_cells + at;
    c->val = 0;
    cell_set_char(c, ch);
    c->hyperlink_id = hyperlink_id;
    if (OPT(underline_hyperlinks) == UNDERLINE_ALWAYS && hyperlink_id) {
        g->decoration_fg = ((OPT(url_color) & COL_MASK) << 8) | 2;
        g->attrs.decoration = OPT(url_style);
    }
}

static PyObject*
set_char(Line *self, PyObject *args) {
#define set_char_doc "set_char(at, ch, width=1, cursor=None, hyperlink_id=0) -> Set the character at the specified cell. If cursor is not None, also set attributes from that cursor."
    unsigned int at, width=1;
    int ch;
    Cursor *cursor = NULL;
    unsigned int hyperlink_id = 0;

    if (!PyArg_ParseTuple(args, "IC|IO!I", &at, &ch, &width, &Cursor_Type, &cursor, &hyperlink_id)) return NULL;
    if (at >= self->xnum) {
        PyErr_SetString(PyExc_ValueError, "Out of bounds");
        return NULL;
    }
    if (width != 1) {
        PyErr_SetString(PyExc_NotImplementedError, "TODO: Implement setting wide char"); return NULL;
    }
    line_set_char(self, at, ch, cursor, hyperlink_id);
    Py_RETURN_NONE;
}

static PyObject*
set_attribute(Line *self, PyObject *args) {
#define set_attribute_doc "set_attribute(which, val) -> Set the attribute on all cells in the line."
    unsigned int val;
    char *which;
    if (!PyArg_ParseTuple(args, "sI", &which, &val)) return NULL;
    if (!set_named_attribute_on_line(self->gpu_cells, which, val, self->xnum)) {
        PyErr_SetString(PyExc_KeyError, "Unknown cell attribute"); return NULL;
    }
    Py_RETURN_NONE;
}

static int
color_as_sgr(char *buf, size_t sz, unsigned long val, unsigned simple_code, unsigned aix_code, unsigned complex_code) {
    switch(val & 0xff) {
        case 1:
            val >>= 8;
            if (val < 16 && simple_code) {
                return snprintf(buf, sz, "%lu;", (val < 8) ? simple_code + val : aix_code + (val - 8));
            }
            return snprintf(buf, sz, "%u:5:%lu;", complex_code, val);
        case 2:
            return snprintf(buf, sz, "%u:2:%lu:%lu:%lu;", complex_code, (val >> 24) & 0xff, (val >> 16) & 0xff, (val >> 8) & 0xff);
        default:
            return snprintf(buf, sz, "%u;", complex_code + 1);  // reset
    }
}

static const char*
decoration_as_sgr(uint8_t decoration) {
    switch(decoration) {
        case 1: return "4;";
        case 2: return "4:2;";
        case 3: return "4:3;";
        case 4: return "4:4";
        case 5: return "4:5";
        default: return "24;";
    }
}


const char*
cell_as_sgr(const GPUCell *cell, const GPUCell *prev) {
    static char buf[128];
#define SZ sizeof(buf) - (p - buf) - 2
#define P(s) { size_t len = strlen(s); if (SZ > len) { memcpy(p, s, len); p += len; } }
    char *p = buf;
#define CA cell->attrs
#define PA prev->attrs
    bool intensity_differs = CA.bold != PA.bold || CA.dim != PA.dim;
    if (intensity_differs) {
        if (CA.bold && CA.dim) { if (!PA.bold) P("1;"); if (!PA.dim) P("2;"); }
        else {
            P("22;"); if (CA.bold) P("1;"); if (CA.dim) P("2;");
        }
    }
    if (CA.italic != PA.italic) P(CA.italic ? "3;" : "23;");
    if (CA.reverse != PA.reverse) P(CA.reverse ? "7;" : "27;");
    if (CA.strike != PA.strike) P(CA.strike ? "9;" : "29;");
    if (cell->fg != prev->fg) p += color_as_sgr(p, SZ, cell->fg, 30, 90, 38);
    if (cell->bg != prev->bg) p += color_as_sgr(p, SZ, cell->bg, 40, 100, 48);
    if (cell->decoration_fg != prev->decoration_fg) p += color_as_sgr(p, SZ, cell->decoration_fg, 0, 0, DECORATION_FG_CODE);
    if (CA.decoration != PA.decoration) P(decoration_as_sgr(CA.decoration));
#undef PA
#undef CA
#undef P
#undef SZ
    if (p > buf) *(p - 1) = 0;  // remove trailing semi-colon
    *p = 0;  // ensure string is null-terminated
    return buf;
}


static Py_ssize_t
__len__(PyObject *self) {
    return (Py_ssize_t)(((Line*)self)->xnum);
}

static int
__eq__(Line *a, Line *b) {
    return a->xnum == b->xnum && memcmp(a->cpu_cells, b->cpu_cells, sizeof(CPUCell) * a->xnum) == 0 && memcmp(a->gpu_cells, b->gpu_cells, sizeof(GPUCell) * a->xnum) == 0;
}

bool
line_has_mark(Line *line, uint16_t mark) {
    for (index_type x = 0; x < line->xnum; x++) {
        const uint16_t m = line->gpu_cells[x].attrs.mark;
        if (m && (!mark || mark == m)) return true;
    }
    return false;
}

static void
report_marker_error(PyObject *marker) {
    if (!PyObject_HasAttrString(marker, "error_reported")) {
        PyErr_Print();
        if (PyObject_SetAttrString(marker, "error_reported", Py_True) != 0) PyErr_Clear();
    } else PyErr_Clear();
}

static void
apply_mark(Line *line, const uint16_t mark, index_type *cell_pos, unsigned int *match_pos) {
#define MARK { line->gpu_cells[x].attrs.mark = mark; }
    index_type x = *cell_pos;
    MARK;
    (*match_pos)++;
    RAII_ListOfChars(lc); text_in_cell(line->cpu_cells + x, line->text_cache, &lc);
    if (lc.chars[0]) {
        if (lc.chars[0] == '\t') {
            unsigned num_cells_to_skip_for_tab = lc.count > 1 ? lc.chars[1] : 0;
            while (num_cells_to_skip_for_tab && x + 1 < line->xnum && cell_is_char(line->cpu_cells+x+1, ' ')) {
                x++;
                num_cells_to_skip_for_tab--;
                MARK;
            }
        } else if (line->cpu_cells[x].is_multicell) {
            MultiCellData mcd = {.val=lc.chars[lc.count]};
            *match_pos += lc.count - 1;
            index_type x_limit = MIN(line->xnum, mcd_x_limit(mcd));
            for (; x < x_limit; x++) { MARK; }
            x--;
        } else {
            *match_pos += lc.count - 1;
        }
    }
    *cell_pos = x + 1;
#undef MARK
}

static void
apply_marker(PyObject *marker, Line *line, const PyObject *text) {
    unsigned int l=0, r=0, col=0, match_pos=0;
    PyObject *pl = PyLong_FromVoidPtr(&l), *pr = PyLong_FromVoidPtr(&r), *pcol = PyLong_FromVoidPtr(&col);
    if (!pl || !pr || !pcol) { PyErr_Clear(); return; }
    PyObject *iter = PyObject_CallFunctionObjArgs(marker, text, pl, pr, pcol, NULL);
    Py_DECREF(pl); Py_DECREF(pr); Py_DECREF(pcol);

    if (iter == NULL) { report_marker_error(marker); return; }
    PyObject *match;
    index_type x = 0;
    while ((match = PyIter_Next(iter)) && x < line->xnum) {
        Py_DECREF(match);
        while (match_pos < l && x < line->xnum) {
            apply_mark(line, 0, &x, &match_pos);
        }
        uint16_t am = (col & MARK_MASK);
        while(x < line->xnum && match_pos <= r) {
            apply_mark(line, am, &x, &match_pos);
        }

    }
    Py_DECREF(iter);
    while(x < line->xnum) line->gpu_cells[x++].attrs.mark = 0;
    if (PyErr_Occurred()) report_marker_error(marker);
}

void
mark_text_in_line(PyObject *marker, Line *line) {
    if (!marker) {
        for (index_type i = 0; i < line->xnum; i++)  line->gpu_cells[i].attrs.mark = 0;
        return;
    }
    PyObject *text = line_as_unicode(line, false);
    if (PyUnicode_GET_LENGTH(text) > 0) {
        apply_marker(marker, line, text);
    } else {
        for (index_type i = 0; i < line->xnum; i++)  line->gpu_cells[i].attrs.mark = 0;
    }
    Py_DECREF(text);
}

PyObject*
as_text_generic(PyObject *args, void *container, get_line_func get_line, index_type lines, ANSIBuf *ansibuf, bool add_trailing_newline) {
#define APPEND(x) { PyObject* retval = PyObject_CallFunctionObjArgs(callback, x, NULL); if (!retval) return NULL; Py_DECREF(retval); }
#define APPEND_AND_DECREF(x) { if (x == NULL) { if (PyErr_Occurred()) return NULL; Py_RETURN_NONE; } PyObject* retval = PyObject_CallFunctionObjArgs(callback, x, NULL); Py_CLEAR(x); if (!retval) return NULL; Py_DECREF(retval); }
    PyObject *callback;
    int as_ansi = 0, insert_wrap_markers = 0;
    if (!PyArg_ParseTuple(args, "O|pp", &callback, &as_ansi, &insert_wrap_markers)) return NULL;
    PyObject *t = NULL;
    RAII_PyObject(nl, PyUnicode_FromString("\n"));
    RAII_PyObject(cr, PyUnicode_FromString("\r"));
    RAII_PyObject(sgr_reset, PyUnicode_FromString("\x1b[m"));
    if (nl == NULL || cr == NULL || sgr_reset == NULL) return NULL;
    const GPUCell *prev_cell = NULL;
    ansibuf->active_hyperlink_id = 0;
    bool need_newline = false;
    for (index_type y = 0; y < lines; y++) {
        Line *line = get_line(container, y);
        if (!line) { if (PyErr_Occurred()) return NULL; break; }
        if (need_newline) APPEND(nl);
        if (as_ansi) {
            // less has a bug where it resets colors when it sees a \r, so work
            // around it by resetting SGR at the start of every line. This is
            // pretty sad performance wise, but I guess it will remain till I
            // get around to writing a nice pager kitten.
            // see https://github.com/kovidgoyal/kitty/issues/2381
            prev_cell = NULL;
            line_as_ansi(line, ansibuf, &prev_cell, 0, line->xnum, 0);
            t = PyUnicode_FromKindAndData(PyUnicode_4BYTE_KIND, ansibuf->buf, ansibuf->len);
            if (t && ansibuf->len > 0) APPEND(sgr_reset);
        } else {
            t = line_as_unicode(line, false);
        }
        APPEND_AND_DECREF(t);
        if (insert_wrap_markers) APPEND(cr);
        need_newline = !line->cpu_cells[line->xnum-1].next_char_was_wrapped;
    }
    if (need_newline && add_trailing_newline) APPEND(nl);
    if (ansibuf->active_hyperlink_id) {
        ansibuf->active_hyperlink_id = 0;
        t = PyUnicode_FromString("\x1b]8;;\x1b\\");
        APPEND_AND_DECREF(t);
    }
    Py_RETURN_NONE;
#undef APPEND
#undef APPEND_AND_DECREF
}

// Boilerplate {{{
static PyObject*
copy_char(Line* self, PyObject *args);
#define copy_char_doc "copy_char(src, to, dest) -> Copy the character at src to the character dest in the line `to`"

#define hyperlink_ids_doc "hyperlink_ids() -> Tuple of hyper link ids at every cell"
static PyObject*
hyperlink_ids(Line *self, PyObject *args UNUSED) {
    PyObject *ans = PyTuple_New(self->xnum);
    for (index_type x = 0; x < self->xnum; x++) {
        PyTuple_SET_ITEM(ans, x, PyLong_FromUnsignedLong(self->cpu_cells[x].hyperlink_id));
    }
    return ans;
}


static PyObject *
richcmp(PyObject *obj1, PyObject *obj2, int op);


static PySequenceMethods sequence_methods = {
    .sq_length = __len__,
    .sq_item = (ssizeargfunc)text_at
};

static PyMethodDef methods[] = {
    METHOD(add_combining_char, METH_VARARGS)
    METHOD(set_text, METH_VARARGS)
    METHOD(cursor_from, METH_VARARGS)
    METHOD(apply_cursor, METH_VARARGS)
    METHOD(clear_text, METH_VARARGS)
    METHOD(copy_char, METH_VARARGS)
    METHOD(set_char, METH_VARARGS)
    METHOD(set_attribute, METH_VARARGS)
    METHOD(as_ansi, METH_NOARGS)
    METHOD(last_char_has_wrapped_flag, METH_NOARGS)
    METHOD(hyperlink_ids, METH_NOARGS)
    METHOD(width, METH_O)
    METHOD(url_start_at, METH_O)
    METHOD(url_end_at, METH_VARARGS)
    METHOD(sprite_at, METH_O)

    {NULL}  /* Sentinel */
};

PyTypeObject Line_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "fast_data_types.Line",
    .tp_basicsize = sizeof(Line),
    .tp_dealloc = (destructor)dealloc,
    .tp_repr = (reprfunc)__repr__,
    .tp_str = (reprfunc)__str__,
    .tp_as_sequence = &sequence_methods,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_richcompare = richcmp,
    .tp_doc = "Lines",
    .tp_methods = methods,
};

Line *alloc_line(TextCache *tc) {
    Line *ans = (Line*)Line_Type.tp_alloc(&Line_Type, 0);
    if (ans) ans->text_cache = tc_incref(tc);
    return ans;
}

static void
cleanup_module(void) {
    free(global_unicode_in_range_buf.chars);
    global_unicode_in_range_buf = (ListOfChars){0};
}

RICHCMP(Line)
INIT_TYPE(Line)
// }}}

static PyObject*
copy_char(Line* self, PyObject *args) {
    unsigned int src, dest;
    Line *to;
    if (!PyArg_ParseTuple(args, "IO!I", &src, &Line_Type, &to, &dest)) return NULL;
    if (src >= self->xnum || dest >= to->xnum) {
        PyErr_SetString(PyExc_ValueError, "Out of bounds");
        return NULL;
    }
    COPY_CELL(self, src, to, dest);
    Py_RETURN_NONE;
}
