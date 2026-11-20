/*
 * json.h - minimal JSON DOM parser and pretty-printer.
 *
 * Copyright (c) 2026 Daniel. MIT License, see LICENSE.
 *
 * Small, dependency-free and good enough for config files and the
 * Open-Meteo response. Numbers keep their original text so a config
 * file round-trips without float formatting noise.
 */
#ifndef T410DASH_JSON_H
#define T410DASH_JSON_H

#include <stddef.h>
#include <stdio.h>

typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } jtype;

typedef struct jval {
	jtype t;
	char *key;           /* member name when inside an object */
	char *s;             /* string value, or raw text of a number */
	double n;            /* numeric value (J_NUM) */
	int b;               /* boolean value (J_BOOL) */
	struct jval *child;  /* first element/member (J_ARR, J_OBJ) */
	struct jval *next;   /* next sibling */
} jval;

/* Parse len bytes. Returns NULL on syntax error or allocation failure. */
jval *json_parse(const char *src, size_t len);
void json_free(jval *v);

/* Object member lookup; NULL if v is not an object or key is missing. */
jval *json_get(const jval *obj, const char *key);
/* Dotted path lookup, e.g. "bank.balance_eur". */
jval *json_path(const jval *root, const char *path);
/* n-th array element, NULL if out of range. */
jval *json_at(const jval *arr, int idx);

/* Replace (or append) a string member of an object. Returns 0 on success. */
int json_set_str(jval *obj, const char *key, const char *value);

/* Pretty-print with two-space indentation and a trailing newline. */
int json_write(FILE *f, const jval *v);

#endif
