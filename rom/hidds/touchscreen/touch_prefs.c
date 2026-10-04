/* Touchscreen preferences text format; see touch_prefs.h. */

#include "touch_prefs.h"

/* No C library here: the HIDD links without one. */

static int prefs_is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r';
}

static size_t prefs_strlen(const char *s)
{
    size_t n = 0;

    while (s[n])
        ++n;
    return n;
}

/* Does [key, key + key_length) equal the NUL-terminated name? */
static int prefs_key_is(const char *key, size_t key_length, const char *name)
{
    size_t i;

    for (i = 0; i < key_length; ++i)
        if (!name[i] || key[i] != name[i])
            return 0;
    return name[i] == 0;
}

/* Unsigned decimal, the whole span; 0 on any other character or overflow. */
static int prefs_number(const char *s, size_t n, uint32_t *out)
{
    uint32_t v = 0;
    size_t i;

    if (!n)
        return 0;
    for (i = 0; i < n; ++i)
    {
        if (s[i] < '0' || s[i] > '9' || v > 429496729U)
            return 0;
        v = v * 10 + (uint32_t)(s[i] - '0');
    }
    *out = v;
    return 1;
}

static int prefs_flags(const char *s, size_t n, uint32_t *out)
{
    uint32_t f = 0;
    size_t i;

    if (n == 1 && s[0] == '-')
    {
        *out = 0;
        return 1;
    }
    if (!n)
        return 0;
    for (i = 0; i < n; ++i)
    {
        if (s[i] == 's')
            f |= TOUCH_PREFS_SWAP_XY;
        else if (s[i] == 'x')
            f |= TOUCH_PREFS_MIRROR_X;
        else if (s[i] == 'y')
            f |= TOUCH_PREFS_MIRROR_Y;
        else
            return 0;
    }
    *out = f;
    return 1;
}

/* x_min,x_max,y_min,y_max,flags with x_min < x_max and y_min < y_max. */
static int prefs_cal(const char *s, size_t n, struct TouchCal *out)
{
    uint32_t v[4], flags = 0;
    size_t start = 0, i, field = 0;

    for (i = 0; i <= n; ++i)
    {
        if (i < n && s[i] != ',')
            continue;
        if (field < 4)
        {
            if (!prefs_number(s + start, i - start, &v[field]))
                return 0;
        }
        else if (field == 4)
        {
            if (!prefs_flags(s + start, i - start, &flags))
                return 0;
        }
        else
            return 0;
        ++field;
        start = i + 1;
    }
    if (field != 5 || v[0] >= v[1] || v[2] >= v[3]
        || v[1] > 65535U || v[3] > 65535U)
        return 0;
    out->x_min = v[0];
    out->x_max = v[1];
    out->y_min = v[2];
    out->y_max = v[3];
    out->flags = flags;
    return 1;
}

static void prefs_line(struct TouchPrefs *p, const char *key, size_t kn,
                       const char *val, size_t vn, const char *name)
{
    uint32_t v;
    size_t bn = prefs_strlen(name);

    if (kn == 12 + bn && prefs_key_is(key, 12, "calibration.")
        && prefs_key_is(key + 12, bn, name))
    {
        struct TouchCal cal;

        if (prefs_cal(val, vn, &cal))
        {
            p->cal = cal;
            p->have |= TOUCH_PREFS_HAVE_CAL;
        }
        return;
    }
    if (prefs_key_is(key, kn, "mode"))
    {
        if (prefs_key_is(val, vn, "tap"))
            p->mode = TOUCH_TAP;
        else if (prefs_key_is(val, vn, "direct"))
            p->mode = TOUCH_DIRECT;
        else
            return;
        p->have |= TOUCH_PREFS_HAVE_MODE;
        return;
    }
    if (!prefs_number(val, vn, &v))
        return;
    /* Bounds keep a hand-edited file from making the touch unusable. */
    if (prefs_key_is(key, kn, "hold_ms") && v >= 100 && v <= 3000)
        p->params.hold_ms = v;
    else if (prefs_key_is(key, kn, "slop_px") && v >= 1 && v <= 64)
        p->params.slop_px = (int32_t)v;
    else if (prefs_key_is(key, kn, "release_polls") && v >= 1 && v <= 10)
        p->params.release_polls = v;
    else if (prefs_key_is(key, kn, "tapdrag_ms") && v <= 1000)
        p->params.tapdrag_ms = v;
    else if (prefs_key_is(key, kn, "tapdrag_px") && v >= 1 && v <= 256)
        p->params.tapdrag_px = (int32_t)v;
    else if (prefs_key_is(key, kn, "two_finger_right") && v <= 1)
        p->params.two_finger_right = v;
    else
        return;
    p->have |= TOUCH_PREFS_HAVE_PARAMS;
}

/* Calls fn for every "key=value" line, trimmed, comments skipped. */
static void prefs_each(const char *text, size_t length,
                       void (*fn)(void *, const char *, size_t,
                                  const char *, size_t, const char *,
                                  size_t),
                       void *context)
{
    size_t pos = 0;

    while (pos < length)
    {
        size_t start = pos, end, eq, k0, k1, v0, v1;

        while (pos < length && text[pos] != '\n')
            ++pos;
        end = pos++;
        k0 = start;
        while (k0 < end && prefs_is_space(text[k0]))
            ++k0;
        if (k0 == end || text[k0] == '#')
            continue;
        for (eq = k0; eq < end && text[eq] != '='; ++eq)
            ;
        if (eq == end)
            continue;
        k1 = eq;
        while (k1 > k0 && prefs_is_space(text[k1 - 1]))
            --k1;
        v0 = eq + 1;
        while (v0 < end && prefs_is_space(text[v0]))
            ++v0;
        v1 = end;
        while (v1 > v0 && prefs_is_space(text[v1 - 1]))
            --v1;
        fn(context, text + k0, k1 - k0, text + v0, v1 - v0,
           text + start, end - start);
    }
}

