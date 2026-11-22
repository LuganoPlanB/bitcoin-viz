# SPDX-License-Identifier: GPL-3.0-or-later
"""Independent RIFF/JPEG inspection and FFmpeg consumption of writer fixtures."""
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def run(command):
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30)
    check(result.returncode == 0, f"{command}: {result.stderr.decode(errors='replace')}")
    return result.stdout


def inspect(path, frames):
    data = path.read_bytes()
    u16 = lambda offset: struct.unpack_from('<H', data, offset)[0]
    u32 = lambda offset: struct.unpack_from('<I', data, offset)[0]
    u64 = lambda offset: struct.unpack_from('<Q', data, offset)[0]
    chunks, images, segments = [], [], []

    def walk(begin, end, segment, movi=None):
        offset = begin
        while offset < end:
            check(offset % 2 == 0 and offset + 8 <= end, f"unaligned/truncated chunk at {offset}")
            tag, length = data[offset:offset+4], u32(offset+4)
            stop = offset + 8 + length
            check(stop <= end, f"{tag} overruns parent at {offset}")
            chunks.append((tag, offset, length, segment, movi))
            if tag in (b'RIFF', b'LIST'):
                kind = data[offset+8:offset+12]
                check(length >= 4, 'short container')
                if tag == b'RIFF':
                    check(kind in (b'AVI ', b'AVIX'), 'wrong RIFF kind')
                    segment = len(segments)
                    segments.append(offset)
                walk(offset+12, stop, segment, offset+8 if kind == b'movi' else movi)
            elif tag == b'00dc':
                check(movi is not None, 'video outside movi')
                images.append((offset, length, segment, movi))
                check(data[stop-2:stop] == b'\xff\xd9', 'chunk size includes padding or truncated JPEG')
            if length & 1:
                check(stop < end and data[stop] == 0, 'missing external zero padding')
            offset = stop + (length & 1)
        check(offset == end, 'container size mismatch')

    walk(0, len(data), -1)
    check(len(images) == frames, f'{path}: {len(images)} physical frames')
    image_map = {offset: length for offset, length, _, _ in images}
    standard, super_entries, legacy = [], [], []
    for tag, offset, length, segment, movi in chunks:
        p = offset + 8
        if tag == b'strh':
            check(data[p:p+8] == b'vidsMJPG', 'stream type/handler')
            check((u32(p+20), u32(p+24), u32(p+32)) == (1, 30, frames), 'rate or stream length')
        elif tag == b'strf':
            check((u32(p+4), u32(p+8)) == (161, 95), 'bitmap dimensions')
            check(data[p+16:p+20] == b'MJPG', 'bitmap compression')
        elif tag == b'idx1':
            check(length % 16 == 0, 'legacy index size')
            first_movi = images[0][3] if images else None
            for entry in range(p, p+length, 16):
                check(data[entry:entry+4] == b'00dc' and u32(entry+4) == 16, 'legacy keyframe')
                target = first_movi + u32(entry+8)
                check(image_map.get(target) == u32(entry+12), 'legacy index offset/actual size')
                legacy.append(target)
        elif tag == b'ix00':
            check(u16(p) == 2 and data[p+3] == 1 and data[p+8:p+12] == b'00dc', 'standard index format')
            entries = u32(p+4)
            check(length == 24 + entries * 8, 'standard index size')
            base = u64(p+12)
            for entry in range(p+24, p+24+entries*8, 8):
                target = base + u32(entry) - 8
                check(image_map.get(target) == u32(entry+4), 'standard offset/actual size')
                standard.append(target)
        elif tag == b'indx':
            count = u32(p+4)
            check(u16(p) == 4 and data[p+3] == 0 and data[p+8:p+12] == b'00dc', 'superindex format')
            check(24 + count * 16 <= length, 'superindex capacity overrun')
            for entry in range(p+24, p+24+count*16, 16):
                target, size, duration = u64(entry), u32(entry+8), u32(entry+12)
                check(data[target:target+4] == b'ix00' and u32(target+4)+8 == size, 'superindex target/size')
                check(u32(target+12) == duration, 'superindex duration')
                super_entries.append((target, duration))
        elif tag == b'dmlh':
            check(u32(p) == frames, 'OpenDML total frame count')
    check(sorted(standard) == sorted(image_map), 'standard indexes cover each frame once')
    check(sum(duration for _, duration in super_entries) == frames, 'superindex frame coverage')
    check(len(set(legacy)) == len(legacy), 'duplicate legacy entry')
    return data, images, segments


def jpeg_tables(data):
    check(data[:2] == b'\xff\xd8', 'JPEG SOI')
    offset, tables, sampling = 2, {}, None
    while offset < len(data):
        check(data[offset] == 255, 'JPEG marker alignment')
        marker = data[offset+1]
        if marker == 0xda:
            break
        length = int.from_bytes(data[offset+2:offset+4], 'big')
        payload = data[offset+4:offset+2+length]
        if marker == 0xc0:
            check(payload[:6] == bytes([8, 0, 95, 0, 161, 3]), 'JPEG dimensions/components')
            sampling = [payload[7], payload[10], payload[13]]
        elif marker == 0xdb:
            at = 0
            while at < len(payload):
                check(payload[at] >> 4 == 0, 'unexpected 16-bit quantization')
                tables[payload[at] & 15] = list(payload[at+1:at+65])
                at += 65
        offset += length + 2
    check(sampling == [0x11, 0x11, 0x11], 'JPEG must use 4:4:4 H1V1')
    return tables


