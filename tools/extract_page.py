import re
import sys

src = open(sys.argv[1], encoding='utf-8').read()
start = src.index('static const char INDEX_HTML')
end = src.index('</html>";', start) + len('</html>";')
block = src[start:end]

html = ''.join(re.findall(r'"((?:[^"\\]|\\.)*)"', block, re.S))
# Снимаем экранирование уровня C
html = html.replace('\\"', '"').replace('\\n', '\n').replace('\\\\', '\\')

out = sys.argv[2] if len(sys.argv) > 2 else 'page.html'
open(out, 'w', encoding='utf-8').write(html)
print('записано в', out, len(html), 'байт')

# Достаём тело скрипта для отдельной проверки
m = re.search(r'<script>(.*?)</script>', html, re.S)
if m:
    js = m.group(1)
    open(out + '.js', 'w', encoding='utf-8').write(js)
    print('скрипт:', len(js), 'байт ->', out + '.js')
