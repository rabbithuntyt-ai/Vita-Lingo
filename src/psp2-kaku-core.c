/* Kaku for Vita: stroke matching (port of Kaku's matcher) and review scheduling. */
#include "psp2-kaku.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define K (109.0f / 1024.0f) /* hanzi-writer thresholds were tuned in a 1024 box */
static const float T_AVGDIST = 350 * K;
static const float T_STARTEND = 250 * K;
static const float T_FRECHET = 0.4f;
static const float T_MINLEN = 0.35f;
static const float T_LENPAD = 25 * K;
static const float T_COSSIM = 0.0f;
static const float T_SHORT = 13.0f;
static const float ROT[5] = { (float) M_PI / 16, (float) M_PI / 32, 0, -(float) M_PI / 32, -(float) M_PI / 16 };

typedef struct KakuPt P;

struct Poly {
	P* p;
	int n;
};

struct Shape {
	struct Poly pts, med, vecs;
	float len;
	P start, end;
	struct Poly rot[5]; /* normalized, rotated reference curves, built on demand */
	bool rotReady;
};

struct Stroke {
	struct Poly dense;
	struct Shape main;
	struct Shape alts[3];
	int nAlts;
	bool closed;
};

struct KakuChar {
	struct Stroke* strokes;
	int n;
};

static float dist(P a, P b) {
	return hypotf(a.x - b.x, a.y - b.y);
}

static P sub(P a, P b) {
	return (P) { a.x - b.x, a.y - b.y };
}

static float polyLen(const P* p, int n) {
	float L = 0;
	for (int i = 1; i < n; ++i) {
		L += dist(p[i - 1], p[i]);
	}
	return L;
}

static struct Poly polyNew(int n) {
	struct Poly r = { calloc(n > 0 ? n : 1, sizeof(P)), n };
	return r;
}

static void polyFree(struct Poly* p) {
	free(p->p);
	p->p = NULL;
	p->n = 0;
}

static struct Poly resample(const P* pts, int n, int count) {
	struct Poly out = polyNew(count);
	float L = polyLen(pts, n);
	if (n < 2 || L < 1e-6f) {
		for (int i = 0; i < count; ++i) {
			out.p[i] = pts[0];
		}
		return out;
	}
	float step = L / (count - 1);
	int k = 0;
	out.p[k++] = pts[0];
	P prev = pts[0];
	float acc = 0;
	int i = 1;
	while (i < n && k < count) {
		P cur = pts[i];
		float seg = dist(prev, cur);
		if (seg > 0 && acc + seg >= step) {
			float t = (step - acc) / seg;
			P q = { prev.x + t * (cur.x - prev.x), prev.y + t * (cur.y - prev.y) };
			out.p[k++] = q;
			prev = q;
			acc = 0;
		} else {
			acc += seg;
			prev = cur;
			++i;
		}
	}
	while (k < count) {
		out.p[k++] = pts[n - 1];
	}
	out.p[count - 1] = pts[n - 1];
	return out;
}

static struct Poly resampleBy(const P* pts, int n, float spacing, int mn, int mx) {
	int c = (int) lroundf(polyLen(pts, n) / spacing) + 1;
	if (c < mn) {
		c = mn;
	}
	if (c > mx) {
		c = mx;
	}
	return resample(pts, n, c);
}

static struct Shape makeShape(const P* dense, int n) {
	struct Shape s;
	memset(&s, 0, sizeof(s));
	s.len = polyLen(dense, n);
	s.pts = resampleBy(dense, n, 1.2f, 2, 400);
	s.med = resampleBy(dense, n, 7, 3, 40);
	s.vecs = polyNew(s.med.n - 1);
	for (int i = 1; i < s.med.n; ++i) {
		s.vecs.p[i - 1] = sub(s.med.p[i], s.med.p[i - 1]);
	}
	s.start = s.pts.p[0];
	s.end = s.pts.p[s.pts.n - 1];
	return s;
}

