/*
 * json.c - minimal JSON DOM parser and pretty-printer.
 *
 * Copyright (c) 2026 Daniel. MIT License, see LICENSE.
 */
#include "json.h"

#include <stdlib.h>
#include <string.h>

#define MAX_DEPTH 32

typedef struct {
	const char *p, *end;
	int depth;
} parser;

static jval *parse_value(parser *ps);

static void skip_ws(parser *ps)
{
	while (ps->p < ps->end &&
	       (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r'))
		ps->p++;
}

static jval *new_val(jtype t)
{
	jval *v = calloc(1, sizeof(*v));
	if (v)
		v->t = t;
	return v;
}

static int hexval(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

static int read_hex4(parser *ps, unsigned *out)
{
	unsigned v = 0;
	if (ps->end - ps->p < 4)
		return -1;
	for (int i = 0; i < 4; i++) {
		int h = hexval(ps->p[i]);
		if (h < 0)
			return -1;
		v = (v << 4) | (unsigned)h;
	}
	ps->p += 4;
	*out = v;
	return 0;
}

static size_t put_utf8(char *dst, unsigned cp)
{
	if (cp < 0x80) {
		dst[0] = (char)cp;
		return 1;
	} else if (cp < 0x800) {
		dst[0] = (char)(0xC0 | (cp >> 6));
		dst[1] = (char)(0x80 | (cp & 0x3F));
		return 2;
	} else if (cp < 0x10000) {
		dst[0] = (char)(0xE0 | (cp >> 12));
		dst[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
		dst[2] = (char)(0x80 | (cp & 0x3F));
		return 3;
	}
	dst[0] = (char)(0xF0 | (cp >> 18));
	dst[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
	dst[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
	dst[3] = (char)(0x80 | (cp & 0x3F));
	return 4;
}

/* Parses a string literal; ps->p points at the opening quote. */
static char *parse_string_raw(parser *ps)
{
	ps->p++; /* opening quote */
	const char *start = ps->p;
	/* Escapes only ever shrink, so the raw length is an upper bound. */
	const char *q = start;
	while (q < ps->end && *q != '"') {
		if (*q == '\\')
			q++;
		q++;
	}
	if (q >= ps->end)
		return NULL;

	char *out = malloc((size_t)(q - start) + 1);
	if (!out)
		return NULL;
	size_t n = 0;
	while (ps->p < ps->end && *ps->p != '"') {
		char c = *ps->p++;
		if ((unsigned char)c < 0x20)
			goto fail;
		if (c != '\\') {
			out[n++] = c;
			continue;
		}
		if (ps->p >= ps->end)
			goto fail;
		c = *ps->p++;
		switch (c) {
		case '"': out[n++] = '"'; break;
		case '\\': out[n++] = '\\'; break;
		case '/': out[n++] = '/'; break;
		case 'b': out[n++] = '\b'; break;
		case 'f': out[n++] = '\f'; break;
		case 'n': out[n++] = '\n'; break;
		case 'r': out[n++] = '\r'; break;
		case 't': out[n++] = '\t'; break;
		case 'u': {
			unsigned cp, lo;
			if (read_hex4(ps, &cp))
				goto fail;
			if (cp >= 0xD800 && cp <= 0xDBFF) {
				if (ps->end - ps->p < 6 || ps->p[0] != '\\' || ps->p[1] != 'u')
					goto fail;
				ps->p += 2;
				if (read_hex4(ps, &lo) || lo < 0xDC00 || lo > 0xDFFF)
					goto fail;
				cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
			}
			n += put_utf8(out + n, cp);
			break;
		}
		default:
			goto fail;
		}
	}
	if (ps->p >= ps->end)
		goto fail;
	ps->p++; /* closing quote */
	out[n] = '\0';
	return out;
fail:
	free(out);
	return NULL;
}

static jval *parse_number(parser *ps)
{
	const char *start = ps->p;
	if (ps->p < ps->end && *ps->p == '-')
		ps->p++;
	while (ps->p < ps->end &&
	       ((*ps->p >= '0' && *ps->p <= '9') || *ps->p == '.' ||
	        *ps->p == 'e' || *ps->p == 'E' || *ps->p == '+' || *ps->p == '-'))
		ps->p++;
	size_t len = (size_t)(ps->p - start);
	if (len == 0 || len > 63)
		return NULL;

	jval *v = new_val(J_NUM);
	if (!v)
		return NULL;
	v->s = malloc(len + 1);
	if (!v->s) {
		free(v);
		return NULL;
	}
	memcpy(v->s, start, len);
	v->s[len] = '\0';
	char *endp;
	v->n = strtod(v->s, &endp);
	if (*endp != '\0') {
		json_free(v);
		return NULL;
	}
	return v;
}

static int match_word(parser *ps, const char *w)
{
	size_t n = strlen(w);
	if ((size_t)(ps->end - ps->p) < n || memcmp(ps->p, w, n) != 0)
		return 0;
	ps->p += n;
	return 1;
}

static jval *parse_container(parser *ps, int is_obj)
{
	jval *v = new_val(is_obj ? J_OBJ : J_ARR);
	jval **tail;
	char close = is_obj ? '}' : ']';

	if (!v)
		return NULL;
	if (++ps->depth > MAX_DEPTH)
		goto fail;
	tail = &v->child;
	ps->p++; /* opening bracket */
	skip_ws(ps);
	if (ps->p < ps->end && *ps->p == close) {
		ps->p++;
		ps->depth--;
		return v;
	}
	for (;;) {
		char *key = NULL;
		skip_ws(ps);
		if (is_obj) {
			if (ps->p >= ps->end || *ps->p != '"')
				goto fail;
			key = parse_string_raw(ps);
			if (!key)
				goto fail;
			skip_ws(ps);
			if (ps->p >= ps->end || *ps->p != ':') {
				free(key);
				goto fail;
			}
			ps->p++;
		}
		jval *item = parse_value(ps);
		if (!item) {
			free(key);
			goto fail;
		}
		item->key = key;
		*tail = item;
		tail = &item->next;
		skip_ws(ps);
		if (ps->p >= ps->end)
			goto fail;
		if (*ps->p == ',') {
			ps->p++;
			continue;
		}
		if (*ps->p == close) {
			ps->p++;
			ps->depth--;
			return v;
		}
		goto fail;
	}
fail:
	json_free(v);
	return NULL;
}

static jval *parse_value(parser *ps)
{
	skip_ws(ps);
	if (ps->p >= ps->end)
		return NULL;
	switch (*ps->p) {
	case '{':
		return parse_container(ps, 1);
	case '[':
		return parse_container(ps, 0);
	case '"': {
		char *s = parse_string_raw(ps);
		jval *v;
		if (!s)
			return NULL;
		v = new_val(J_STR);
		if (!v) {
			free(s);
			return NULL;
		}
		v->s = s;
		return v;
	}
	case 't':
	case 'f': {
		int is_true = *ps->p == 't';
		jval *v;
		if (!match_word(ps, is_true ? "true" : "false"))
			return NULL;
		v = new_val(J_BOOL);
		if (v)
			v->b = is_true;
		return v;
	}
	case 'n':
		if (!match_word(ps, "null"))
			return NULL;
		return new_val(J_NULL);
	default:
		return parse_number(ps);
	}
}

jval *json_parse(const char *src, size_t len)
{
	parser ps = { src, src + len, 0 };
	jval *v = parse_value(&ps);
	if (!v)
		return NULL;
	skip_ws(&ps);
	if (ps.p != ps.end) {
		json_free(v);
		return NULL;
	}
	return v;
}

void json_free(jval *v)
{
	while (v) {
		jval *next = v->next;
		json_free(v->child);
		free(v->key);
		free(v->s);
		free(v);
		v = next;
	}
}

jval *json_get(const jval *obj, const char *key)
{
	if (!obj || obj->t != J_OBJ)
		return NULL;
	for (jval *c = obj->child; c; c = c->next)
		if (c->key && strcmp(c->key, key) == 0)
			return c;
	return NULL;
}

jval *json_path(const jval *root, const char *path)
{
	char part[64];
	const jval *cur = root;
	while (cur && *path) {
		size_t n = strcspn(path, ".");
		if (n >= sizeof(part))
			return NULL;
		memcpy(part, path, n);
		part[n] = '\0';
		cur = json_get(cur, part);
		path += n;
		if (*path == '.')
			path++;
	}
	return (jval *)cur;
}

jval *json_at(const jval *arr, int idx)
{
	if (!arr || arr->t != J_ARR || idx < 0)
		return NULL;
	jval *c = arr->child;
	while (c && idx-- > 0)
		c = c->next;
	return c;
}

int json_set_str(jval *obj, const char *key, const char *value)
{
	char *dup;
	jval *v;

	if (!obj || obj->t != J_OBJ)
		return -1;
	dup = strdup(value);
	if (!dup)
		return -1;
	v = json_get(obj, key);
	if (v) {
		json_free(v->child);
		v->child = NULL;
		free(v->s);
	} else {
		jval **tail = &obj->child;
		v = new_val(J_STR);
		if (!v || !(v->key = strdup(key))) {
			free(v);
			free(dup);
			return -1;
		}
		while (*tail)
			tail = &(*tail)->next;
		*tail = v;
	}
	v->t = J_STR;
	v->s = dup;
	return 0;
}

static void write_string(FILE *f, const char *s)
{
	fputc('"', f);
	for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
		switch (*p) {
		case '"': fputs("\\\"", f); break;
		case '\\': fputs("\\\\", f); break;
		case '\n': fputs("\\n", f); break;
		case '\r': fputs("\\r", f); break;
		case '\t': fputs("\\t", f); break;
		default:
			if (*p < 0x20)
				fprintf(f, "\\u%04x", *p);
			else
				fputc(*p, f);
		}
	}
	fputc('"', f);
}

static void indent(FILE *f, int level)
{
	for (int i = 0; i < level; i++)
		fputs("  ", f);
}

static void write_value(FILE *f, const jval *v, int level)
{
	switch (v->t) {
	case J_NULL: fputs("null", f); break;
	case J_BOOL: fputs(v->b ? "true" : "false", f); break;
	case J_NUM: fputs(v->s, f); break;
	case J_STR: write_string(f, v->s); break;
	case J_ARR:
	case J_OBJ: {
		int obj = v->t == J_OBJ;
		if (!v->child) {
			fputs(obj ? "{}" : "[]", f);
			break;
		}
		fputc(obj ? '{' : '[', f);
		for (const jval *c = v->child; c; c = c->next) {
			fputc('\n', f);
			indent(f, level + 1);
			if (obj) {
				write_string(f, c->key ? c->key : "");
				fputs(": ", f);
			}
			write_value(f, c, level + 1);
			if (c->next)
				fputc(',', f);
		}
		fputc('\n', f);
		indent(f, level);
		fputc(obj ? '}' : ']', f);
		break;
	}
	}
}

int json_write(FILE *f, const jval *v)
{
	write_value(f, v, 0);
	fputc('\n', f);
	return ferror(f) ? -1 : 0;
}