struct prefs_parse_ctx
{
    struct TouchPrefs *prefs;
    const char *name;
};

static void prefs_parse_fn(void *context, const char *key, size_t kn,
                           const char *val, size_t vn, const char *line,
                           size_t ln)
{
    struct prefs_parse_ctx *c = context;

    (void)line;
    (void)ln;
    prefs_line(c->prefs, key, kn, val, vn, c->name);
}

void touch_prefs_parse(struct TouchPrefs *prefs, const char *text,
                         size_t length, const char *name)
{
    struct prefs_parse_ctx c;

    c.prefs = prefs;
    c.name = name;
    prefs->have = 0;
    prefs_each(text, length, prefs_parse_fn, &c);
}

/* Output buffer with overflow tracking. */
struct prefs_out
{
    char *buf;
    size_t size, len;
    int overflow;
};

static void out_bytes(struct prefs_out *o, const char *s, size_t n)
{
    size_t i;

    for (i = 0; i < n; ++i)
    {
        if (o->len + 1 >= o->size)
        {
            o->overflow = 1;
            return;
        }
        o->buf[o->len++] = s[i];
    }
}

static void out_str(struct prefs_out *o, const char *s)
{
    out_bytes(o, s, prefs_strlen(s));
}

static void out_num(struct prefs_out *o, uint32_t v)
{
    char d[10];
    int n = 0;

    do
    {
        d[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n)
        out_bytes(o, &d[--n], 1);
}

static void out_kv(struct prefs_out *o, const char *key, uint32_t v)
{
    out_str(o, key);
    out_str(o, "=");
    out_num(o, v);
    out_str(o, "\n");
}

struct prefs_keep_ctx
{
    struct prefs_out *out;
    const char *name;
};

/* Carry over other names' calibration lines verbatim. */
static void prefs_keep_fn(void *context, const char *key, size_t kn,
                          const char *val, size_t vn, const char *line,
                          size_t ln)
{
    struct prefs_keep_ctx *c = context;
    size_t bn = prefs_strlen(c->name);

    (void)val;
    (void)vn;
    if (kn > 12 && prefs_key_is(key, 12, "calibration.")
        && !(kn == 12 + bn && prefs_key_is(key + 12, bn, c->name)))
    {
        out_bytes(c->out, line, ln);
        out_str(c->out, "\n");
    }
}

size_t touch_prefs_format(const struct TouchPrefs *p, const char *name,
                            const char *keep, size_t keep_length,
                            char *buf, size_t size)
{
    struct prefs_out o;

    if (!size)
        return 0;
    o.buf = buf;
    o.size = size;
    o.len = 0;
    o.overflow = 0;

    out_str(&o, "# Touchscreen preferences\n");
    out_kv(&o, "version", TOUCH_PREFS_VERSION);
    if (p->have & TOUCH_PREFS_HAVE_CAL)
    {
        out_str(&o, "calibration.");
        out_str(&o, name);
        out_str(&o, "=");
        out_num(&o, p->cal.x_min);
        out_str(&o, ",");
        out_num(&o, p->cal.x_max);
        out_str(&o, ",");
        out_num(&o, p->cal.y_min);
        out_str(&o, ",");
        out_num(&o, p->cal.y_max);
        out_str(&o, ",");
        if (!p->cal.flags)
            out_str(&o, "-");
        if (p->cal.flags & TOUCH_PREFS_SWAP_XY)
            out_str(&o, "s");
        if (p->cal.flags & TOUCH_PREFS_MIRROR_X)
            out_str(&o, "x");
        if (p->cal.flags & TOUCH_PREFS_MIRROR_Y)
            out_str(&o, "y");
        out_str(&o, "\n");
    }
    if (keep && keep_length)
    {
        struct prefs_keep_ctx c;

        c.out = &o;
        c.name = name;
        prefs_each(keep, keep_length, prefs_keep_fn, &c);
    }
    if (p->have & TOUCH_PREFS_HAVE_MODE)
    {
        out_str(&o, "mode=");
        out_str(&o, p->mode == TOUCH_DIRECT ? "direct" : "tap");
        out_str(&o, "\n");
    }
    if (p->have & TOUCH_PREFS_HAVE_PARAMS)
    {
        out_kv(&o, "hold_ms", p->params.hold_ms);
        out_kv(&o, "slop_px", (uint32_t)p->params.slop_px);
        out_kv(&o, "release_polls", p->params.release_polls);
        out_kv(&o, "tapdrag_ms", p->params.tapdrag_ms);
        out_kv(&o, "tapdrag_px", (uint32_t)p->params.tapdrag_px);
        out_kv(&o, "two_finger_right", p->params.two_finger_right);
    }
    if (o.overflow)
    {
        buf[0] = 0;
        return 0;
    }
    buf[o.len] = 0;
    return o.len;
}