static void freeShape(struct Shape* s) {
	polyFree(&s->pts);
	polyFree(&s->med);
	polyFree(&s->vecs);
	for (int i = 0; i < 5; ++i) {
		polyFree(&s->rot[i]);
	}
}

static struct Stroke buildStroke(const P* src, int count) {
	struct Stroke st;
	memset(&st, 0, sizeof(st));
	st.dense = polyNew(count);
	memcpy(st.dense.p, src, count * sizeof(P));
	const P* dense = st.dense.p;
	int n = count;
	st.main = makeShape(dense, n);
	struct Shape* s = &st.main;
	st.closed = s->len < 60 && dist(s->start, s->end) < 0.2f * s->len;
	/* KanjiVG draws short flick strokes with a curled tail most people write as
	 * a plain dash: give those simpler alternates to match. */
	if (!st.closed && s->len < 45) {
		P a = dense[0], b = dense[n - 1];
		int k = 0;
		for (int j = 1; j < n; ++j) {
			if (dist(dense[j], a) > dist(dense[k], a)) {
				k = j;
			}
		}
		float hook = polyLen(dense + k, n - k) / s->len;
		float chord = dist(a, b);
		if (chord <= 0) {
			chord = 1;
		}
		float dev = 0;
		for (int j = 0; j < n; ++j) {
			P p = dense[j];
			float d = fabsf((b.x - a.x) * (a.y - p.y) - (a.x - p.x) * (b.y - a.y)) / chord;
			if (d > dev) {
				dev = d;
			}
		}
		if (hook >= 0.12f || dev / chord >= 0.25f) {
			P seg[3] = { a, { (a.x + b.x) / 2, (a.y + b.y) / 2 }, b };
			st.alts[st.nAlts++] = makeShape(seg, 3);
			if (hook >= 0.12f) {
				st.alts[st.nAlts++] = makeShape(dense, k + 1);
				P seg2[3] = { a, { (a.x + dense[k].x) / 2, (a.y + dense[k].y) / 2 }, dense[k] };
				st.alts[st.nAlts++] = makeShape(seg2, 3);
			}
		}
	}
	return st;
}

struct KakuChar* kakuCharLoad(int firstStroke, int nStrokes) {
	struct KakuChar* c = calloc(1, sizeof(*c));
	if (!c) {
		return NULL;
	}
	c->n = nStrokes;
	c->strokes = calloc(nStrokes, sizeof(struct Stroke));
	for (int i = 0; i < nStrokes; ++i) {
		const struct KakuStroke* ks = &KAKU_STROKES[firstStroke + i];
		c->strokes[i] = buildStroke((const P*) &KAKU_POINTS[ks->first * 2], ks->count);
	}
	return c;
}

void kakuCharFree(struct KakuChar* c) {
	if (!c) {
		return;
	}
	for (int i = 0; i < c->n; ++i) {
		polyFree(&c->strokes[i].dense);
		freeShape(&c->strokes[i].main);
		for (int a = 0; a < c->strokes[i].nAlts; ++a) {
			freeShape(&c->strokes[i].alts[a]);
		}
	}
	free(c->strokes);
	free(c);
}

int kakuCharStrokes(const struct KakuChar* c) {
	return c->n;
}

const struct KakuPt* kakuCharStroke(const struct KakuChar* c, int i, int* count) {
	*count = c->strokes[i].dense.n;
	return c->strokes[i].dense.p;
}

/* ---- matching ---- */

static float cosSim(P a, P b) {
	float m = hypotf(a.x, a.y) * hypotf(b.x, b.y);
	return m ? (a.x * b.x + a.y * b.y) / m : 0;
}

static float avgDistance(const struct Poly* user, const struct Shape* s) {
	float tot = 0;
	for (int i = 0; i < user->n; ++i) {
		float best = FLT_MAX;
		for (int j = 0; j < s->pts.n; ++j) {
			float dx = user->p[i].x - s->pts.p[j].x, dy = user->p[i].y - s->pts.p[j].y;
			float dd = dx * dx + dy * dy;
			if (dd < best) {
				best = dd;
			}
		}
		tot += sqrtf(best);
	}
	return tot / user->n;
}