def decoded_frame(frame, number):
    def rgb(x, y):
        start = (y * 161 + x) * 3
        return tuple(frame[start:start+3])
    top, bottom = rgb(40, 10), rgb(40, 85)
    check(top[0] > 180 and top[2] < 70 and bottom[2] > 180 and bottom[0] < 70,
          f'orientation/colors at frame {number}: {top}/{bottom}')
    check(abs(rgb(75, 20)[1] - (40 + number * 2)) <= 12, f'frame order {number}')
    check(min(rgb(11, 12)) >= 180, f'text glyph at frame {number}')


def main():
    executable, ffprobe, ffmpeg, output = sys.argv[1:]
    directory = Path(output)
    if directory.exists():
        shutil.rmtree(directory)
    directory.mkdir(parents=True)
    print(run([executable, str(directory)]).decode(), end='')
    inspect(directory / 'empty.avi', 0)
    inspect(directory / 'single.avi', 1)
    single = run([ffmpeg, '-v', 'error', '-xerror', '-i', str(directory / 'single.avi'),
                  '-pix_fmt', 'rgb24', '-f', 'rawvideo', '-'])
    check(len(single) == 161*95*3, 'single frame independent decode')
    decoded_frame(single, 0)
    tables_by_name = {}
    for name in ('normal', 'rollover', 'quality'):
        path = directory / (name + '.avi')
        data, images, segments = inspect(path, 90)
        sizes = [length for _, length, _, _ in images]
        check(len(set(sizes)) > 10 and any(size & 1 for size in sizes), 'variable/odd compressed sizes')
        tables = [jpeg_tables(data[offset+8:offset+8+length]) for offset, length, _, _ in images]
        check(all(table == tables[0] for table in tables), 'stable quality tables')
        tables_by_name[name] = tables[0]
        streams = json.loads(run([ffprobe, '-v', 'error', '-count_frames', '-show_streams', '-of', 'json', str(path)]))['streams']
        check(len(streams) == 1, 'one video stream')
        stream = streams[0]
        check((stream['codec_name'], stream['codec_tag_string'], stream['width'], stream['height'],
               stream['r_frame_rate'], stream['nb_read_frames']) == ('mjpeg', 'MJPG', 161, 95, '30/1', '90'), 'independent stream properties')
        run([ffmpeg, '-v', 'error', '-xerror', '-i', str(path), '-f', 'null', '-'])
        raw = run([ffmpeg, '-v', 'error', '-xerror', '-i', str(path), '-pix_fmt', 'rgb24', '-f', 'rawvideo', '-'])
        frame_bytes = 161 * 95 * 3
        check(len(raw) == 90 * frame_bytes, 'independent raw frame count')
        for number in range(90):
            decoded_frame(raw[number*frame_bytes:(number+1)*frame_bytes], number)
        seek_frames = {89}
        if name == 'rollover':
            check(len(segments) >= 3, 'reduced-threshold OpenDML rollover')
            boundary = next(i for i in range(1, 90) if images[i][2] != images[i-1][2])
            seek_frames.update((boundary-1, boundary))
        for number in sorted(seek_frames):
            raw = run([ffmpeg, '-v', 'error', '-xerror', '-ss', f'{max(0, number-0.1)/30:.8f}',
                       '-i', str(path), '-frames:v', '1', '-pix_fmt', 'rgb24', '-f', 'rawvideo', '-'])
            check(len(raw) == frame_bytes, f'seek frame {number}')
            decoded_frame(raw, number)
        print(f'{name}: 90 decoded frames; {len(segments)} RIFF segments; {sum(size & 1 for size in sizes)} odd chunks; seeks {sorted(seek_frames)}')
    luma = [16,11,12,14,12,10,16,14,13,14,18,17,16,19,24,40,26,24,22,22,24,49,35,37,29,40,58,51,61,60,57,51,56,55,64,72,92,78,64,68,87,69,55,56,80,109,81,87,95,98,103,104,103,62,77,113,121,112,100,120,92,101,103,99]
    chroma = [17,18,18,24,21,24,47,26,26,47,99,66,56,66] + [99]*50
    expected = {0: [(value*10+50)//100 for value in luma], 1: [(value*10+50)//100 for value in chroma]}
    check(tables_by_name['normal'] == expected == tables_by_name['rollover'], 'default quality-95 tables')
    check(tables_by_name['quality'] != expected, 'selected quality changes encoding')
    print('RIFF/index/JPEG contracts passed; 271 independently decoded frames, 5 seek checks')


if __name__ == '__main__':
    main()
