#!/usr/bin/env python3
# 由 code/web_page_src.html 生成 code/web_html.cpp（gzip 后的字节数组）。
# 改完 HTML 后运行：  python3 gen_web_html.py
import gzip, io, os

ROOT = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(ROOT, 'code', 'web_page_src.html')
OUT = os.path.join(ROOT, 'code', 'web_html.cpp')

html = open(SRC, 'rb').read()
buf = io.BytesIO()
with gzip.GzipFile(fileobj=buf, mode='wb', compresslevel=9, mtime=0) as f:
    f.write(html)
gz = buf.getvalue()

lines = ['// 【自动生成，请勿手改】源文件 code/web_page_src.html，gzip 后内嵌。',
         '// 重新生成： python3 gen_web_html.py',
         '#include "web_html.h"',
         '',
         'const unsigned htmlPageGzLen = %d;' % len(gz),
         'const uint8_t htmlPageGz[] PROGMEM = {']
for i in range(0, len(gz), 16):
    chunk = gz[i:i+16]
    lines.append('  ' + ','.join('0x%02x' % b for b in chunk) + ',')
lines.append('};')
open(OUT, 'w', encoding='utf-8').write('\n'.join(lines) + '\n')

print('源 HTML %d 字节 -> gzip %d 字节 (%.1f%%)' % (len(html), len(gz), 100*len(gz)/len(html)))
print('已写出', OUT)