static struct Poly normalizeCurve(const P* pts, int n) {
	struct Poly r = resample(pts, n, 30);
	float mx = 0, my = 0;
	for (int i = 0; i < r.n; ++i) {
		mx += r.p[i].x;
		my += r.p[i].y;
	}
	mx /= r.n;
	my /= r.n;
	for (int i = 0; i < r.n; ++i) {
		r.p[i].x -= mx;
		r.p[i].y -= my;
	}
	P a = r.p[0], b = r.p[r.n - 1];
	float scale = sqrtf((a.x * a.x + a.y * a.y + b.x * b.x + b.y * b.y) / 2);
	if (scale < 1e-6f) {
		float t = 0;
		for (int i = 0; i < r.n; ++i) {
			t += r.p[i].x * r.p[i].x + r.p[i].y * r.p[i].y;
		}
		scale = sqrtf(t / r.n);
		if (scale <= 0) {
			scale = 1;
		}
	}
	for (int i = 0; i < r.n; ++i) {
		r.p[i].x /= scale;
		r.p[i].y /= scale;
	}
	/* subdivide so no segment is longer than 0.05 */
	int total = 1;
	for (int i = 1; i < r.n; ++i) {
		int k = (int) ceilf(dist(r.p[i - 1], r.p[i]) / 0.05f);
		total += k > 0 ? k : 0;
	}
	if (total > 2000) {
		total = 2000;
	}
	struct Poly out = polyNew(total);
	int o = 0;
	out.p[o++] = r.p[0];
	for (int i = 1; i < r.n && o < total; ++i) {
		P p = r.p[i - 1], q = r.p[i];
		int k = (int) ceilf(dist(p, q) / 0.05f);
		for (int j = 1; j <= k && o < total; ++j) {
			out.p[o++] = (P) { p.x + (q.x - p.x) * j / k, p.y + (q.y - p.y) * j / k };
		}
	}
	out.n = o;
	polyFree(&r);
	return out;
}

static float frechet(const struct Poly* A, const struct Poly* B) {
	int n = A->n, m = B->n;
	float* prev = malloc(m * sizeof(float));
	float* cur = malloc(m * sizeof(float));
	for (int i = 0; i < n; ++i) {
		for (int j = 0; j < m; ++j) {
			float d = dist(A->p[i], B->p[j]);
			if (i == 0 && j == 0) {
				cur[j] = d;
			} else if (i == 0) {
				cur[j] = fmaxf(cur[j - 1], d);
			} else if (j == 0) {
				cur[j] = fmaxf(prev[0], d);
			} else {
				cur[j] = fmaxf(fminf(fminf(prev[j], prev[j - 1]), cur[j - 1]), d);
			}
		}
		float* t = prev;
		prev = cur;
		cur = t;
	}
	float r = prev[m - 1];
	free(prev);
	free(cur);
	return r;
}

static bool shapeFit(const struct Poly* user, struct Shape* s, float len) {
	struct Poly a = normalizeCurve(user->p, user->n);
	if (!s->rotReady) {
		struct Poly b = normalizeCurve(s->pts.p, s->pts.n);
		for (int r = 0; r < 5; ++r) {
			float cs = cosf(ROT[r]), sn = sinf(ROT[r]);
			s->rot[r] = polyNew(b.n);
			for (int i = 0; i < b.n; ++i) {
				s->rot[r].p[i] = (P) { b.p[i].x * cs - b.p[i].y * sn, b.p[i].x * sn + b.p[i].y * cs };
			}
		}
		polyFree(&b);
		s->rotReady = true;
	}
	float best = FLT_MAX;
	for (int r = 0; r < 5; ++r) {
		float f = frechet(&a, &s->rot[r]);
		if (f < best) {
			best = f;
		}
	}
	polyFree(&a);
	float lim = T_FRECHET * len * (s->len < T_SHORT ? 1.8f : 1.0f);
	return best <= lim;
}

