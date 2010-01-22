#define DEBUG
#include <assert.h>
#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include "board.h"
#include "debug.h"
#include "pattern.h"
#include "patternsp.h"
#include "tactics.h"


/* Mapping from point sequence to coordinate offsets (to determine
 * coordinates relative to pattern center). The array is ordered
 * in the gridcular metric order so that we can go through it
 * and incrementally match spatial features in nested circles.
 * Within one circle, coordinates are ordered by rows to keep
 * good cache behavior. */
struct ptcoord ptcoords[MAX_PATTERN_AREA];

/* For each radius, starting index in ptcoords[]. */
int ptind[MAX_PATTERN_DIST + 2];

/* ptcoords[], ptind[] setup */
static void __attribute__((constructor))
ptcoords_init(void)
{
	int i = 0; /* Indexing ptcoords[] */

	/* First, center point. */
	ptind[0] = ptind[1] = 0;
	ptcoords[i].x = ptcoords[i].y = 0; i++;

	for (int d = 2; d <= MAX_PATTERN_DIST; d++) {
		ptind[d] = i;
		/* For each y, examine all integer solutions
		 * of d = |x| + |y| + max(|x|, |y|). */
		/* TODO: (Stern, 2006) uses a hand-modified
		 * circles that are finer for small d. */
		for (short y = d / 2; y >= 0; y--) {
			short x;
			if (y > d / 3) {
				/* max(|x|, |y|) = |y|, non-zero x */
				x = d - y * 2;
				if (x + y * 2 != d) continue;
			} else {
				/* max(|x|, |y|) = |x| */
				/* Or, max(|x|, |y|) = |y| and x is zero */
				x = (d - y) / 2;
				if (x * 2 + y != d) continue;
			}

			assert((x > y ? x : y) + x + y == d);

			ptcoords[i].x = x; ptcoords[i].y = y; i++;
			if (x != 0) { ptcoords[i].x = -x; ptcoords[i].y = y; i++; }
			if (y != 0) { ptcoords[i].x = x; ptcoords[i].y = -y; i++; }
			if (x != 0 && y != 0) { ptcoords[i].x = -x; ptcoords[i].y = -y; i++; }
		}
	}
	ptind[MAX_PATTERN_DIST + 1] = i;

#if 0
	for (int d = 0; d <= MAX_PATTERN_DIST; d++) {
		fprintf(stderr, "d=%d (%d) ", d, ptind[d]);
		for (int j = ptind[d]; j < ptind[d + 1]; j++) {
			fprintf(stderr, "%d,%d ", ptcoords[j].x, ptcoords[j].y);
		}
		fprintf(stderr, "\n");
	}
#endif
}


/* Zobrist hashes used for points in patterns. */
hash_t pthashes[PTH__ROTATIONS][MAX_PATTERN_AREA][S_MAX];

static void __attribute__((constructor))
pthashes_init(void)
{
	/* We need fixed hashes for all pattern-relative in
	 * all pattern users! This is a simple way to generate
	 * hopefully good ones. Park-Miller powa. :) */

	/* We create a virtual board (centered at the sequence start),
	 * plant the hashes there, then pick them up into the sequence
	 * with correct coordinates. It would be possible to generate
	 * the sequence point hashes directly, but the rotations would
	 * make for enormous headaches. */
	hash_t pthboard[MAX_PATTERN_AREA][4];
	int pthbc = MAX_PATTERN_AREA / 2; // tengen coord

	/* The magic numbers are tuned for minimal collisions. */
	hash_t h = 0x313131;
	for (int i = 0; i < MAX_PATTERN_AREA; i++) {
		pthboard[i][S_NONE] = (h = h * 16803 - 7);
		pthboard[i][S_BLACK] = (h = h * 16805 + 7);
		pthboard[i][S_WHITE] = (h = h * 16807 + 3);
		pthboard[i][S_OFFBOARD] = (h = h * 16809 - 3);
	}

	/* Virtual board with hashes created, now fill
	 * pthashes[] with hashes for points in actual
	 * sequences, also considering various rotations. */
#define PTH_VMIRROR	1
#define PTH_HMIRROR	2
#define PTH_90ROT	4
	for (int r = 0; r < PTH__ROTATIONS; r++) {
		for (int i = 0; i < MAX_PATTERN_AREA; i++) {
			/* Rotate appropriately. */
			int rx = ptcoords[i].x;
			int ry = ptcoords[i].y;
			if (r & PTH_VMIRROR) ry = -ry;
			if (r & PTH_HMIRROR) rx = -rx;
			if (r & PTH_90ROT) {
				int rs = rx; rx = -ry; ry = rs;
			}
			int bi = pthbc + ry * MAX_PATTERN_DIST + rx;

			/* Copy info. */
			pthashes[r][i][S_NONE] = pthboard[bi][S_NONE];
			pthashes[r][i][S_BLACK] = pthboard[bi][S_BLACK];
			pthashes[r][i][S_WHITE] = pthboard[bi][S_WHITE];
			pthashes[r][i][S_OFFBOARD] = pthboard[bi][S_OFFBOARD];
		}
	}
}

