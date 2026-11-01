#!/usr/bin/env python3
"""Download one day of Nasdaq TotalView-ITCH 5.0, check it and decompress it.

    python3 tools/download_itch.py 12302019

Downloads data/raw/<day>.NASDAQ_ITCH50.gz in parallel byte ranges (one
connection to the Nasdaq server is slow), resumes if interrupted, checks the
file and writes the decompressed day to data/itch/<day>.NASDAQ_ITCH50.
Standard library only.
"""
import argparse
import hashlib
import json
import os
import queue
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request

BASE_URL = "https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/"
PART_BYTES = 64 * 1024 * 1024


def content_length(url):
    request = urllib.request.Request(url, method="HEAD")
    with urllib.request.urlopen(request, timeout=60) as response:
        return int(response.headers["Content-Length"])


def fetch_part(url, fd, start, end):
    """Download bytes [start, end] into the open file at the same offset.
    Retries from where it stopped if the connection drops."""
    position = start
    attempts = 0
    while position <= end:
        request = urllib.request.Request(url, headers={"Range": f"bytes={position}-{end}"})
        try:
            with urllib.request.urlopen(request, timeout=60) as response:
                if response.status != 206:
                    raise RuntimeError(f"server ignored the range request (HTTP {response.status})")
                while True:
                    chunk = response.read(1 << 20)
                    if not chunk:
                        break
                    os.pwrite(fd, chunk, position)
                    position += len(chunk)
        except (urllib.error.URLError, TimeoutError, ConnectionError, OSError) as error:
            attempts += 1
            if attempts > 20:
                raise
            print(f"  retrying bytes {position}-{end}: {error}", file=sys.stderr)
            time.sleep(min(30, 2 * attempts))


def download(url, path, workers):
    size = content_length(url)
    progress_path = path + ".parts.json"
    done = set()
    if os.path.exists(progress_path) and os.path.exists(path):
        with open(progress_path) as f:
            done = set(json.load(f))

    parts = [(i, i * PART_BYTES, min(size, (i + 1) * PART_BYTES) - 1)
             for i in range((size + PART_BYTES - 1) // PART_BYTES)]
    todo = queue.Queue()
    for part in parts:
        if part[0] not in done:
            todo.put(part)
    print(f"{os.path.basename(path)}: {size:,} bytes, {len(parts)} parts, {len(done)} already done")

    fd = os.open(path, os.O_RDWR | os.O_CREAT, 0o644)
    os.ftruncate(fd, size)
    lock = threading.Lock()
    errors = []
    started = time.time()

    def worker():
        while True:
            try:
                index, start, end = todo.get_nowait()
            except queue.Empty:
                return
            try:
                fetch_part(url, fd, start, end)
            except Exception as error:  # noqa: BLE001 - report and stop this worker
                errors.append(error)
                return
            with lock:
                done.add(index)
                with open(progress_path, "w") as f:
                    json.dump(sorted(done), f)
                mb = len(done) * PART_BYTES / 1e6
                print(f"  {len(done)}/{len(parts)} parts, {mb / (time.time() - started):.1f} MB/s")

    threads = [threading.Thread(target=worker) for _ in range(workers)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    os.close(fd)
    if errors or len(done) != len(parts):
        raise SystemExit(f"download incomplete: {errors[:3]}")
    os.remove(progress_path)
    return size


def md5_of(path):
    digest = hashlib.md5()
    with open(path, "rb") as f:
        while chunk := f.read(1 << 24):
            digest.update(chunk)
    return digest.hexdigest()


def nasdaq_md5(url):
    """Nasdaq lists a .md5sum next to each file, but in Nov 2026 the server
    answered 404 for them. Return the published hash if it can be fetched."""
    try:
        with urllib.request.urlopen(url + ".md5sum", timeout=60) as response:
            return response.read().decode().split()[0].lower()
    except (urllib.error.URLError, IndexError, UnicodeDecodeError):
        return None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("day", help="MMDDYYYY, for example 12302019")
    parser.add_argument("--workers", type=int, default=8)
    parser.add_argument("--data-dir", default=os.path.join(os.path.dirname(__file__), "..", "data"))
    args = parser.parse_args()

    name = f"{args.day}.NASDAQ_ITCH50"
    raw_dir = os.path.join(args.data_dir, "raw")
    itch_dir = os.path.join(args.data_dir, "itch")
    os.makedirs(raw_dir, exist_ok=True)
    os.makedirs(itch_dir, exist_ok=True)
    gz_path = os.path.join(raw_dir, name + ".gz")
    out_path = os.path.join(itch_dir, name)
    url = BASE_URL + name + ".gz"

    if not os.path.exists(gz_path) or os.path.exists(gz_path + ".parts.json"):
        download(url, gz_path, args.workers)

    print("md5 ...")
    md5 = md5_of(gz_path)
    with open(gz_path + ".md5", "w") as f:
        f.write(f"{md5}  {name}.gz\n")
    published = nasdaq_md5(url)
    if published is None:
        print(f"  md5 {md5} (Nasdaq's .md5sum could not be fetched; relying on gzip's CRC-32)")
    elif published != md5:
        raise SystemExit(f"md5 mismatch: file {md5}, Nasdaq {published}")
    else:
        print(f"  md5 {md5} matches Nasdaq's .md5sum")

    if not os.path.exists(out_path):
        # gzip checks the CRC-32 and length stored in the file while it decompresses
        print("decompressing and checking the gzip CRC-32 ...")
        tmp_path = out_path + ".tmp"
        with open(tmp_path, "wb") as out:
            subprocess.run(["gzip", "-dc", gz_path], stdout=out, check=True)
        os.rename(tmp_path, out_path)
    print(f"done: {out_path} ({os.path.getsize(out_path):,} bytes)")


if __name__ == "__main__":
    main()