static bool directionOk(const struct Poly* user, const struct Shape* s) {
	struct Poly med = resampleBy(user->p, user->n, 5, 3, 60);
	float tot = 0;
	int n = 0;
	for (int i = 1; i < med.n; ++i) {
		P v = sub(med.p[i], med.p[i - 1]);
		float best = -1;
		for (int j = 0; j < s->vecs.n; ++j) {
			float c = cosSim(v, s->vecs.p[j]);
			if (c > best) {
				best = c;
			}
		}
		tot += best;
		++n;
	}
	polyFree(&med);
	return n ? tot / n > T_COSSIM : false;
}

static P centroid(const P* p, int n) {
	P c = { 0, 0 };
	for (int i = 0; i < n; ++i) {
		c.x += p[i].x;
		c.y += p[i].y;
	}
	c.x /= n;
	c.y /= n;
	return c;
}

static float rms(const P* p, int n, P c) {
	float t = 0;
	for (int i = 0; i < n; ++i) {
		t += (p[i].x - c.x) * (p[i].x - c.x) + (p[i].y - c.y) * (p[i].y - c.y);
	}
	return sqrtf(t / n);
}

static float turning(const P* p, int n) {
	struct Poly r = resampleBy(p, n, 2.5f, 4, 120);
	float tot = 0;
	for (int i = 2; i < r.n; ++i) {
		P a = sub(r.p[i - 1], r.p[i - 2]), b = sub(r.p[i], r.p[i - 1]);
		tot += atan2f(a.x * b.y - a.y * b.x, a.x * b.x + a.y * b.y);
	}
	polyFree(&r);
	return fabsf(tot);
}

struct Opts {
	float leniency;
	bool outline;
	int index;
	bool checkBackwards;
};

struct M {
	bool isMatch;
	float avg;
	bool backwards;
};

static struct M matchLoop(const struct Poly* user, struct Stroke* st, struct Opts o) {
	float len = o.leniency;
	struct M m = { false, avgDistance(user, &st->main), false };
	P uc = centroid(user->p, user->n), tc = centroid(st->main.pts.p, st->main.pts.n);
	bool near = dist(uc, tc) <= (o.outline || o.index > 0 ? 12 : 20) * len;
	float ratio = rms(user->p, user->n, uc) / rms(st->main.pts.p, st->main.pts.n, tc);
	m.isMatch = near && ratio > 0.45f / len && ratio < 2.2f * len && turning(user->p, user->n) > 4.2f / fminf(len, 1.3f);
	return m;
}

static struct M matchOne(const struct Poly* user, struct Shape* s, struct Opts o) {
	float len = o.leniency;
	struct M m = { false, avgDistance(user, s), false };
	float distMod = o.outline || o.index > 0 ? 0.5f : 1.0f;
	if (m.avg <= T_AVGDIST * distMod * len) {
		float d0 = dist(s->start, user->p[0]), d1 = dist(s->end, user->p[user->n - 1]);
		bool ends = d0 <= T_STARTEND * len && d1 <= T_STARTEND * len;
		bool lengthOk = (len * (polyLen(user->p, user->n) + T_LENPAD)) / (s->len + T_LENPAD) >= T_MINLEN;
		m.isMatch = ends && lengthOk && directionOk(user, s) && shapeFit(user, s, len);
	}
	return m;
}

static struct M matchData(const struct Poly* user, struct Stroke* st, struct Opts o) {
	if (st->closed) {
		return matchLoop(user, st, o);
	}
	struct M m = matchOne(user, &st->main, o);
	if (!m.isMatch) {
		for (int a = 0; a < st->nAlts; ++a) {
			struct M r = matchOne(user, &st->alts[a], o);
			if (r.isMatch) {
				m.isMatch = true;
				m.avg = fminf(m.avg, r.avg);
				break;
			}
		}
	}
	if (!m.isMatch && o.checkBackwards) {
		struct Poly rev = polyNew(user->n);
		for (int i = 0; i < user->n; ++i) {
			rev.p[i] = user->p[user->n - 1 - i];
		}
		struct Opts o2 = o;
		o2.checkBackwards = false;
		m.backwards = matchData(&rev, st, o2).isMatch;
		polyFree(&rev);
	}
	return m;
}

