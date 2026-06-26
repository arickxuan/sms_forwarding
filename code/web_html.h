#ifndef WEB_HTML_H
#define WEB_HTML_H

#include <Arduino.h>

// gzip 压缩后的主页面（由 code/web_page_src.html 生成，见 gen_web_html.py）
extern const uint8_t htmlPageGz[];
extern const unsigned htmlPageGzLen;

#endif
