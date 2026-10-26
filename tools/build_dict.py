#!/usr/bin/env python3
"""Builds lingo.dic, the compact offline dictionary the Vita app reads.

Source: JMdict (EDRDG, CC BY-SA 4.0) as packaged in jamdict-data's SQLite
database. Every kana reading becomes a lookup key (katakana folded to
hiragana, since games write the same word either way), pointing at its
entries ranked common-first.

Usage: build_dict.py jamdict.db lingo.dic
"""
import sqlite3
import struct
import sys
from collections import defaultdict

POS_V1, POS_V5, POS_ADJI, POS_VK, POS_VS, POS_NOUN, POS_PRT, POS_EXP, POS_ADV, POS_ADJNA, POS_OTHER = (
    1 << i for i in range(11))

COMMON = {"news1", "ichi1", "spec1", "spec2", "gai1"}
SEMI = {"news2", "ichi2", "gai2"}
MAX_SENSES = 3
# Ties that games settle one way: きみ is nearly always 君 ("you"), not 気味.
PREFER = {("きみ", "君"), ("おれ", "俺"), ("ぼく", "僕"), ("あなた", "貴方")}
MAX_GLOSSES = 3
MAX_GLOSS_CHARS = 150


def pos_bits(texts):
    bits = 0
    for t in texts:
        if t.startswith("Ichidan verb"):
            bits |= POS_V1
        elif t.startswith("Godan verb"):
            bits |= POS_V5
        elif t.startswith("adjective (keiyoushi)"):
            bits |= POS_ADJI
        elif t.startswith("Kuru verb"):
            bits |= POS_VK
        elif t.startswith("suru verb") or t.startswith("noun or participle which takes the aux. verb suru"):
            bits |= POS_VS
        elif t.startswith("noun") or t == "pronoun" or t == "counter" or t == "numeric":
            bits |= POS_NOUN
        elif t == "particle":
            bits |= POS_PRT
        elif t.startswith("expressions"):
            bits |= POS_EXP
        elif t.startswith("adverb"):
            bits |= POS_ADV
        elif t.startswith("adjectival nouns"):
            bits |= POS_ADJNA
        else:
            bits |= POS_OTHER
    return bits


def to_hira(s):
    return "".join(chr(ord(c) - 0x60) if 0x30A1 <= ord(c) <= 0x30F6 else c for c in s)


def short_pos(texts):
    for t in texts:
        if t.startswith("Ichidan"):
            return "ru-verb"
        if t.startswith("Godan"):
            return "u-verb"
        if t.startswith("Kuru"):
            return "irregular verb"
        if t.startswith("suru verb"):
            return "suru verb"
        if t.startswith("adjective (keiyoushi)"):
            return "i-adjective"
        if t.startswith("adjectival nouns"):
            return "na-adjective"
        if t == "particle":
            return "particle"
        if t.startswith("adverb"):
            return "adverb"
        if t.startswith("interjection"):
            return "interjection"
        if t.startswith("expressions"):
            return "expression"
        if t.startswith("noun"):
            return "noun"
    return ""


