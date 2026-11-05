#!/usr/bin/env python3
"""计算转写与参考答案的编辑距离错误率 (中文按字 CER / 英文按词 WER)。

用法:
    python tools/wer.py ref.txt hyp.txt [--diff N]
"""
import argparse
import difflib
import re
import sys
import unicodedata

CJK = re.compile(r"[\u3400-\u4dbf\u4e00-\u9fff\u3040-\u30ff\uac00-\ud7af]")
TOKEN = re.compile(r"[\u3400-\u4dbf\u4e00-\u9fff\u3040-\u30ff\uac00-\ud7af]|[0-9]+|[A-Za-z]+")


def normalize(text: str) -> str:
    text = unicodedata.normalize("NFKC", text).lower()
    text = re.sub(r"[^\w\u3400-\u4dbf\u4e00-\u9fff\u3040-\u30ff\uac00-\ud7af]+", " ", text)
    return " ".join(text.split())


def tokenize(text: str):
    return TOKEN.findall(normalize(text))


def edit_distance(a, b) -> int:
    if not a:
        return len(b)
    if not b:
        return len(a)
    prev = list(range(len(b) + 1))
    for i, ca in enumerate(a, 1):
        cur = [i]
        for j, cb in enumerate(b, 1):
            cur.append(min(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (ca != cb)))
        prev = cur
    return prev[-1]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("ref")
    ap.add_argument("hyp")
    ap.add_argument("--diff", type=int, default=3, help="打印若干处不一致的片段")
    args = ap.parse_args()

    ref = open(args.ref, encoding="utf-8-sig").read()
    hyp = open(args.hyp, encoding="utf-8-sig").read()

    r = tokenize(ref)
    h = tokenize(hyp)
    if not r:
        print("参考文本为空，无法比较")
        return 2

    d = edit_distance(r, h)
    err = 100.0 * d / len(r)
    kind = "CER(mixed char/word)" if CJK.search(ref) else "WER(word)"
    print(f"unit: {kind}")
    print(f"ref_units: {len(r)}  hyp_units: {len(h)}")
    print(f"edit_distance: {d}")
    print(f"ERROR_RATE: {err:.2f}%   ACCURACY: {100.0 - err:.2f}%")

    if args.diff > 0:
        sm = difflib.SequenceMatcher(a=r, b=h, autojunk=False)
        shown = 0
        for tag, i1, i2, j1, j2 in sm.get_opcodes():
            if tag == "equal":
                continue
            if shown >= args.diff:
                break
            ref_part = " ".join(r[i1:i2]) or "(空)"
            hyp_part = " ".join(h[j1:j2]) or "(空)"
            print(f"[{tag}] 参考: {ref_part}  ->  实际: {hyp_part}")
            shown += 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