struct KakuResult kakuCheck(struct KakuChar* c, const struct KakuPt* raw, int n, int idx, float leniency, bool outline) {
	struct KakuResult res = { false, false, false, -1 };
	/* strip duplicate points */
	struct Poly base = polyNew(n);
	int k = 0;
	for (int i = 0; i < n; ++i) {
		if (!k || dist(base.p[k - 1], raw[i]) > 0.01f) {
			base.p[k++] = raw[i];
		}
	}
	base.n = k;
	if (base.n < 2 || polyLen(base.p, base.n) < 1.5f || idx < 0 || idx >= c->n) {
		polyFree(&base);
		res.tap = true;
		return res;
	}
	struct Poly pts = resampleBy(base.p, base.n, 1.5f, 2, 300);
	polyFree(&base);
	struct Opts o = { leniency > 0 ? leniency : 1, outline, idx, true };
	struct M m = matchData(&pts, &c->strokes[idx], o);
	if (!m.isMatch) {
		int wrong = -1;
		for (int j = idx + 1; j < c->n; ++j) {
			struct Opts oj = o;
			oj.index = j;
			oj.checkBackwards = false;
			if (matchData(&pts, &c->strokes[j], oj).isMatch) {
				wrong = j;
				break;
			}
		}
		res.backwards = m.backwards && wrong < 0;
		res.wrongOrder = wrong;
		polyFree(&pts);
		return res;
	}
	/* a later stroke fitting better usually means the wrong stroke was drawn */
	float closest = m.avg;
	int closestIdx = -1;
	for (int j = idx + 1; j < c->n; ++j) {
		struct Opts oj = o;
		oj.index = j;
		oj.checkBackwards = false;
		struct M r = matchData(&pts, &c->strokes[j], oj);
		if (r.isMatch && r.avg < closest) {
			closest = r.avg;
			closestIdx = j;
		}
	}
	if (closestIdx >= 0) {
		float adj = (0.6f * (closest + m.avg)) / (2 * m.avg);
		struct Opts oa = o;
		oa.leniency = o.leniency * adj;
		oa.checkBackwards = false;
		if (!matchData(&pts, &c->strokes[idx], oa).isMatch) {
			res.wrongOrder = closestIdx;
			polyFree(&pts);
			return res;
		}
	}
	polyFree(&pts);
	res.isMatch = true;
	return res;
}

/* ---- Scheduling: SM-2 style, Good keeps ease ---- */

void kakuCardInit(struct KakuCard* c, uint32_t today) {
	memset(c, 0, sizeof(*c));
	c->due = today;
	c->ease = 250;
}

static void next(const struct KakuCard* c, enum KakuGrade g, int* interval, int* ease) {
	int e = c->ease ? c->ease : 250;
	int iv = c->interval;
	switch (g) {
	case KAKU_AGAIN:
		e -= 20;
		iv = 0;
		break;
	case KAKU_HARD:
		e -= 15;
		iv = iv ? (iv * 12 + 9) / 10 : 1;
		break;
	case KAKU_GOOD:
		iv = iv ? (iv * e + 99) / 100 : 1;
		if (c->reps == 1 && iv < 3) {
			iv = 3;
		}
		break;
	case KAKU_EASY:
		e += 15;
		iv = iv ? (iv * e * 13 + 999) / 1000 : 4;
		break;
	}
	if (e < 130) {
		e = 130;
	}
	if (iv > 3650) {
		iv = 3650;
	}
	*interval = iv;
	*ease = e;
}

void kakuCardGrade(struct KakuCard* c, enum KakuGrade g, uint32_t today) {
	int iv, e;
	next(c, g, &iv, &e);
	c->interval = iv;
	c->ease = e;
	c->due = today + iv;
	if (g == KAKU_AGAIN) {
		c->lapses++;
		c->reps = 0;
	} else {
		c->reps++;
	}
}

int kakuCardPreview(const struct KakuCard* c, enum KakuGrade g) {
	int iv, e;
	next(c, g, &iv, &e);
	return iv;
}