inline hash_t
spatial_hash(int rotation, struct spatial *s)
{
	hash_t h = 0;
	for (int i = 0; i < ptind[s->dist + 1]; i++) {
		h ^= pthashes[rotation][i][spatial_point_at(*s, i)];
	}
	return h & spatial_hash_mask;
}

char *
spatial2str(struct spatial *s)
{
	static char buf[1024];
	for (int i = 0; i < ptind[s->dist + 1]; i++) {
		buf[i] = stone2char(spatial_point_at(*s, i));
	}
	buf[ptind[s->dist + 1]] = 0;
	return buf;
}

void
spatial_from_board(struct pattern_config *pc, struct spatial *s,
                   struct board *b, struct move *m)
{
	assert(pc->spat_min > 0);

	/* We record all spatial patterns black-to-play; simply
	 * reverse all colors if we are white-to-play. */
	static enum stone bt_black[4] = { S_NONE, S_BLACK, S_WHITE, S_OFFBOARD };
	static enum stone bt_white[4] = { S_NONE, S_WHITE, S_BLACK, S_OFFBOARD };
	enum stone (*bt)[4] = m->color == S_WHITE ? &bt_white : &bt_black;

	memset(s, 0, sizeof(*s));
	for (int j = 0; j < ptind[pc->spat_max + 1]; j++) {
		ptcoords_at(x, y, m->coord, b, j);
		s->points[j / 4] |= (*bt)[board_atxy(b, x, y)] << ((j % 4) * 2);
	}
	s->dist = pc->spat_max;
}


/* Spatial dict manipulation. */

static int
spatial_dict_addc(struct spatial_dict *dict, struct spatial *s)
{
	/* Allocate space in 1024 blocks. */
#define SPATIALS_ALLOC 1024
	if (!(dict->nspatials % SPATIALS_ALLOC)) {
		dict->spatials = realloc(dict->spatials,
				(dict->nspatials + SPATIALS_ALLOC)
				* sizeof(*dict->spatials));
	}
	dict->spatials[dict->nspatials] = *s;
	return dict->nspatials++;
}

static bool
spatial_dict_addh(struct spatial_dict *dict, hash_t hash, int id)
{
	if (dict->hash[hash]) {
		dict->collisions++;
		/* Give up, not worth the trouble. */
		return false;
	}
	dict->hash[hash] = id;
	return true;
}

/* Spatial dictionary file format:
 * /^#/ - comments
 * INDEX RADIUS STONES HASH...
 * INDEX: index in the spatial table
 * RADIUS: @d of the pattern
 * STONES: string of ".XO#" chars
 * HASH...: space-separated 18bit hash-table indices for the pattern */

static void
spatial_dict_read(struct spatial_dict *dict, char *buf)
{
	/* XXX: We trust the data. Bad data will crash us. */
	char *bufp = buf;

	int index, radius;
	index = strtol(bufp, &bufp, 10);
	radius = strtol(bufp, &bufp, 10);
	while (isspace(*bufp)) bufp++;

	/* Load the stone configuration. */
	struct spatial s = { .dist = radius };
	int sl = 0;
	while (!isspace(*bufp)) {
		s.points[sl / 4] |= char2stone(*bufp++) << (sl % 4);
		sl++;
	}
	while (isspace(*bufp)) bufp++;

	/* Sanity check. */
	if (sl != ptind[s.dist + 1]) {
		fprintf(stderr, "Spatial dictionary: Invalid number of stones (%d != %d) on this line: %s\n",
			sl, ptind[radius + 1] - 1, buf);
		exit(EXIT_FAILURE);
	}

	/* Add to collection. */
	int id = spatial_dict_addc(dict, &s);

	/* Add to specified hash places. */
	while (*bufp) {
		int hash = strtol(bufp, &bufp, 16);
		while (isspace(*bufp)) bufp++;
		spatial_dict_addh(dict, hash & spatial_hash_mask, id);
	}
}

