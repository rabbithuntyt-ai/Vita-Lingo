/* Lingo libretro front end: opening games inside .zip files.
 *
 * Reads the central directory, picks the first file whose extension the
 * console accepts (else the largest file), and inflates it with zlib. */
#include "retro-zip.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <zlib.h>

static uint16_t rd16(const uint8_t* p) {
	return (uint16_t) (p[0] | (p[1] << 8));
}

static uint32_t rd32(const uint8_t* p) {
	return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

static bool extOk(const char* name, const char* exts) {
	const char* dot = strrchr(name, '.');
	if (!dot || !exts) {
		return false;
	}
	/* exts is ".a.b.c" */
	size_t el = strlen(dot);
	for (const char* p = exts; (p = strchr(p, '.')) != NULL; ++p) {
		const char* e = strchr(p + 1, '.');
		size_t l = e ? (size_t) (e - p) : strlen(p);
		if (l == el && !strncasecmp(p, dot, l)) {
			return true;
		}
	}
	return false;
}

bool retroZipIs(const char* path) {
	FILE* f = fopen(path, "rb");
	if (!f) {
		return false;
	}
	uint8_t sig[4] = { 0 };
	bool ok = fread(sig, 1, 4, f) == 4 && rd32(sig) == 0x04034b50;
	fclose(f);
	return ok;
}

bool retroZipExtract(const char* path, const char* exts, uint8_t** outData, size_t* outSize, char* name, size_t nameLen,
                     char* why, size_t whyLen) {
	*outData = NULL;
	*outSize = 0;
	FILE* f = fopen(path, "rb");
	if (!f) {
		snprintf(why, whyLen, "couldn't open the zip");
		return false;
	}
	fseek(f, 0, SEEK_END);
	long fileSize = ftell(f);
	/* end of central directory: within the last 64 KiB + 22 bytes */
	long tailLen = fileSize < 65557 ? fileSize : 65557;
	uint8_t* tail = malloc((size_t) tailLen);
	if (!tail) {
		fclose(f);
		return false;
	}
	fseek(f, fileSize - tailLen, SEEK_SET);
	if (fread(tail, 1, (size_t) tailLen, f) != (size_t) tailLen) {
		free(tail);
		fclose(f);
		return false;
	}
	long eocd = -1;
	for (long i = tailLen - 22; i >= 0; --i) {
		if (rd32(tail + i) == 0x06054b50) {
			eocd = i;
			break;
		}
	}
	if (eocd < 0) {
		free(tail);
		fclose(f);
		snprintf(why, whyLen, "not a zip file (or a damaged one)");
		return false;
	}
	uint16_t count = rd16(tail + eocd + 10);
	uint32_t cdSize = rd32(tail + eocd + 12), cdOff = rd32(tail + eocd + 16);
	free(tail);
	uint8_t* cd = malloc(cdSize);
	if (!cd || fseek(f, (long) cdOff, SEEK_SET) || fread(cd, 1, cdSize, f) != cdSize) {
		free(cd);
		fclose(f);
		snprintf(why, whyLen, "couldn't read the zip's directory");
		return false;
	}
	/* choose: first file with a wanted extension, else the largest */
	long pick = -1, biggest = -1;
	uint32_t biggestSize = 0;
	uint32_t p = 0;
	for (uint16_t i = 0; i < count && p + 46 <= cdSize; ++i) {
		if (rd32(cd + p) != 0x02014b50) {
			break;
		}
		uint32_t usize = rd32(cd + p + 24);
		uint16_t nlen = rd16(cd + p + 28), xlen = rd16(cd + p + 30), clen = rd16(cd + p + 32);
		char nm[260];
		size_t k = nlen < sizeof(nm) - 1 ? nlen : sizeof(nm) - 1;
		memcpy(nm, cd + p + 46, k);
		nm[k] = 0;
		bool isDir = k && nm[k - 1] == '/';
		if (!isDir) {
			if (pick < 0 && extOk(nm, exts)) {
				pick = p;
			}
			if (usize > biggestSize) {
				biggestSize = usize;
				biggest = p;
			}
		}
		p += 46 + nlen + xlen + clen;
	}
	if (pick < 0) {
		pick = biggest;
	}
	if (pick < 0) {
		free(cd);
		fclose(f);
		snprintf(why, whyLen, "the zip is empty");
		return false;
	}
	uint16_t method = rd16(cd + pick + 10);
	uint32_t csize = rd32(cd + pick + 20), usize = rd32(cd + pick + 24), local = rd32(cd + pick + 42);
	uint16_t nlen = rd16(cd + pick + 28);
	size_t k = nlen < nameLen - 1 ? nlen : nameLen - 1;
	memcpy(name, cd + pick + 46, k);
	name[k] = 0;
	free(cd);
	uint8_t lh[30];
	if (fseek(f, (long) local, SEEK_SET) || fread(lh, 1, 30, f) != 30 || rd32(lh) != 0x04034b50) {
		fclose(f);
		snprintf(why, whyLen, "damaged zip");
		return false;
	}
	fseek(f, (long) local + 30 + rd16(lh + 26) + rd16(lh + 28), SEEK_SET);
	if (method != 0 && method != 8) {
		fclose(f);
		snprintf(why, whyLen, "this zip uses a compression method Lingo can't open (only Deflate); re-zip it or unzip it");
		return false;
	}
	uint8_t* out = malloc(usize ? usize : 1);
	uint8_t* in = malloc(csize ? csize : 1);
	if (!out || !in || fread(in, 1, csize, f) != csize) {
		free(out);
		free(in);
		fclose(f);
		snprintf(why, whyLen, "not enough memory to unzip this game");
		return false;
	}
	fclose(f);
	bool ok = true;
	if (method == 0) {
		memcpy(out, in, usize < csize ? usize : csize);
	} else {
		z_stream z;
		memset(&z, 0, sizeof(z));
		z.next_in = in;
		z.avail_in = csize;
		z.next_out = out;
		z.avail_out = usize;
		ok = inflateInit2(&z, -MAX_WBITS) == Z_OK;
		if (ok) {
			int r = inflate(&z, Z_FINISH);
			ok = r == Z_STREAM_END && z.total_out == usize;
			inflateEnd(&z);
		}
	}
	free(in);
	if (!ok) {
		free(out);
		snprintf(why, whyLen, "the zip is damaged");
		return false;
	}
	*outData = out;
	*outSize = usize;
	return true;
}
