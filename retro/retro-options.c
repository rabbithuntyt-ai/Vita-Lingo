/* Lingo libretro front end: core options (see retro-options.h). */
#include "retro-options.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void roInit(struct RetroOptions* o, const char* savePath) {
	memset(o, 0, sizeof(*o));
	snprintf(o->path, sizeof(o->path), "%s", savePath ? savePath : "");
}

static void freeOpt(struct RetroOption* x) {
	for (int i = 0; i < x->nValues; ++i) {
		if (x->labels[i] != x->values[i]) {
			free(x->labels[i]);
		}
		free(x->values[i]);
	}
	x->nValues = 0;
}

void roFree(struct RetroOptions* o) {
	for (int i = 0; i < o->n; ++i) {
		freeOpt(&o->opt[i]);
	}
	o->n = 0;
}

static char* dup(const char* s) {
	size_t n = strlen(s);
	char* d = malloc(n + 1);
	if (d) {
		memcpy(d, s, n + 1);
	}
	return d;
}

static struct RetroOption* slot(struct RetroOptions* o, const char* key) {
	for (int i = 0; i < o->n; ++i) {
		if (!strcmp(o->opt[i].key, key)) {
			freeOpt(&o->opt[i]);
			return &o->opt[i];
		}
	}
	if (o->n >= RO_MAX) {
		return NULL;
	}
	struct RetroOption* x = &o->opt[o->n++];
	memset(x, 0, sizeof(*x));
	snprintf(x->key, sizeof(x->key), "%s", key);
	return x;
}

static void addValue(struct RetroOption* x, const char* value, const char* label) {
	if (x->nValues >= RO_MAX_VALUES || !value) {
		return;
	}
	x->values[x->nValues] = dup(value);
	x->labels[x->nValues] = label && *label ? dup(label) : x->values[x->nValues];
	x->nValues++;
}

static void finish(struct RetroOption* x, const char* def) {
	x->def = 0;
	for (int i = 0; def && i < x->nValues; ++i) {
		if (!strcmp(x->values[i], def)) {
			x->def = i;
		}
	}
	x->current = x->def;
	x->visible = true;
}

void roSetVariables(struct RetroOptions* o, const struct retro_variable* vars) {
	roFree(o);
	for (; vars && vars->key; ++vars) {
		/* "Description; first|second|third", the first is the default */
		struct RetroOption* x = slot(o, vars->key);
		if (!x || !vars->value) {
			continue;
		}
		const char* semi = strchr(vars->value, ';');
		size_t dl = semi ? (size_t) (semi - vars->value) : strlen(vars->value);
		if (dl >= sizeof(x->desc)) {
			dl = sizeof(x->desc) - 1;
		}
		memcpy(x->desc, vars->value, dl);
		x->desc[dl] = 0;
		const char* p = semi ? semi + 1 : "";
		while (*p == ' ') {
			++p;
		}
		while (*p) {
			const char* bar = strchr(p, '|');
			size_t l = bar ? (size_t) (bar - p) : strlen(p);
			char v[128];
			if (l >= sizeof(v)) {
				l = sizeof(v) - 1;
			}
			memcpy(v, p, l);
			v[l] = 0;
			addValue(x, v, NULL);
			p += l + (bar ? 1 : 0);
		}
		finish(x, x->nValues ? x->values[0] : NULL);
	}
	roLoad(o);
}

void roSetDefinitions(struct RetroOptions* o, const struct retro_core_option_definition* defs) {
	roFree(o);
	for (; defs && defs->key; ++defs) {
		struct RetroOption* x = slot(o, defs->key);
		if (!x) {
			continue;
		}
		snprintf(x->desc, sizeof(x->desc), "%s", defs->desc ? defs->desc : defs->key);
		snprintf(x->info, sizeof(x->info), "%s", defs->info ? defs->info : "");
		for (int i = 0; i < RETRO_NUM_CORE_OPTION_VALUES_MAX && defs->values[i].value; ++i) {
			addValue(x, defs->values[i].value, defs->values[i].label);
		}
		finish(x, defs->default_value);
	}
	roLoad(o);
}

void roSetV2(struct RetroOptions* o, const struct retro_core_options_v2* v2) {
	roFree(o);
	if (!v2) {
		return;
	}
	for (const struct retro_core_option_v2_definition* d = v2->definitions; d && d->key; ++d) {
		struct RetroOption* x = slot(o, d->key);
		if (!x) {
			continue;
		}
		snprintf(x->desc, sizeof(x->desc), "%s", d->desc ? d->desc : d->key);
		snprintf(x->info, sizeof(x->info), "%s", d->info ? d->info : "");
		for (int i = 0; i < RETRO_NUM_CORE_OPTION_VALUES_MAX && d->values[i].value; ++i) {
			addValue(x, d->values[i].value, d->values[i].label);
		}
		finish(x, d->default_value);
	}
	roLoad(o);
}

void roSetVisible(struct RetroOptions* o, const char* key, bool visible) {
	for (int i = 0; i < o->n; ++i) {
		if (!strcmp(o->opt[i].key, key)) {
			o->opt[i].visible = visible;
		}
	}
}

const char* roGet(const struct RetroOptions* o, const char* key) {
	for (int i = 0; i < o->n; ++i) {
		const struct RetroOption* x = &o->opt[i];
		if (!strcmp(x->key, key) && x->nValues) {
			return x->values[x->current];
		}
	}
	return NULL;
}

void roStep(struct RetroOptions* o, int index, int dir) {
	if (index < 0 || index >= o->n || !o->opt[index].nValues) {
		return;
	}
	struct RetroOption* x = &o->opt[index];
	x->current = (x->current + dir + x->nValues) % x->nValues;
	o->changed = true;
	roSave(o);
}

void roReset(struct RetroOptions* o) {
	for (int i = 0; i < o->n; ++i) {
		o->opt[i].current = o->opt[i].def;
	}
	o->changed = true;
	roSave(o);
}

bool roLoad(struct RetroOptions* o) {
	if (!o->path[0]) {
		return false;
	}
	FILE* f = fopen(o->path, "r");
	if (!f) {
		return false;
	}
	char line[512];
	while (fgets(line, sizeof(line), f)) {
		char* eq = strstr(line, " = ");
		if (!eq) {
			continue;
		}
		*eq = 0;
		char* v = eq + 3;
		size_t l = strlen(v);
		while (l && (v[l - 1] == '\n' || v[l - 1] == '\r')) {
			v[--l] = 0;
		}
		if (l >= 2 && v[0] == '"' && v[l - 1] == '"') {
			v[l - 1] = 0;
			++v;
		}
		for (int i = 0; i < o->n; ++i) {
			struct RetroOption* x = &o->opt[i];
			if (strcmp(x->key, line)) {
				continue;
			}
			for (int k = 0; k < x->nValues; ++k) {
				if (!strcmp(x->values[k], v)) {
					x->current = k;
				}
			}
		}
	}
	fclose(f);
	return true;
}

bool roSave(const struct RetroOptions* o) {
	if (!o->path[0]) {
		return false;
	}
	FILE* f = fopen(o->path, "w");
	if (!f) {
		return false;
	}
	/* RetroArch's .opt format, so the files are familiar */
	for (int i = 0; i < o->n; ++i) {
		const struct RetroOption* x = &o->opt[i];
		if (x->nValues && x->current != x->def) {
			fprintf(f, "%s = \"%s\"\n", x->key, x->values[x->current]);
		}
	}
	fclose(f);
	return true;
}