def main(db_path, out_path):
    db = sqlite3.connect(db_path)
    c = db.cursor()
    print("reading kana...", flush=True)
    kana = defaultdict(list)       # idseq -> [(kana id, text)]
    for kid, idseq, text in c.execute("SELECT ID, idseq, text FROM Kana ORDER BY ID"):
        kana[idseq].append((kid, text))
    pri = defaultdict(set)
    for kid, text in c.execute("SELECT kid, text FROM KNP"):
        pri[kid].add(text)
    for kid, text in c.execute("SELECT k.idseq, p.text FROM KJP p JOIN Kanji k ON k.ID = p.kid"):
        pri[("e", kid)].add(text)
    kanji = defaultdict(list)
    for idseq, text in c.execute("SELECT idseq, text FROM Kanji ORDER BY ID"):
        kanji[idseq].append(text)
    print("reading senses...", flush=True)
    senses = defaultdict(list)     # idseq -> [sense id]
    for sid, idseq in c.execute("SELECT ID, idseq FROM Sense ORDER BY ID"):
        senses[idseq].append(sid)
    glosses = defaultdict(list)
    for sid, text in c.execute("SELECT sid, text FROM SenseGloss WHERE lang='eng' OR lang IS NULL OR lang=''"):
        glosses[sid].append(text)
    pos = defaultdict(list)
    for sid, text in c.execute("SELECT sid, text FROM pos"):
        pos[sid].append(text)
    misc = defaultdict(list)
    for sid, text in c.execute("SELECT sid, text FROM misc"):
        misc[sid].append(text)

    print("assembling...", flush=True)
    entries = []                   # (head, reading, gloss, posbits, rank)
    keys = defaultdict(list)       # hira key -> [(rank, entry index)]
    for idseq in sorted(kana):
        sids = [s for s in senses.get(idseq, []) if glosses.get(s)]
        if not sids:
            continue
        all_pos = []
        for s in sids:
            all_pos.extend(pos.get(s, []))
        # senses inherit the previous part of speech when they list none
        bits = pos_bits(all_pos)
        usually_kana = any("usually written using kana alone" in m for s in sids for m in misc.get(s, []))
        parts = []
        for n, s in enumerate(sids[:MAX_SENSES], 1):
            g = "; ".join(glosses[s][:MAX_GLOSSES])
            parts.append(f"{n}. {g}" if len(sids) > 1 else g)
        gloss = "  ".join(parts)
        if len(gloss) > MAX_GLOSS_CHARS:
            gloss = gloss[:MAX_GLOSS_CHARS - 1].rstrip() + "…"
        label = short_pos(all_pos)
        if label:
            gloss = f"[{label}] {gloss}"
        def prank(p):
            # ichi1 marks everyday vocabulary, which is what games use; newspaper
            # frequency alone (news1) favours words like 京 over 今日
            if "ichi1" in p:
                return 0
            if p & {"spec1", "spec2"}:
                return 0.5
            if p & {"news1", "gai1"}:
                return 1
            if p & SEMI or any(x.startswith("nf") for x in p):
                return 1.5
            return 2
        kanji_p = pri.get(("e", idseq), set())
        rank_e = 3
        for kid, text in kana[idseq]:
            rank_e = min(rank_e, prank(pri.get(kid, set()) | kanji_p))
        boost = 0.0
        if bits & POS_PRT:
            boost -= 2.0   # particles are what short kana spans usually are
        if usually_kana:
            boost -= 0.5   # games write these in kana
        if not (bits & (POS_V1 | POS_V5 | POS_ADJI | POS_VK | POS_VS | POS_PRT)) and len(kana[idseq][0][1]) == 1:
            boost += 1.0   # one-kana nouns (歯, 木, 手) rarely beat the particle reading
        reading = kana[idseq][0][1]
        head = reading if (usually_kana or not kanji.get(idseq)) else kanji[idseq][0]
        idx = len(entries)
        entry_rank = max(0, int((rank_e + boost + 2) * 2))
        entries.append((head, reading, gloss, bits, entry_rank))
        seen = set()
        for kid, text in kana[idseq]:
            k = to_hira(text)
            if k in seen:
                continue
            seen.add(k)
            # priority belongs to this reading; a kanji's tags only count when
            # the entry has no other reading they could belong to
            own = pri.get(kid, set())
            r = prank(own | (kanji_p if len(kana[idseq]) == 1 else set())) + boost
            if any((k, kj) in PREFER for kj in kanji.get(idseq, [])):
                r -= 0.5
            keys[k].append((r, idseq, idx))

    print(f"{len(entries)} entries, {len(keys)} keys", flush=True)

    strings = bytearray()
    cache = {}

    def put(s):
        if s in cache:
            return cache[s]
        off = len(strings)
        strings.extend(s.encode("utf-8") + b"\0")
        cache[s] = off
        return off

    entry_bin = bytearray()
    for head, reading, gloss, bits, rank in entries:
        entry_bin += struct.pack("<IIIHH", put(head), put(reading), put(gloss), bits, rank)

    sorted_keys = sorted(keys, key=lambda k: k.encode("utf-8"))
    key_bin = bytearray()
    ids_bin = bytearray()
    nids = 0
    for k in sorted_keys:
        lst = sorted(keys[k])[:8]
        key_bin += struct.pack("<IIHH", put(k), nids, len(lst), 0)
        for r, _, idx in lst:
            level = max(0, min(15, int((r + 3) * 2)))
            ids_bin += struct.pack("<I", idx | (level << 28))  # top 4 bits: how common this reading is
            nids += 1

    header_size = 4 * 8
    off_entries = header_size
    off_keys = off_entries + len(entry_bin)
    off_ids = off_keys + len(key_bin)
    off_strings = off_ids + len(ids_bin)
    header = b"LDIC" + struct.pack("<IIIIIII", 2, len(entries), len(sorted_keys), off_entries, off_keys,
                                   off_ids, off_strings)
    assert len(header) == header_size
    with open(out_path, "wb") as f:
        f.write(header)
        f.write(entry_bin)
        f.write(key_bin)
        f.write(ids_bin)
        f.write(strings)
    total = off_strings + len(strings)
    print(f"wrote {out_path}: {total / 1e6:.1f} MB", flush=True)


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