void
spatial_write(struct spatial *s, int id, FILE *f)
{
	fprintf(f, "%d %d ", id, s->dist);
	fputs(spatial2str(s), f);
	for (int r = 0; r < PTH__ROTATIONS; r++)
		fprintf(f, " %"PRIhash"", spatial_hash(r, s));
	fputc('\n', f);
}

static void
spatial_dict_load(struct spatial_dict *dict, FILE *f)
{
	char buf[1024];
	while (fgets(buf, sizeof(buf), f)) {
		if (buf[0] == '#') continue;
		spatial_dict_read(dict, buf);
	}
}

void
spatial_dict_writeinfo(struct spatial_dict *dict, FILE *f)
{
	/* New file. First, create a comment describing order
	 * of points in the array. This is just for purposes
	 * of external tools, Pachi never interprets it itself. */
	fprintf(f, "# Pachi spatial patterns dictionary v1.0 maxdist %d\n",
		MAX_PATTERN_DIST);
	for (int d = 0; d <= MAX_PATTERN_DIST; d++) {
		fprintf(f, "# Point order: d=%d ", d);
		for (int j = ptind[d]; j < ptind[d + 1]; j++) {
			fprintf(f, "%d,%d ", ptcoords[j].x, ptcoords[j].y);
		}
		fprintf(f, "\n");
	}
}

const char *spatial_dict_filename = "patterns.spat";
struct spatial_dict *
spatial_dict_init(bool will_append)
{
	FILE *f = fopen(spatial_dict_filename, "r");
	if (!f && !will_append) {
		if (DEBUGL(1))
			fprintf(stderr, "No spatial dictionary, will not match spatial pattern features.\n");
		return NULL;
	}

	struct spatial_dict *dict = calloc(1, sizeof(*dict));
	/* We create a dummy record for index 0 that we will
	 * never reference. This is so that hash value 0 can
	 * represent "no value". */
	struct spatial dummy = { .dist = 0 };
	spatial_dict_addc(dict, &dummy);

	if (f) {
		spatial_dict_load(dict, f);
		fclose(f); f = NULL;
	} else {
		assert(will_append);
	}

	return dict;
}

int
spatial_dict_put(struct spatial_dict *dict, struct spatial *s, hash_t h)
{
	int id = spatial_dict_get(dict, s->dist, h);
	if (id > 0) {
		/* Check for collisions in append mode. */
		/* Tough job, we simply try if any other rotation
		 * is also covered by the existing record. */
		int r; hash_t rhash; int rid;
		for (r = 1; r < PTH__ROTATIONS; r++) {
			rhash = spatial_hash(r, s);
			rid = dict->hash[rhash];
			if (rid != id)
				goto collision;
		}
		/* All rotations match, id is good to go! */
		return id;

collision:
		if (DEBUGL(1))
			fprintf(stderr, "Collision %d vs %d (hash %d:%"PRIhash")\n",
				id, dict->nspatials, r, h);
		id = 0;
		/* dict->collisions++; gets done by addh */
	}

	/* Add new pattern! */
	id = spatial_dict_addc(dict, s);
	for (int r = 0; r < PTH__ROTATIONS; r++)
		spatial_dict_addh(dict, spatial_hash(r, s), id);
	return id;
}


/** Pattern3 helpers */

/* XXX: We have hard-coded this point order:
 * # Point order: d=1 0,0
 * # Point order: d=2 0,1 0,-1 1,0 -1,0
 * # Point order: d=3 1,1 -1,1 1,-1 -1,-1
 */
/* p3bits describe location of given point in the
 * pattern3 hash word. */
static const int p3bits[] = { -1,  1, 6, 3, 4,  0, 2, 5, 7 };


static hash_t
pattern3_to_spatial(int pat3)
{
	hash_t h = pthashes[0][0][S_NONE];
	for (int i = 1; i < 9; i++)
		h ^= pthashes[0][i][(pat3 >> (p3bits[i] * 2)) & 0x3];
	return h;
}

static int
spatial_to_pattern3(struct spatial *s)
{
	assert(s->dist == 3);
	int pat3 = 0;
	for (int i = 1; i < 9; i++)
		pat3 |= spatial_point_at(*s, i) << (p3bits[i] * 2);
	return pat3;
}

int
pattern3_by_spatial(struct spatial_dict *dict, int pat3)
{
	/* Just pull pat3 through the spatial database to generate
	 * hash of its canonical form. */
	int s = spatial_dict_get(dict, 3, pattern3_to_spatial(pat3));
	return spatial_to_pattern3(&dict->spatials[s]);
}
